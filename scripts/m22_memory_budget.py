#!/usr/bin/env python3
"""Phase-specific, fail-closed resource gates for M2.2.

The build allowance is a conservative operating budget, not a measured compiler
peak. Runtime observations are logged so it can be calibrated after a safe build.
"""
import argparse
import math
import pathlib
import time

GIB_KIB = 1024 * 1024
MICROBATCH_HOST_BUDGET = 64 * 1024 * 1024
PER_NONCE_HOST_BUDGET = 6 * 256 * 1024 + 64 * 1024
LEGACY_PSI_WARNING = 0.10
POLICIES = {
    # Keep 2 GiB for the node/desktop plus a 2 GiB single-tool compiler allowance.
    "build": {"available": 4 * GIB_KIB, "swap_free": 2 * GIB_KIB},
    # Runtime reference work keeps the original 2 GiB reserve and GPU reserve.
    "cuda": {"available": 2 * GIB_KIB, "swap_free": 2 * GIB_KIB},
    # Benchmarks need additional RAM/swap headroom; PSI remains recorded telemetry.
    "benchmark": {"available": 6 * GIB_KIB, "swap_free": 4 * GIB_KIB},
}


def select_microbatch(logical_batch: int, available_kib: int) -> int:
    if logical_batch not in (1, 8, 64, 256):
        raise ValueError("logical batch must be 1, 8, 64, or 256")
    if available_kib < POLICIES["cuda"]["available"]:
        raise RuntimeError(f"MemAvailable below fixed CUDA safety floor 2 GiB: {available_kib} KiB")
    available_mib = available_kib // 1024
    if available_mib < 3072:
        selected = 1
    elif available_mib < 4096:
        selected = 2
    elif available_mib < 6144:
        selected = 4
    else:
        selected = 8
    selected = min(selected, logical_batch, MICROBATCH_HOST_BUDGET // PER_NONCE_HOST_BUDGET)
    if selected < 1:
        raise RuntimeError("no microbatch fits the fixed host allocation budget")
    return selected


def parse_psi_full_avg10(text: str) -> float:
    line = next((item for item in text.splitlines() if item.startswith("full ")), None)
    if line is None:
        raise RuntimeError("memory PSI full metric unavailable")
    raw = next((part.partition("=")[2] for part in line.split() if part.startswith("avg10=")), None)
    if raw is None:
        raise RuntimeError("memory PSI full avg10 unavailable")
    try:
        value = float(raw)
    except ValueError as exc:
        raise RuntimeError(f"invalid memory PSI full avg10: {raw!r}") from exc
    if not math.isfinite(value) or value < 0:
        raise RuntimeError(f"invalid memory PSI full avg10: {raw!r}")
    return value


def read_snapshot() -> dict:
    values = {}
    for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
        key, _, rest = line.partition(":")
        if key in ("MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached", "Shmem",
                   "Slab", "SReclaimable", "SUnreclaim", "AnonPages", "SwapTotal", "SwapFree"):
            values[key] = int(rest.split()[0])
    vm = {}
    for line in pathlib.Path("/proc/vmstat").read_text().splitlines():
        key, _, raw = line.partition(" ")
        if key in ("pswpin", "pswpout"):
            vm[key] = int(raw)
    if "MemAvailable" not in values or "SwapFree" not in values or len(vm) != 2:
        raise RuntimeError("RAM/swap metrics are incomplete")
    try:
        values["psi_full_avg10"] = parse_psi_full_avg10(
            pathlib.Path("/proc/pressure/memory").read_text())
    except (OSError, RuntimeError):
        # PSI is telemetry only. Keep missing data explicit; never substitute zero.
        values["psi_full_avg10"] = None
    values["pswpin"] = vm["pswpin"]
    values["pswpout"] = vm["pswpout"]
    return values


def validate_snapshot(phase: str, snapshot: dict) -> None:
    if phase not in POLICIES:
        raise ValueError(f"unknown resource phase: {phase}")
    policy = POLICIES[phase]
    if snapshot["MemAvailable"] < policy["available"]:
        raise RuntimeError(
            f"{phase} preflight BLOCKED: MemAvailable={snapshot['MemAvailable']} KiB; "
            f"requires {policy['available']} KiB ({policy['available'] / GIB_KIB:.1f} GiB)"
        )
    if snapshot["SwapFree"] < policy["swap_free"]:
        raise RuntimeError(
            f"{phase} preflight BLOCKED: SwapFree={snapshot['SwapFree']} KiB; "
            f"requires {policy['swap_free']} KiB ({policy['swap_free'] / GIB_KIB:.1f} GiB)"
        )


def psi_status(value: float | None) -> str:
    if value is None:
        return "UNAVAILABLE"
    return "WARNING" if value > LEGACY_PSI_WARNING else "OK"


def stable_preflight(phase: str, samples: int = 3, interval: float = 1.0,
                     reader=read_snapshot, sleeper=time.sleep) -> list[dict]:
    if samples < 2 or interval < 0:
        raise ValueError("stable preflight requires at least two nonnegative-interval samples")
    observed = []
    for index in range(samples):
        snapshot = reader()
        validate_snapshot(phase, snapshot)
        if observed and snapshot["pswpout"] != observed[0]["pswpout"]:
            raise RuntimeError(
                f"{phase} preflight BLOCKED: swap-out changed "
                f"{observed[0]['pswpout']}->{snapshot['pswpout']} pages during sampling"
            )
        observed.append(snapshot)
        print_snapshot(phase, index + 1, snapshot, samples)
        if index + 1 < samples:
            sleeper(interval)
    return observed


def print_snapshot(phase: str, index: int, snapshot: dict, total: int) -> None:
    print(
        f"M222_RESOURCE_SAMPLE phase={phase} sample={index}/{total} "
        f"MemTotal_kib={snapshot['MemTotal']} MemFree_kib={snapshot['MemFree']} "
        f"MemAvailable_kib={snapshot['MemAvailable']} Cached_kib={snapshot['Cached']} "
        f"AnonPages_kib={snapshot['AnonPages']} Slab_kib={snapshot['Slab']} "
        f"SwapFree_kib={snapshot['SwapFree']} pswpin_pages={snapshot['pswpin']} "
        f"pswpout_pages={snapshot['pswpout']} PSI_full_avg10="
        f"{snapshot['psi_full_avg10'] if snapshot['psi_full_avg10'] is not None else 'UNAVAILABLE'} "
        f"PSI_status={psi_status(snapshot['psi_full_avg10'])}"
    , flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=POLICIES, required=True)
    parser.add_argument("--logical-batch", type=int, choices=(1, 8, 64, 256))
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--interval", type=float, default=1.0)
    args = parser.parse_args()
    observed = stable_preflight(args.phase, args.samples, args.interval)
    if args.logical_batch is not None:
        selected = select_microbatch(args.logical_batch, observed[-1]["MemAvailable"])
        print(f"M22_MICROBATCH={selected} logical_batch={args.logical_batch} phase={args.phase} "
              f"estimated_host_budget_bytes={selected * PER_NONCE_HOST_BUDGET}")
    else:
        print(f"M222_PREFLIGHT={args.phase.upper()}_PASS samples={len(observed)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as exc:
        print(str(exc))
        raise SystemExit(13)
