#!/usr/bin/env python3
"""Single-job build supervisor with measured admission and emergency reserve."""
from __future__ import annotations

import os
import pathlib
import signal
import subprocess
import sys
import time

from check_persistent_env import validate as validate_persistent_env
from m22_memory_budget import read_snapshot, parse_psi_full_avg10

GIB_KIB = 1024 * 1024
CGROUP_BUILD_MAX_KIB = 1536 * 1024
SYSTEM_RESERVE_KIB = 2 * GIB_KIB
START_MARGIN_KIB = 512 * 1024
START_AVAILABLE_KIB = SYSTEM_RESERVE_KIB + CGROUP_BUILD_MAX_KIB + START_MARGIN_KIB
MIN_SWAP_FREE_KIB = 2 * GIB_KIB
START_PSI_MAX = 0.10
EMERGENCY_AVAILABLE_KIB = 2 * GIB_KIB
EMERGENCY_PSI_MAX = 0.20
POLL_SECONDS = 1.0


def snapshot() -> dict:
    result = read_snapshot()
    if result["MemAvailable"] < START_AVAILABLE_KIB:
        raise RuntimeError(f"build admission BLOCKED MemAvailable={result['MemAvailable']} KiB; "
                           f"requires {START_AVAILABLE_KIB} KiB = 2GiB system reserve + "
                           "1536MiB compiler cgroup maximum + 512MiB margin")
    if result["SwapFree"] < MIN_SWAP_FREE_KIB:
        raise RuntimeError(f"build admission BLOCKED SwapFree={result['SwapFree']} KiB; "
                           f"requires {MIN_SWAP_FREE_KIB} KiB")
    if result["psi_full_avg10"] > START_PSI_MAX:
        raise RuntimeError(f"build admission BLOCKED PSI full avg10={result['psi_full_avg10']}; "
                           f"maximum {START_PSI_MAX}")
    return result


def child_usage(pgid: int) -> tuple[int, int, int]:
    rss = pss = count = 0
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        proc = pathlib.Path("/proc") / name
        try:
            raw = (proc / "stat").read_text()
            tail = raw[raw.rfind(")") + 2:].split()
            if int(tail[2]) != pgid:
                continue
            status = (proc / "status").read_text().splitlines()
            proc_rss = next((int(x.split()[1]) for x in status if x.startswith("VmRSS:")), 0)
            rollup = (proc / "smaps_rollup").read_text().splitlines()
            proc_pss = next((int(x.split()[1]) for x in rollup if x.startswith("Pss:")), 0)
            rss += proc_rss
            pss += proc_pss
            count += 1
        except (OSError, ValueError, IndexError):
            continue
    return rss, pss, count


def stop_owned_group(child: subprocess.Popen, reason: str) -> None:
    if child.poll() is not None:
        return
    print(f"M232_BUILD_STOP reason={reason} pgid={child.pid} signal=SIGINT", flush=True)
    try:
        os.killpg(child.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        pass
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and child_usage(child.pid)[2]:
        time.sleep(0.25)
    if child.poll() is not None and child_usage(child.pid)[2] == 0:
        return
    print(f"M232_BUILD_STOP_ESCALATION pgid={child.pid} signal=SIGTERM", flush=True)
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        print("M232_BUILD_STOP_FAILED; no SIGKILL sent", flush=True)


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        print("usage: m232_build_guard.py WORKSPACE -- COMMAND [ARGS...]", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve(strict=True)
    failures = validate_persistent_env(root, os.environ)
    if failures:
        raise RuntimeError("persistent tool paths BLOCKED: " + "; ".join(failures))
    samples = []
    for index in range(3):
        current = snapshot()
        if samples and current["pswpout"] != samples[0]["pswpout"]:
            raise RuntimeError("build admission BLOCKED: swap-out changed during stable preflight")
        samples.append(current)
        print(f"M232_BUILD_PREFLIGHT_SAMPLE={index+1}/3 MemAvailable_kib={current['MemAvailable']} "
              f"SwapFree_kib={current['SwapFree']} PSI_full_avg10={current['psi_full_avg10']} "
              f"pswpout={current['pswpout']}", flush=True)
        if index != 2:
            time.sleep(1)
    print(f"M232_BUILD_PREFLIGHT=PASS command={sys.argv[3]} jobs=1 isolated_process_group=1 "
          "systemd_memory_max_kib=1572864 swap_max_kib=0", flush=True)
    child = subprocess.Popen(sys.argv[3:], start_new_session=True)
    peak_rss = peak_pss = 0
    try:
        while child.poll() is None:
            time.sleep(POLL_SECONDS)
            if child.poll() is not None:
                break
            current = read_snapshot()
            rss, pss, count = child_usage(child.pid)
            peak_rss, peak_pss = max(peak_rss, rss), max(peak_pss, pss)
            print(f"M232_BUILD_SAMPLE pgid={child.pid} processes={count} tree_rss_kib={rss} "
                  f"tree_pss_kib={pss} peak_rss_kib={peak_rss} peak_pss_kib={peak_pss} "
                  f"MemAvailable_kib={current['MemAvailable']} SwapFree_kib={current['SwapFree']} "
                  f"PSI_full_avg10={current['psi_full_avg10']} pswpout={current['pswpout']}", flush=True)
            reason = None
            if current["MemAvailable"] < EMERGENCY_AVAILABLE_KIB:
                reason = "MemAvailable crossed 2GiB reserve"
            elif current["SwapFree"] < MIN_SWAP_FREE_KIB:
                reason = "SwapFree crossed 2GiB reserve"
            elif current["psi_full_avg10"] > EMERGENCY_PSI_MAX:
                reason = "memory PSI full avg10 crossed 0.20 emergency limit"
            elif current["pswpout"] != samples[0]["pswpout"]:
                reason = "swap-out increased during build"
            if reason:
                stop_owned_group(child, reason)
                return 13
    except KeyboardInterrupt:
        stop_owned_group(child, "operator interrupt")
        return 130
    rc = child.wait()
    print(f"M232_BUILD_RESULT={'PASS' if rc == 0 else 'FAIL'} exit={rc} "
          f"peak_tree_rss_kib={peak_rss} peak_tree_pss_kib={peak_pss}", flush=True)
    return rc


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as exc:
        print(f"M232_BUILD_PREFLIGHT=BLOCKED {exc}", file=sys.stderr, flush=True)
        raise SystemExit(13)
