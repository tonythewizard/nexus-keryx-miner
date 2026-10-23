#!/usr/bin/env python3
"""Build one target in a serial, memory-limited transient user service.

Host PSI/RAM/swap are recorded as telemetry only. The build cgroup's hard
MemoryMax and actual OOM events provide compiler containment.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time
import uuid

from check_persistent_env import validate as validate_persistent_env
from m22_memory_budget import read_snapshot

MIB = 1024 * 1024
# The OS cgroup is the build's hard memory containment boundary.
CGROUP_MAX_BYTES = 768 * MIB
CGROUP_HIGH_BYTES = 640 * MIB
POLL_SECONDS = 1.0


def parse_key_values(path: pathlib.Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in path.read_text().splitlines():
        key, sep, raw = line.partition(" ")
        if not sep or not raw.isdigit():
            raise RuntimeError(f"invalid cgroup metric in {path}: {line!r}")
        result[key] = int(raw)
    return result


def parse_pressure(text: str) -> dict[str, dict[str, float | int]]:
    """Parse PSI averages and cumulative stall microseconds."""
    result: dict[str, dict[str, float | int]] = {}
    for line in text.splitlines():
        fields = line.split()
        if not fields or fields[0] not in ("some", "full"):
            continue
        values: dict[str, float | int] = {}
        for field in fields[1:]:
            key, sep, raw = field.partition("=")
            if not sep:
                raise RuntimeError(f"invalid PSI field: {field!r}")
            if key == "total":
                if not raw.isdigit():
                    raise RuntimeError(f"invalid PSI total: {raw!r}")
                values[key] = int(raw)
            else:
                try:
                    value = float(raw)
                except ValueError as exc:
                    raise RuntimeError(f"invalid PSI average: {raw!r}") from exc
                if not 0 <= value <= 100:
                    raise RuntimeError(f"PSI average out of range: {raw!r}")
                values[key] = value
        if not all(key in values for key in ("avg10", "avg60", "avg300", "total")):
            raise RuntimeError(f"incomplete PSI line: {line!r}")
        result[fields[0]] = values
    if not result:
        raise RuntimeError("PSI metrics unavailable")
    return result


def pressure_metrics(path: pathlib.Path) -> dict[str, dict[str, float | int]]:
    if not path.is_file():
        raise RuntimeError(f"required PSI metric unavailable: {path}")
    return parse_pressure(path.read_text())


def cgroup_process_metrics(cgroup: pathlib.Path) -> list[dict[str, int | str]]:
    """Read RSS/HWM only for processes currently inside the isolated build cgroup."""
    pids_path = cgroup / "cgroup.procs"
    if not pids_path.is_file():
        raise RuntimeError(f"build cgroup process list unavailable: {pids_path}")
    result = []
    for raw_pid in pids_path.read_text().splitlines():
        if not raw_pid.isdigit():
            raise RuntimeError(f"invalid PID in build cgroup.procs: {raw_pid!r}")
        pid = int(raw_pid)
        status_path = pathlib.Path("/proc") / str(pid) / "status"
        try:
            fields = {}
            for line in status_path.read_text().splitlines():
                key, sep, value = line.partition(":")
                if sep and key in ("Name", "VmRSS", "VmHWM"):
                    fields[key] = value.strip()
        except FileNotFoundError:
            # Processes can exit between cgroup.procs and /proc; they are no
            # longer contributing to the next cgroup memory.current sample.
            continue
        result.append({
            "pid": pid, "name": fields.get("Name", "unknown"),
            "rss_kib": int(fields.get("VmRSS", "0 kB").split()[0]),
            "hwm_kib": int(fields.get("VmHWM", "0 kB").split()[0]),
        })
    return result


def stall_rate(before: dict, after: dict, elapsed_seconds: float, kind: str = "full") -> float:
    """Return the fraction of wall time stalled from PSI total deltas."""
    if elapsed_seconds <= 0:
        raise ValueError("PSI sample interval must be positive")
    delta_us = int(after[kind]["total"]) - int(before[kind]["total"])
    if delta_us < 0:
        raise RuntimeError("PSI cumulative total moved backwards")
    return delta_us / (elapsed_seconds * 1_000_000)


def memory_metrics(cgroup: pathlib.Path) -> dict[str, int | dict | str]:
    required = ("memory.current", "memory.peak", "memory.max", "memory.high",
                "memory.swap.current", "memory.events")
    for item in required:
        if not (cgroup / item).is_file():
            raise RuntimeError(f"required cgroup-v2 metric unavailable: {cgroup / item}")
    max_value = (cgroup / "memory.max").read_text().strip()
    high_value = (cgroup / "memory.high").read_text().strip()
    if not max_value.isdigit() or not high_value.isdigit():
        raise RuntimeError("build cgroup memory.max/high is not a finite byte limit")
    swap_max = (cgroup / "memory.swap.max").read_text().strip()
    if swap_max != "0":
        raise RuntimeError(f"build cgroup swap is not disabled: memory.swap.max={swap_max}")
    events = parse_key_values(cgroup / "memory.events")
    result = {
        "current": int((cgroup / "memory.current").read_text().strip()),
        "peak": int((cgroup / "memory.peak").read_text().strip()),
        "limit_max": int(max_value),
        "limit_high": int(high_value),
        "swap_current": int((cgroup / "memory.swap.current").read_text().strip()),
        **{f"event_{key}": value for key, value in events.items()},
    }
    try:
        result["pressure_telemetry"] = pressure_metrics(cgroup / "memory.pressure")
    except (OSError, RuntimeError) as exc:
        result["pressure_telemetry_error"] = str(exc)
    return result


def build_abort_reason(ev: dict[str, int], baseline: dict[str, int]) -> str | None:
    """Report only a real build-cgroup OOM; host pressure is telemetry."""
    if (ev.get("event_oom", 0) > baseline.get("event_oom", 0) or
            ev.get("event_oom_kill", 0) > baseline.get("event_oom_kill", 0)):
        return "actual build-cgroup OOM event increased"
    return None


def cgroup_path(unit: str) -> pathlib.Path:
    shown = subprocess.run(
        ["systemctl", "--user", "show", "--property=ControlGroup", "--value", unit],
        check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout.strip()
    if not shown.startswith("/") or ".." in pathlib.PurePosixPath(shown).parts:
        raise RuntimeError(f"systemd returned an invalid ControlGroup for {unit}: {shown!r}")
    path = pathlib.Path("/sys/fs/cgroup") / shown.lstrip("/")
    if not path.is_dir():
        raise RuntimeError(f"transient build cgroup is not present: {path}")
    return path


def host_telemetry() -> dict:
    """Best-effort host pressure snapshot. These metrics never gate a build."""
    result: dict = {"available": False}
    try:
        result["memory"] = read_snapshot()
        result["available"] = True
    except (OSError, RuntimeError, ValueError) as exc:
        result["memory_error"] = str(exc)
    try:
        result["psi"] = pressure_metrics(pathlib.Path("/proc/pressure/memory"))
    except (OSError, RuntimeError) as exc:
        result["psi_error"] = str(exc)
    result["sample_monotonic"] = time.monotonic()
    return result


def log_host_telemetry(log, label: str, snapshot: dict, previous: dict | None = None) -> None:
    current_memory = snapshot.get("memory", {})
    prior_memory = previous.get("memory", {}) if previous else {}
    current_psi = snapshot.get("psi")
    prior_psi = previous.get("psi") if previous else None
    elapsed = max(0.001, snapshot["sample_monotonic"] - previous["sample_monotonic"]) if previous else None
    full_stall_rate = None
    if elapsed and current_psi and prior_psi:
        full_stall_rate = stall_rate(prior_psi, current_psi, elapsed)
    record = {
        "label": label,
        "MemAvailable_kib": current_memory.get("MemAvailable"),
        "SwapFree_kib": current_memory.get("SwapFree"),
        "pswpin_pages": current_memory.get("pswpin"),
        "pswpout_pages": current_memory.get("pswpout"),
        "swapin_pages_per_second": (
            max(0.0, (current_memory["pswpin"] - prior_memory["pswpin"]) / elapsed)
            if elapsed and current_memory.get("pswpin") is not None and prior_memory.get("pswpin") is not None
            else None
        ),
        "swapout_pages_per_second": (
            max(0.0, (current_memory["pswpout"] - prior_memory["pswpout"]) / elapsed)
            if elapsed and current_memory.get("pswpout") is not None and prior_memory.get("pswpout") is not None
            else None
        ),
        "host_full_stall_rate": full_stall_rate,
        "host_psi": current_psi,
        "telemetry_errors": {
            key: snapshot[key] for key in ("memory_error", "psi_error") if key in snapshot
        },
    }
    log.write("BUILD_HOST_TELEMETRY=" + json.dumps(record, sort_keys=True) + "\n")


def unit_kill(unit: str, sig: str) -> None:
    subprocess.run(["systemctl", "--user", "kill", f"--signal={sig}", unit],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def stop_build(unit: str, runner: subprocess.Popen, log) -> None:
    if runner.poll() is not None:
        return
    print(f"BUILD_STOP unit={unit} signal=SIGINT", file=log, flush=True)
    unit_kill(unit, "SIGINT")
    try:
        runner.wait(timeout=15)
        return
    except subprocess.TimeoutExpired:
        pass
    print(f"BUILD_STOP unit={unit} signal=SIGTERM; no SIGKILL escalation", file=log, flush=True)
    unit_kill(unit, "SIGTERM")
    try:
        runner.wait(timeout=20)
    except subprocess.TimeoutExpired:
        print("BUILD_STOP_TIMEOUT; builder may still be exiting; no SIGKILL sent", file=log, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("workspace", type=pathlib.Path)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--log", required=True, type=pathlib.Path)
    parser.add_argument("target", nargs="?", default="nexus-cuda-real-model")
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    args = parser.parse_args()

    root = args.workspace.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    log_path = args.log.resolve(strict=False)
    if root not in build.parents or root not in log_path.parents:
        raise RuntimeError("workspace, build directory, and log must remain under persistent workspace")
    failures = validate_persistent_env(root, os.environ)
    if failures:
        raise RuntimeError("persistent tool paths BLOCKED: " + "; ".join(failures))
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"configured build cache missing: {cache}")
    cache_text = cache.read_text(errors="replace")
    if not re.search(r"^CMAKE_CUDA_ARCHITECTURES(?::[^=]+)?=86$", cache_text, re.M):
        raise RuntimeError("configured build is not exclusively CUDA SM 8.6")
    if "NEXUS_ENABLE_CUDA:BOOL=ON" not in cache_text:
        raise RuntimeError("CUDA is not enabled in the existing build configuration")
    if not shutil_which("systemd-run") or not shutil_which("systemctl"):
        raise RuntimeError("systemd-run/systemctl unavailable; cannot enforce build cgroup")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(f"BUILD_STARTUP utc={time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}\n")
    host_baseline = host_telemetry()
    with log_path.open("a", encoding="utf-8") as log:
        log_host_telemetry(log, "before-build", host_baseline)
    if args.timeout_seconds < 1:
        raise RuntimeError("build timeout must be positive")
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    unit = f"nexus-m233-build-{stamp}-{uuid.uuid4().hex[:6]}.service"
    command = [
        "systemd-run", "--user", f"--unit={unit}", "--collect", "--wait", "--pipe",
        "--property=Type=exec", "--property=KillMode=control-group",
        f"--property=MemoryHigh={CGROUP_HIGH_BYTES // MIB}M",
        f"--property=MemoryMax={CGROUP_MAX_BYTES // MIB}M",
        "--property=MemorySwapMax=0",
        f"--property=WorkingDirectory={build}",
    ]
    for key in ("TMPDIR", "TMP", "TEMP", "CUDA_CACHE_PATH", "CARGO_TARGET_DIR", "CARGO_HOME",
                "XDG_CACHE_HOME", "CCACHE_DIR", "PIP_CACHE_DIR", "PYTHONPYCACHEPREFIX"):
        command.append(f"--setenv={key}={os.environ[key]}")
    for key, value in (("CMAKE_BUILD_PARALLEL_LEVEL", "1"), ("CARGO_BUILD_JOBS", "1"),
                       ("CUDA_VISIBLE_DEVICES", os.environ.get("CUDA_VISIBLE_DEVICES", "")),
                       ("CUDA_DEVICE_ORDER", "PCI_BUS_ID")):
        if value:
            command.append(f"--setenv={key}={value}")
    command += ["--", "cmake", "--build", str(build), "--target", args.target, "--parallel", "1"]

    print("BUILD_CGROUP_POLICY=PASS compiler_max_mib=768 memory_high_mib=640 "
          "swap_max=0 host_pressure_metrics=TELEMETRY_ONLY", flush=True)
    print(f"BUILD_TARGET={args.target} jobs=1 cuda_arch=86 unit={unit}", flush=True)
    print("BUILD_COMMAND=" + " ".join(command), flush=True)
    peak_bytes = 0
    event_baseline: dict[str, int] | None = None
    last_metrics: dict[str, int] | None = None
    with log_path.open("a", encoding="utf-8", buffering=1) as log:
        log.write(f"BUILD_START utc={stamp} unit={unit} target={args.target} timeout_seconds={args.timeout_seconds}\n")
        log.write(f"BUILD_CGROUP_LIMIT memory.high={CGROUP_HIGH_BYTES} memory.max={CGROUP_MAX_BYTES} memory.swap.max=0\n")
        runner = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 30
        cg = None
        while time.monotonic() < deadline and runner.poll() is None:
            try:
                cg = cgroup_path(unit)
                break
            except (OSError, RuntimeError, subprocess.SubprocessError):
                time.sleep(0.25)
        if cg is None and runner.poll() is None:
            stop_build(unit, runner, log)
            raise RuntimeError("cannot verify the transient build cgroup; build stopped")
        if cg is None:
            rc = runner.wait()
            log.write(f"BUILD_RESULT exit={rc} cgroup_created=0\n")
            return rc
        try:
            last_metrics = memory_metrics(cg)
            if last_metrics["limit_max"] != CGROUP_MAX_BYTES or last_metrics["limit_high"] != CGROUP_HIGH_BYTES:
                stop_build(unit, runner, log)
                raise RuntimeError("systemd did not apply the requested finite memory.high/max")
            event_baseline = {key: int(value) for key, value in last_metrics.items()
                              if key.startswith("event_")}
            previous_host = host_baseline
            build_deadline = time.monotonic() + args.timeout_seconds
            while runner.poll() is None:
                if time.monotonic() >= build_deadline:
                    log.write("BUILD_ABORT reason=build timeout; incremental objects preserved\n")
                    stop_build(unit, runner, log)
                    return 124
                time.sleep(POLL_SECONDS)
                host = host_telemetry()
                try:
                    last_metrics = memory_metrics(cg)
                except (OSError, RuntimeError):
                    if runner.poll() is not None:
                        break
                    raise
                peak_bytes = max(peak_bytes, last_metrics["peak"])
                ev = last_metrics
                log_host_telemetry(log, "during-build", host, previous_host)
                sample = {
                    "memory_current_bytes": ev["current"], "memory_peak_bytes": ev["peak"],
                    "memory_high_bytes": ev["limit_high"], "memory_max_bytes": ev["limit_max"],
                    "memory_swap_current_bytes": ev["swap_current"],
                    "memory_events_high": ev.get("event_high", 0),
                    "memory_events_max": ev.get("event_max", 0),
                    "memory_events_oom": ev.get("event_oom", 0),
                    "memory_events_oom_kill": ev.get("event_oom_kill", 0),
                    "host_telemetry": host,
                    "build_cgroup_pressure_telemetry": ev.get("pressure_telemetry"),
                    "build_cgroup_pressure_telemetry_error": ev.get("pressure_telemetry_error"),
                    "build_processes": cgroup_process_metrics(cg),
                    "classification": "TELEMETRY_ONLY",
                }
                log.write("BUILD_SAMPLE_JSON=" + json.dumps(sample, sort_keys=True) + "\n")
                reason = build_abort_reason(ev, event_baseline)
                if reason:
                    log.write(f"BUILD_ABORT reason={reason}\n")
                    stop_build(unit, runner, log)
                    return 13
                previous_host = host
            rc = runner.wait()
        except KeyboardInterrupt:
            stop_build(unit, runner, log)
            return 130
        finally:
            if last_metrics:
                log.write("BUILD_FINAL_CGROUP=" + repr(last_metrics) + "\n")
                log.write(f"BUILD_OBSERVED_CGROUP_PEAK_BYTES={max(peak_bytes, last_metrics['peak'])}\n")
        log.write(f"BUILD_RESULT exit={rc}\n")
        print(f"BUILD_RESULT={'PASS' if rc == 0 else 'FAIL'} exit={rc} "
              f"cgroup_peak_bytes={max(peak_bytes, last_metrics['peak'] if last_metrics else 0)} "
              f"log={log_path}", flush=True)
        return rc


def shutil_which(name: str) -> str | None:
    import shutil
    return shutil.which(name)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"BUILD_PREFLIGHT=BLOCKED {exc}", file=sys.stderr, flush=True)
        raise SystemExit(13)
