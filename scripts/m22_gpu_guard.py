#!/usr/bin/env python3
"""Persistent-workspace-only CUDA test launcher with conservative GPU checks."""
import argparse
import csv
import math
import os
import pathlib
import signal
import subprocess
import sys
import time

GPU_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
KWN_EXE = "/usr/bin/kwin_wayland"
MAX_KWIN_MIB = 128
MAX_UTIL = 5
LEGACY_PSI_WARNING = 0.10


def psi_status(value):
    if value is None:
        return "UNAVAILABLE"
    return "WARNING" if value > LEGACY_PSI_WARNING else "OK"


def host_memory():
    values = {}
    for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].rstrip(":") in ("MemAvailable", "SwapFree", "SwapTotal", "Committed_AS"):
            values[parts[0].rstrip(":")] = int(parts[1])
    try:
        avg = parse_psi_full_avg10(pathlib.Path("/proc/pressure/memory").read_text())
    except (OSError, RuntimeError):
        # PSI is observable telemetry, not a runtime admission or termination gate.
        avg = None
    if "MemAvailable" not in values or "SwapFree" not in values:
        raise RuntimeError("host memory metrics are incomplete")
    vm = {}
    for line in pathlib.Path("/proc/vmstat").read_text().splitlines():
        key, _, val = line.partition(" ")
        if key in ("pswpin", "pswpout"):
            vm[key] = int(val)
    if ("pswpin" not in vm or "pswpout" not in vm or
            "SwapTotal" not in values or "Committed_AS" not in values):
        raise RuntimeError("swap activity counters unavailable")
    return (values["MemAvailable"], values["SwapFree"], avg, vm["pswpin"],
            vm["pswpout"], values["Committed_AS"], values["SwapTotal"])


def parse_psi_full_avg10(text):
    full = next((line for line in text.splitlines() if line.startswith("full ")), None)
    if full is None:
        raise RuntimeError("memory PSI full metric unavailable")
    raw = next((part.split("=", 1)[1] for part in full.split()
                if part.startswith("avg10=")), None)
    if raw is None:
        raise RuntimeError("memory PSI full avg10 unavailable")
    try:
        value = float(raw)
    except ValueError as exc:
        raise RuntimeError(f"invalid memory PSI full avg10 value: {raw!r}") from exc
    if not math.isfinite(value) or value < 0:
        raise RuntimeError(f"invalid memory PSI full avg10 value: {raw!r}")
    return value


def process_memory(pid):
    status = pathlib.Path(f"/proc/{pid}/status").read_text().splitlines()
    values = {}
    for line in status:
        key, sep, rest = line.partition(":")
        if sep and key in ("VmRSS", "VmHWM"):
            values[key] = int(rest.strip().split()[0])
    rollup = pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
    pss = next((int(line.split()[1]) for line in rollup if line.startswith("Pss:")), None)
    if not all(k in values for k in ("VmRSS", "VmHWM")) or pss is None:
        raise RuntimeError(f"incomplete RSS/PSS metrics for CUDA process {pid}")
    return {"rss_kib": values["VmRSS"], "hwm_kib": values["VmHWM"], "pss_kib": pss}


