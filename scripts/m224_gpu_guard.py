#!/usr/bin/env python3
"""Attribution-based bounded CUDA runner; only the primary NVIDIA GPU is queried."""
import argparse
import errno
import os
import pathlib
import signal
import subprocess
import sys
import time

import m22_gpu_guard as hostguard
import m224_gpu_monitor as monitor
from check_persistent_env import validate as validate_persistent_env

LEGACY_PSI_WARNING = 0.10


def cpu_pressure_some_avg10(text=None):
    if text is None:
        try:
            text = pathlib.Path("/proc/pressure/cpu").read_text()
        except OSError:
            return None
    some = next((line for line in text.splitlines() if line.startswith("some ")), None)
    if some is None:
        return None
    raw = next((part.split("=", 1)[1] for part in some.split() if part.startswith("avg10=")), None)
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return None
    if not 0 <= value < float("inf"):
        return None
    return value


def psi_telemetry(value):
    if value is None:
        return "UNAVAILABLE"
    status = "WARNING" if value > LEGACY_PSI_WARNING else "OK"
    return f"{value:.3f}:{status}"


def verify_environment():
    if os.environ.get("CUDA_VISIBLE_DEVICES") != monitor.GPU_UUID:
        raise RuntimeError("CUDA_VISIBLE_DEVICES must contain only the authorized RTX UUID")
    if os.environ.get("NEXUS_CUDA_DEVICE_UUID") != monitor.GPU_UUID:
        raise RuntimeError("NEXUS_CUDA_DEVICE_UUID does not match the authorized RTX")


def verify_host_memory(baseline_swapout=None, mode="benchmark"):
    if mode not in ("functional", "benchmark"):
        raise RuntimeError(f"unknown host safety mode: {mode!r}")
    host = hostguard.host_memory()
    if host[0] < 2 * 1024 * 1024 or host[1] < 2 * 1024 * 1024:
        raise RuntimeError(f"host preflight BLOCKED MemAvailable={host[0]} KiB SwapFree={host[1]} KiB")
    swapout_delta = 0 if baseline_swapout is None else host[4] - baseline_swapout
    if swapout_delta < 0:
        raise RuntimeError("host swap-out counter moved backwards during CUDA guard")
    if mode == "functional" and swapout_delta:
        raise RuntimeError(
            f"host memory safety BLOCKED: swap-out advanced during CUDA session "
            f"{baseline_swapout}->{host[4]} pages"
        )
    if mode == "benchmark" and swapout_delta:
        raise RuntimeError(
            f"benchmark requires stable swap-out counter {baseline_swapout}->{host[4]} pages"
        )
    cpu_pressure = cpu_pressure_some_avg10()
    host = (*host, cpu_pressure, swapout_delta)
    return host


def terminate_child(child):
    if child.poll() is not None:
        return
    child.send_signal(signal.SIGINT)
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("CUDA child did not stop after SIGINT/SIGTERM; no SIGKILL sent") from exc


def read_child_memory_or_exit(child, retries=5, delay_seconds=0.05):
    """Read procfs metrics, tolerating only the exit-vs-procfs disappearance race."""
    last_error = None
    for attempt in range(retries):
        try:
            return hostguard.process_memory(child.pid), False
        except OSError as exc:
            # procfs can report either ENOENT or ESRCH when a very short CUDA
            # process exits between the status and smaps_rollup reads.
            if not isinstance(exc, FileNotFoundError) and exc.errno != errno.ESRCH:
                raise
            last_error = exc
            if child.poll() is not None:
                return None, True
            if attempt + 1 < retries:
                time.sleep(delay_seconds)
    if child.poll() is not None:
        return None, True
    raise RuntimeError(
        f"CUDA child PID={child.pid} is still alive but procfs memory metrics disappeared"
    ) from last_error


