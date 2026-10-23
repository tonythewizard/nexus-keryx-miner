#!/usr/bin/env python3
import io
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "scripts"))
from m233_build_guard import (
    CGROUP_MAX_BYTES, CGROUP_HIGH_BYTES, build_abort_reason, log_host_telemetry,
    memory_metrics, parse_pressure, stall_rate,
)

WORK = pathlib.Path(__file__).resolve().parents[1] / "work" / "tmp"
WORK.mkdir(parents=True, exist_ok=True)
assert WORK.resolve().is_relative_to(pathlib.Path(__file__).resolve().parents[1])
assert CGROUP_HIGH_BYTES < CGROUP_MAX_BYTES

with tempfile.TemporaryDirectory(prefix="m233-cgroup-", dir=WORK) as raw:
    cg = pathlib.Path(raw)
    values = {
        "memory.current": "1234\n", "memory.peak": "2345\n",
        "memory.max": str(CGROUP_MAX_BYTES) + "\n",
        "memory.high": str(CGROUP_HIGH_BYTES) + "\n",
        "memory.swap.current": "9876\n", "memory.swap.max": "0\n",
        "memory.events": "low 0\nhigh 2\nmax 3\noom 0\noom_kill 0\n",
        "memory.pressure": "some avg10=0.00 avg60=0.00 avg300=0.00 total=100\n"
                           "full avg10=0.00 avg60=0.00 avg300=0.00 total=50\n",
    }
    for name, content in values.items():
        (cg / name).write_text(content)
    observed = memory_metrics(cg)
    assert observed["current"] == 1234 and observed["peak"] == 2345
    assert observed["event_high"] == 2 and observed["event_max"] == 3
    assert observed["event_oom"] == 0 and observed["event_oom_kill"] == 0
    assert observed["swap_current"] == 9876
    assert observed["pressure_telemetry"]["full"]["total"] == 50
    assert observed["pressure_telemetry"]["some"]["avg10"] == 0.0
    (cg / "memory.swap.max").write_text("max\n")
    try:
        memory_metrics(cg)
    except RuntimeError as exc:
        assert "swap is not disabled" in str(exc)
    else:
        raise AssertionError("build cgroup without the configured hard swap limit must be rejected")

events = {"event_oom": 0, "event_oom_kill": 0, "event_max": 0, "event_high": 0}
# Host PSI, RAM and swap values are deliberately absent from the stop decision.
assert build_abort_reason(events, events) is None
host = {
    "memory": {"MemAvailable": 1, "SwapFree": 1, "pswpin": 900_000, "pswpout": 9_000_000},
    "psi": parse_pressure("some avg10=99 avg60=99 avg300=99 total=9000000\n"
                           "full avg10=99 avg60=99 avg300=99 total=8000000\n"),
    "sample_monotonic": 10.0,
}
previous = {
    "memory": {"MemAvailable": 2_000_000, "SwapFree": 8_000_000,
               "pswpin": 1, "pswpout": 1},
    "psi": parse_pressure("some avg10=0 avg60=0 avg300=0 total=1\n"
                           "full avg10=0 avg60=0 avg300=0 total=1\n"),
    "sample_monotonic": 9.0,
}
log = io.StringIO()
log_host_telemetry(log, "during-build", host, previous)
record = log.getvalue()
assert '"MemAvailable_kib": 1' in record
assert '"SwapFree_kib": 1' in record
assert '"swapout_pages_per_second": 8999999.0' in record
assert '"host_full_stall_rate": 7.999999' in record
assert build_abort_reason(events, events) is None

# Only actual OOM events abort; high/max counters and host-pressure telemetry do not.
assert build_abort_reason({**events, "event_high": 4, "event_max": 2}, events) is None
assert "OOM" in build_abort_reason({**events, "event_oom": 1}, events)
assert "OOM" in build_abort_reason({**events, "event_oom_kill": 1}, events)

before = parse_pressure("some avg10=0.1 avg60=0.2 avg300=0.3 total=100\n"
                        "full avg10=0.0 avg60=0.0 avg300=0.0 total=10\n")
after = parse_pressure("some avg10=0.1 avg60=0.2 avg300=0.3 total=100\n"
                       "full avg10=0.1 avg60=0.1 avg300=0.1 total=510\n")
assert stall_rate(before, after, 2.0) == 0.00025
try:
    parse_pressure("full avg10=broken avg60=0 avg300=0 total=1\n")
except RuntimeError:
    pass
else:
    raise AssertionError("malformed telemetry must be labelled as a parse error")
print("M233_BUILD_GUARD_TESTS=PASS host_psi_ram_swap_are_telemetry_only cgroup_oom_only_stops")