def run_nvidia(args):
    p = subprocess.run(["nvidia-smi", *args], text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(f"nvidia-smi failed ({p.returncode}): {p.stderr.strip()}")
    return p.stdout.strip()


def target_gpu():
    rows = list(csv.reader(run_nvidia(["--id=" + GPU_UUID, "--query-gpu=name,uuid,compute_cap,memory.free,memory.total,utilization.gpu,temperature.gpu,power.draw,power.limit,clocks.current.graphics",
                                        "--format=csv,noheader,nounits"]).splitlines()))
    matches = []
    for row in rows:
        row = [x.strip() for x in row]
        if len(row) != 10 or row[1] != GPU_UUID:
            continue
        matches.append(row)
    if len(matches) != 1:
        raise RuntimeError(f"primary NVIDIA GPU UUID not uniquely visible: matches={len(matches)}")
    name, uuid, cc, free, total, util, temperature, power, power_limit, graphics_clock = matches[0]
    if name != "NVIDIA GeForce primary NVIDIA GPU" or cc != "8.6":
        raise RuntimeError(f"wrong CUDA target: name={name!r} uuid={uuid} SM={cc}")
    if int(free) < 2048:
        raise RuntimeError(f"VRAM preflight below 2048 MiB: free={free} MiB")
    return {"name": name, "uuid": uuid, "cc": cc, "free_mib": int(free),
            "util": int(util), "temperature_c": int(temperature),
            "power_w": float(power), "power_limit_w": float(power_limit),
            "graphics_clock_mhz": int(graphics_clock)}


def proc_identity(pid):
    proc = pathlib.Path(f"/proc/{pid}")
    exe_error = None
    try:
        exe = str((proc / "exe").resolve(strict=True))
    except PermissionError as exc:
        exe = None
        exe_error = exc
    status = (proc / "status").read_text()
    uidline = next(line for line in status.splitlines() if line.startswith("Uid:"))
    euid = int(uidline.split()[2])
    comm = (proc / "comm").read_text().strip()
    raw = (proc / "stat").read_text()
    tail = raw[raw.rfind(")") + 2:].split()
    starttime = tail[19]
    cmdline = [x.decode(errors="replace") for x in (proc / "cmdline").read_bytes().split(b"\0") if x]
    ppid = int(next(line.split()[1] for line in status.splitlines() if line.startswith("PPid:")))
    cgroup = (proc / "cgroup").read_text().strip().split(":", 2)[-1]
    method = "proc-exe"
    if exe_error is not None:
        if os.environ.get("KERYX_ALLOW_KWIN_PROCFS_FALLBACK") != "1":
            raise RuntimeError(f"/proc/{pid}/exe inaccessible; explicit restricted KWin fallback is disabled") from exe_error
        exe, method = verify_kwin_fallback(pid, ppid, euid, comm, cmdline, cgroup, starttime)
    return (pid, starttime, euid, comm, exe, cmdline, ppid, cgroup, method)


def read_wrapper_identity(pid):
    proc = pathlib.Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    uid = int(next(line.split()[2] for line in status.splitlines() if line.startswith("Uid:")))
    comm = (proc / "comm").read_text().strip()
    cmdline = [x.decode(errors="replace") for x in (proc / "cmdline").read_bytes().split(b"\0") if x]
    cgroup = (proc / "cgroup").read_text().strip().split(":", 2)[-1]
    statline = (proc / "stat").read_text()
    starttime = statline[statline.rfind(")") + 2:].split()[19]
    return uid, comm, cmdline, cgroup, starttime


def verify_kwin_fallback(pid, ppid, euid, comm, cmdline, cgroup, starttime):
    verify_kwin_service(pid, ppid, euid, comm, cmdline, cgroup, starttime)
    # This fallback establishes association with the service using multiple procfs/systemd facts;
    # it explicitly does not claim that comm/cmdline prove the inaccessible executable inode.
    return KWN_EXE, "systemd-cgroup-parent-cmdline-fallback"


def verify_kwin_service(pid, ppid, euid, comm, cmdline, cgroup, starttime):
    uid = os.geteuid()
    expected_cgroup = f"/user.slice/user-{uid}.slice/user@{uid}.service/session.slice/plasma-kwin_wayland.service"
    if euid != uid or comm != "kwin_wayland" or not cmdline or cmdline[0] != KWN_EXE or cgroup != expected_cgroup:
        raise RuntimeError(f"KWin procfs fallback identity mismatch for PID {pid}: UID/comm/cmdline/cgroup")
    p = subprocess.run(["systemctl", "--user", "show", "plasma-kwin_wayland.service",
                        "-p", "ActiveState", "-p", "MainPID", "-p", "ControlGroup", "-p", "ExecStart"],
                       text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(f"cannot verify KWin systemd service: {p.stderr.strip()}")
    props = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
    if props.get("ActiveState") != "active" or props.get("ControlGroup") != expected_cgroup:
        raise RuntimeError("KWin systemd unit state/cgroup is not the expected active user service")
    try:
        mainpid = int(props["MainPID"])
    except (KeyError, ValueError) as exc:
        raise RuntimeError("KWin systemd MainPID is unavailable") from exc
    if ppid != mainpid:
        raise RuntimeError(f"KWin process parent PID {ppid} does not match systemd MainPID {mainpid}")
    wrapper_uid, wrapper_comm, wrapper_args, wrapper_cgroup, wrapper_start = read_wrapper_identity(mainpid)
    if (wrapper_uid != uid or wrapper_comm != "kwin_wayland_wr" or not wrapper_args or
            wrapper_args[0] != "/usr/bin/kwin_wayland_wrapper" or wrapper_cgroup != expected_cgroup):
        raise RuntimeError("KWin wrapper identity/cgroup does not match active systemd unit")
    if starttime != (pathlib.Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[19]):
        raise RuntimeError("KWin PID/starttime changed during procfs fallback verification")


def compute_apps():
    out = run_nvidia(["--id=" + GPU_UUID, "--query-compute-apps=gpu_uuid,pid,process_name,used_gpu_memory",
                      "--format=csv,noheader,nounits"])
    apps = []
    if not out:
        return apps
    for row in csv.reader(out.splitlines()):
        row = [x.strip() for x in row]
        if len(row) != 4:
            raise RuntimeError(f"unparseable CUDA process row: {row!r}")
        uuid, pid_s, pname, mem_s = row
        if uuid != GPU_UUID:
            continue
        if pid_s in ("[N/A]", "N/A") or mem_s in ("[N/A]", "N/A"):
            raise RuntimeError(f"unverifiable GPU process attribution: {row!r}")
        pid = int(pid_s)
        identity = proc_identity(pid)
        apps.append({"pid": pid, "pname": pname, "memory_mib": int(mem_s), "identity": identity})
    return apps


def classify_apps(apps, child_pid=None, child_exe=None):
    identities = {}
    for app in apps:
        pid = app["pid"]
        ident = app["identity"]
        if pid == child_pid:
            if ident[4] != child_exe:
                raise RuntimeError(f"test process identity mismatch for PID {pid}: {ident[4]}")
            identities[pid] = ident
            continue
        if ident[4] == KWN_EXE:
            if ident[2] != os.geteuid() or ident[3] != "kwin_wayland" or not ident[5] or ident[5][0] != KWN_EXE:
                raise RuntimeError(f"KWin process identity check failed for PID {pid}: {ident}")
            verify_kwin_service(pid, ident[6], ident[2], ident[3], ident[5], ident[7], ident[1])
            if app["memory_mib"] > MAX_KWIN_MIB:
                raise RuntimeError(f"KWin GPU memory exceeds {MAX_KWIN_MIB} MiB: {app['memory_mib']} MiB")
            identities[pid] = ident
            continue
        raise RuntimeError(f"unapproved CUDA process PID={pid} exe={ident[4]} name={app['pname']}")
    return identities


def sample(child_pid=None, child_exe=None):
    gpu = target_gpu()
    apps = compute_apps()
    identities = classify_apps(apps, child_pid, child_exe)
    return gpu, identities, host_memory()


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
            except subprocess.TimeoutExpired:
                raise RuntimeError("CUDA test did not stop after SIGINT/SIGTERM; no SIGKILL sent")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-samples", type=int, default=12)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--allow-exe")
    ap.add_argument("--preflight-only", action="store_true",
                    help="perform GPU/process/memory preflight without launching a command")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not cmd and not args.preflight_only:
        raise RuntimeError("missing CUDA test command")
    if cmd and not args.allow_exe:
        raise RuntimeError("--allow-exe is required when launching a CUDA test")
    allowed_exe = str(pathlib.Path(args.allow_exe).resolve(strict=True)) if args.allow_exe else None
    if args.max_samples != 12 or args.interval < 0.9:
        raise RuntimeError("idle policy is fixed: 12 samples maximum, about one second apart")
    previous_ids = None
    consecutive = 0
    started = False
    last = None
    for sample_no in range(1, args.max_samples + 1):
        gpu, ids, host = sample()
        if host[0] < 2 * 1024 * 1024 or host[1] < 2 * 1024 * 1024:
            raise RuntimeError(f"host resource preflight BLOCKED: MemAvailable={host[0]} KiB SwapFree={host[1]} KiB swapin_pages={host[3]} swapout_pages={host[4]}")
        if previous_ids is None:
            baseline_swapout = host[4]
        elif host[4] != baseline_swapout:
            raise RuntimeError(f"host resource preflight BLOCKED: swap-out increased {baseline_swapout}->{host[4]} pages")
        if previous_ids is not None and ids != previous_ids:
            raise RuntimeError("GPU compute-process identity changed during idle preflight")
        previous_ids = ids
        last = gpu
        consecutive = consecutive + 1 if gpu["util"] < MAX_UTIL else 0
        print(f"GPU_IDLE_SAMPLE={sample_no} uuid={gpu['uuid']} util={gpu['util']}% free_mib={gpu['free_mib']} temp_c={gpu['temperature_c']} power_w={gpu['power_w']} power_limit_w={gpu['power_limit_w']} graphics_clock_mhz={gpu['graphics_clock_mhz']} MemAvailable_kib={host[0]} SwapFree_kib={host[1]} SwapUsed_kib={host[6]-host[1]} Committed_AS_kib={host[5]} PSI_full_avg10={host[2] if host[2] is not None else 'UNAVAILABLE'} PSI_status={psi_status(host[2])} swapin_pages={host[3]} swapout_pages={host[4]} consecutive={consecutive}/3 KWin_or_compute_pids={sorted(ids)}", flush=True)
        if consecutive == 3:
            started = True
            break
        if sample_no < args.max_samples:
            time.sleep(args.interval)
    if not started:
        raise RuntimeError(f"GPU idle BLOCKED after {args.max_samples} samples; last utilization={last['util'] if last else 'unknown'}%")
    gpu, ids_before, host = sample()
    if ids_before != previous_ids:
        raise RuntimeError("GPU process set changed after idle window")
    if gpu["util"] >= MAX_UTIL or gpu["free_mib"] < 2048:
        raise RuntimeError(f"GPU prelaunch recheck failed: util={gpu['util']}% free={gpu['free_mib']} MiB")
    if host[0] < 2 * 1024 * 1024 or host[1] < 2 * 1024 * 1024 or host[4] != baseline_swapout:
        raise RuntimeError(f"host resource recheck failed: MemAvailable={host[0]} KiB SwapFree={host[1]} KiB swapout_pages={host[4]} baseline={baseline_swapout}")
    print(f"GPU_PREFLIGHT=PASS name={gpu['name']} uuid={gpu['uuid']} SM={gpu['cc']} free_mib={gpu['free_mib']}", flush=True)
    if args.preflight_only:
        print(f"M222_CUDA_PREFLIGHT_ONLY=PASS uuid={GPU_UUID} utilization={gpu['util']}% "
              f"MemAvailable_kib={host[0]} SwapFree_kib={host[1]}", flush=True)
        return 0
    child = subprocess.Popen(cmd)
    child_id = proc_identity(child.pid)
    if child_id[4] != allowed_exe:
        terminate_child(child)
        raise RuntimeError(f"launched CUDA test executable identity mismatch: {child_id[4]}")
    print(f"CUDA_TEST_PID={child.pid} exe={child_id[4]}", flush=True)
    peak_rss = peak_pss = peak_hwm = 0
    sample_no = 0
    next_gpu_check = time.monotonic()
    while child.poll() is None:
        time.sleep(0.25)
        if child.poll() is not None:
            break
        try:
            proc_mem = process_memory(child.pid)
            peak_rss = max(peak_rss, proc_mem["rss_kib"])
            peak_pss = max(peak_pss, proc_mem["pss_kib"])
            peak_hwm = max(peak_hwm, proc_mem["hwm_kib"])
            host_now = host_memory()
            if host_now[0] < 2 * 1024 * 1024 or host_now[1] < 2 * 1024 * 1024:
                raise RuntimeError(f"host memory emergency guard crossed: MemAvailable={host_now[0]} KiB SwapFree={host_now[1]} KiB")
            if host_now[4] != baseline_swapout:
                raise RuntimeError(f"swap-out activity during CUDA child: {baseline_swapout}->{host_now[4]} pages")
            if time.monotonic() < next_gpu_check:
                continue
            next_gpu_check = time.monotonic() + 1.0
            sample_no += 1
            gpu_now, ids_now, _ = sample(child.pid, allowed_exe)
            expected = dict(ids_before)
            expected_with_child = dict(expected)
            expected_with_child[child.pid] = child_id
            if proc_identity(child.pid) != child_id:
                raise RuntimeError("CUDA child PID/starttime/executable identity changed")
            if ids_now != expected and ids_now != expected_with_child:
                raise RuntimeError(f"compute process set changed while CUDA test ran: got={ids_now!r} expected={expected!r} or child-visible")
            if gpu_now["free_mib"] < 1024:
                raise RuntimeError(f"VRAM emergency floor crossed: {gpu_now['free_mib']} MiB free")
            print(f"CHILD_RESOURCE_SAMPLE={sample_no} pid={child.pid} rss_kib={proc_mem['rss_kib']} pss_kib={proc_mem['pss_kib']} hwm_kib={proc_mem['hwm_kib']} peak_rss_kib={peak_rss} peak_pss_sampled_kib={peak_pss} peak_hwm_kib={peak_hwm} MemAvailable_kib={host_now[0]} SwapFree_kib={host_now[1]} SwapUsed_kib={host_now[6]-host_now[1]} Committed_AS_kib={host_now[5]} PSI_full_avg10={host_now[2] if host_now[2] is not None else 'UNAVAILABLE'} PSI_status={psi_status(host_now[2])} swapin_pages={host_now[3]} swapout_pages={host_now[4]} gpu_util={gpu_now['util']} gpu_free_mib={gpu_now['free_mib']} temp_c={gpu_now['temperature_c']} power_w={gpu_now['power_w']} graphics_clock_mhz={gpu_now['graphics_clock_mhz']}", flush=True)
        except Exception:
            terminate_child(child)
            raise
    rc = child.returncode
    gpu_after, ids_after, host_after = sample()
    if ids_after != ids_before:
        raise RuntimeError("GPU compute process identity changed after CUDA test")
    if host_after[4] != baseline_swapout:
        raise RuntimeError(f"swap-out activity detected in post-test check: {baseline_swapout}->{host_after[4]} pages")
    if host_after[0] < 2 * 1024 * 1024 or host_after[1] < 2 * 1024 * 1024:
        raise RuntimeError(f"host post-test reserve failed: MemAvailable={host_after[0]} KiB SwapFree={host_after[1]} KiB")
    if rc:
        raise RuntimeError(f"CUDA test exited {rc}")
    print(f"CHILD_RESOURCE_PEAKS=pid={child.pid} sampled_rss_kib={peak_rss} sampled_pss_kib={peak_pss} sampled_hwm_kib={peak_hwm} note=process-must-live-for-proc-samples", flush=True)
    print(f"GPU_GUARDED_COMMAND=PASS uuid={GPU_UUID} post_util={gpu_after['util']}% free_mib={gpu_after['free_mib']} MemAvailable_kib={host_after[0]} PSI_full_avg10={host_after[2] if host_after[2] is not None else 'UNAVAILABLE'} PSI_status={psi_status(host_after[2])} swapout_pages={host_after[4]}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"GPU_GUARDED_COMMAND=BLOCKED reason={exc}", file=sys.stderr, flush=True)
        sys.exit(13)