def collect_child_gpu_sample(device, child, executable, identity,
                             retries=3, delay_seconds=0.05):
    """Retry only the verified child's exit-vs-/proc/PID/exe sampling race.

    NVML/pmon identity and the pre-run process identity remain mandatory. An
    ENOENT/ESRCH for any other PID/path, or for this child while it remains
    alive after bounded retries, is still a hard failure. Once Popen confirms
    exit, the monitor receives the previously recorded identity and applies its
    PID-reuse checks while comparing the process lists again.
    """
    expected_path = f"/proc/{child.pid}/exe"
    last_error = None
    for attempt in range(retries):
        exited = child.poll() is not None
        try:
            return monitor.collect(
                device, child.pid, executable,
                exited_nexus_identity=identity if exited else None,
            )
        except OSError as exc:
            last_error = exc
            if (exc.errno not in (errno.ENOENT, errno.ESRCH) or
                    exc.filename != expected_path):
                raise
            if attempt + 1 < retries:
                time.sleep(delay_seconds)
                continue
            if child.poll() is None:
                raise RuntimeError(
                    f"CUDA child PID={child.pid} remains alive while its executable procfs identity is missing"
                ) from exc
    raise RuntimeError("bounded GPU identity sampling retries exhausted") from last_error


def summary(snap):
    return {"uuid": snap["uuid"], "gpu_util": snap["gpu_util"], "vram_free_mib": snap["vram_free_mib"],
            "temperature_c": snap["temperature_c"], "power_w": snap["power_w"],
            "power_limit_w": snap["power_limit_w"], "throttle_reasons": snap["throttle_reasons"],
            "processes": [{"pid": p["pid"], "category": p["category"], "type": p["type"],
                           "sm": p["sm"], "vram_mib": p["vram_mib"], "exe": p["exe"]}
                          for p in snap["processes"]]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=6)
    ap.add_argument("--max-runtime-seconds", type=int, default=900)
    ap.add_argument("--mode", choices=("functional", "benchmark"), default="benchmark")
    ap.add_argument("--allow-exe", required=True)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    verify_environment()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not command:
        raise RuntimeError("missing bounded CUDA command")
    if not 5 <= args.samples <= 10 or args.max_runtime_seconds <= 0:
        raise RuntimeError("attribution policy requires 5-10 samples and a finite runtime")
    if args.mode == "functional" and args.max_runtime_seconds > 120:
        raise RuntimeError("functional CUDA tests are bounded to 120 seconds per logical batch")
    allow_exe = str(pathlib.Path(args.allow_exe).resolve(strict=True))
    if str(pathlib.Path(command[0]).resolve(strict=True)) != allow_exe:
        raise RuntimeError("command executable differs from the bounded allow-exe")
    root = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
    persistent = validate_persistent_env(root, os.environ)
    if persistent:
        raise RuntimeError("persistent toolchain paths BLOCKED: " + "; ".join(persistent))

    device = monitor.Nvml()
    preflight = []
    baseline_swapout = None
    try:
        for index in range(args.samples):
            host = verify_host_memory(baseline_swapout, args.mode)
            if baseline_swapout is None:
                baseline_swapout = host[4]
            snap = monitor.collect(device)
            preflight.append(snap)
            print("M224_GPU_SAMPLE phase=preflight " + monitor.json_line(snap), flush=True)
        failures = monitor.policy_failures(preflight, mode=args.mode, bounded_functional=args.mode == "functional")
        if failures:
            raise RuntimeError("attribution preflight BLOCKED: " + "; ".join(failures))
        host = verify_host_memory(baseline_swapout, args.mode)
        external_ids = monitor.identity_key(preflight[-1])
        print(f"M224_GPU_PREFLIGHT=PASS mode={args.mode} uuid={monitor.GPU_UUID} samples={len(preflight)} "
              f"graphics_processes={len(preflight[-1]['processes'])} MemAvailable_kib={host[0]} "
                  f"SwapFree_kib={host[1]} PSI_full_avg10={psi_telemetry(host[2])} "
                  f"CPU_PSI_some_avg10={psi_telemetry(host[7])} swapout_delta_pages={host[8]}", flush=True)

        child = subprocess.Popen(command)
        child_identity = monitor.proc_details(child.pid)
        if child_identity["exe"] != allow_exe:
            terminate_child(child)
            raise RuntimeError(f"CUDA child executable mismatch PID={child.pid}: {child_identity['exe']}")
        start = time.monotonic()
        online = []
        peak_rss = peak_pss = peak_hwm = 0
        print(f"M224_CUDA_CHILD=START pid={child.pid} exe={allow_exe}", flush=True)
        try:
            while child.poll() is None:
                if time.monotonic() - start > args.max_runtime_seconds:
                    raise RuntimeError(f"bounded CUDA batch exceeded {args.max_runtime_seconds}s")
                time.sleep(0.25)
                if child.poll() is not None:
                    break
                proc_mem, exited = read_child_memory_or_exit(child)
                if exited:
                    break
                peak_rss = max(peak_rss, proc_mem["rss_kib"])
                peak_pss = max(peak_pss, proc_mem["pss_kib"])
                peak_hwm = max(peak_hwm, proc_mem["hwm_kib"])
                host = verify_host_memory(baseline_swapout, args.mode)
                snap = collect_child_gpu_sample(device, child, allow_exe, child_identity)
                if child.poll() is not None:
                    break
                online.append(snap)
                print("M224_GPU_SAMPLE phase=running " + monitor.json_line(snap), flush=True)
                window = online[-3:] if len(online) >= 3 else [snap] * 3
                failures = monitor.policy_failures(window, running=True, own_pid=child.pid,
                                                   mode=args.mode, bounded_functional=args.mode == "functional")
                if failures:
                    raise RuntimeError("online GPU safety guard BLOCKED: " + "; ".join(failures))
                ids = monitor.identity_key(snap)
                base_without_child = tuple(row for row in ids if row[0] != child.pid)
                if base_without_child != external_ids:
                    raise RuntimeError("external GPU process identity set changed during CUDA batch")
                if monitor.proc_details(child.pid) != child_identity:
                    raise RuntimeError("CUDA child PID/starttime/executable identity changed")
                print(f"M224_HOST_SAMPLE pid={child.pid} rss_kib={proc_mem['rss_kib']} "
                      f"pss_kib={proc_mem['pss_kib']} hwm_kib={proc_mem['hwm_kib']} "
                      f"peak_rss_kib={peak_rss} peak_pss_kib={peak_pss} "
                      f"MemAvailable_kib={host[0]} PSI_full_avg10={psi_telemetry(host[2])} "
                      f"CPU_PSI_some_avg10={psi_telemetry(host[7])} swapout_delta_pages={host[8]}", flush=True)
        except Exception:
            terminate_child(child)
            raise
        if child.returncode:
            raise RuntimeError(f"CUDA child exited with status {child.returncode}")
        post_window = []
        for _ in range(3):
            post_sample = collect_child_gpu_sample(device, child, allow_exe, child_identity)
            post_window.append(post_sample)
            print("M2216_POST_SAMPLE=" + monitor.json_line(post_sample), flush=True)
        post = post_window[-1]
        post_failures = monitor.policy_failures(post_window, running=True, own_pid=child.pid,
                                                mode=args.mode, bounded_functional=args.mode == "functional")
        if post_failures:
            raise RuntimeError("post-test attribution check BLOCKED: " + "; ".join(post_failures))
        post_external_ids = tuple(row for row in monitor.identity_key(post) if row[0] != child.pid)
        if post_external_ids != external_ids:
            raise RuntimeError("GPU process identity set changed after CUDA batch")
        verify_host_memory(baseline_swapout, args.mode)
        print(f"M224_CUDA_CHILD=PASS pid={child.pid} sampled_peak_rss_kib={peak_rss} "
              f"sampled_peak_pss_kib={peak_pss} sampled_peak_hwm_kib={peak_hwm} "
              f"mode={args.mode} submit=DISABLED", flush=True)
        return 0
    finally:
        device.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"M224_GPU_GUARD=BLOCKED reason={exc}", file=sys.stderr, flush=True)
        raise SystemExit(13)
