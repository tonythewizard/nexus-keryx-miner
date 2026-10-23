#!/usr/bin/env python3
"""Launch one sequential build command with persistent-path and memory guards."""
import argparse
import os
import pathlib
import signal
import subprocess
import sys
import time

from m22_memory_budget import read_snapshot, stable_preflight
from check_persistent_env import validate as validate_persistent_env

EMERGENCY_AVAILABLE_KIB = 2 * 1024 * 1024
EMERGENCY_PSI_FULL = 0.20
POLL_SECONDS = 1.0
_interrupted = False


def process_group_usage(pgid: int) -> tuple[int, int, int]:
    """Return RSS/PSS and process count for only the isolated build process group."""
    rss_kib = pss_kib = count = 0
    for raw_pid in os.listdir("/proc"):
        if not raw_pid.isdigit():
            continue
        proc = pathlib.Path("/proc") / raw_pid
        try:
            stat = (proc / "stat").read_text()
            tail = stat[stat.rfind(")") + 2:].split()
            process_group = int(tail[2])  # stat field 5; tail begins at field 3
            if process_group != pgid:
                continue
            status = (proc / "status").read_text().splitlines()
            rss = next((int(x.split()[1]) for x in status if x.startswith("VmRSS:")), 0)
            rollup = (proc / "smaps_rollup").read_text().splitlines()
            pss = next((int(x.split()[1]) for x in rollup if x.startswith("Pss:")), 0)
            rss_kib += rss
            pss_kib += pss
            count += 1
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, IndexError):
            continue
    return rss_kib, pss_kib, count


def wait_process_group_empty(pgid: int, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process_group_usage(pgid)[2] == 0:
            return True
        time.sleep(0.25)
    return process_group_usage(pgid)[2] == 0


def stop_own_process_group(child: subprocess.Popen, reason: str) -> bool:
    if child.poll() is not None:
        return True
    print(f"M222_BUILD_STOP_REQUEST reason={reason} pgid={child.pid} signal=SIGINT", flush=True)
    try:
        os.killpg(child.pid, signal.SIGINT)
    except ProcessLookupError:
        return True
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        pass
    if wait_process_group_empty(child.pid):
        return True
    print(f"M222_BUILD_STOP_ESCALATION pgid={child.pid} signal=SIGTERM", flush=True)
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return True
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        pass
    if wait_process_group_empty(child.pid, timeout=15):
        return True
    print(f"M222_BUILD_STOP_FAILED pgid={child.pid}; no SIGKILL sent", flush=True)
    return False


def on_interrupt(_signum, _frame):
    global _interrupted
    _interrupted = True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not command:
        raise RuntimeError("missing build command after --")
    workspace = pathlib.Path(args.workspace).resolve(strict=True)
    failures = validate_persistent_env(workspace, os.environ)
    if failures:
        raise RuntimeError("persistent toolchain path check failed: " + "; ".join(failures))

    stable_preflight("build", samples=3, interval=1.0)
    print(f"M222_BUILD_PREFLIGHT=PASS command={command[0]} max_parallelism=1", flush=True)
    baseline = read_snapshot()
    child = subprocess.Popen(command, start_new_session=True)
    signal.signal(signal.SIGINT, on_interrupt)
    signal.signal(signal.SIGTERM, on_interrupt)
    peak_rss = peak_pss = 0
    sample = 0
    while child.poll() is None:
        time.sleep(POLL_SECONDS)
        if child.poll() is not None:
            break
        current = read_snapshot()
        rss, pss, count = process_group_usage(child.pid)
        peak_rss = max(peak_rss, rss)
        peak_pss = max(peak_pss, pss)
        sample += 1
        print(
            f"M222_BUILD_SAMPLE={sample} pid={child.pid} process_count={count} "
            f"tree_rss_kib={rss} tree_pss_kib={pss} peak_tree_rss_kib={peak_rss} "
            f"peak_tree_pss_kib={peak_pss} MemAvailable_kib={current['MemAvailable']} "
            f"SwapFree_kib={current['SwapFree']} PSI_full_avg10={current['psi_full_avg10']} "
            f"pswpin_pages={current['pswpin']} pswpout_pages={current['pswpout']}",
            flush=True,
        )
        reason = None
        if current["MemAvailable"] < EMERGENCY_AVAILABLE_KIB:
            reason = "MemAvailable crossed 2 GiB emergency reserve"
        elif current["psi_full_avg10"] > EMERGENCY_PSI_FULL:
            reason = "memory PSI full avg10 crossed 0.20 emergency threshold"
        elif current["pswpout"] != baseline["pswpout"]:
            reason = "swap-out increased during build"
        elif _interrupted:
            reason = "received external shutdown signal"
        if reason:
            stopped = stop_own_process_group(child, reason)
            return 13 if stopped else 14

    rc = child.wait()
    final = read_snapshot()
    if (final["MemAvailable"] < EMERGENCY_AVAILABLE_KIB or
            final["SwapFree"] < 2 * 1024 * 1024 or
            final["psi_full_avg10"] > EMERGENCY_PSI_FULL or
            final["pswpout"] != baseline["pswpout"]):
        print("M222_BUILD_POSTCHECK=BLOCKED resource reserve, PSI, or swap-out changed", flush=True)
        return 13
    print(
        f"M222_BUILD_GUARD={'PASS' if rc == 0 else 'FAIL'} exit={rc} "
        f"peak_tree_rss_kib={peak_rss} peak_tree_pss_kib={peak_pss} "
        f"MemAvailable_kib={final['MemAvailable']} PSI_full_avg10={final['psi_full_avg10']} "
        f"submit=DISABLED",
        flush=True,
    )
    return rc


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as exc:
        print(f"M222_BUILD_PREFLIGHT=BLOCKED {exc}", file=sys.stderr, flush=True)
        raise SystemExit(13)
