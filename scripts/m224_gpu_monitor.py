#!/usr/bin/env python3
"""RTX-3070-Ti-only NVML + nvidia-smi pmon attribution and policy."""
from __future__ import annotations

import ctypes
import errno
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
from dataclasses import dataclass

import m22_gpu_guard as procguard

GPU_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
GPU_NAME = "NVIDIA GeForce primary NVIDIA GPU"
GPU_CC = "8.6"
DISPLAY_KWIN = "/usr/bin/kwin_wayland"
MAX_KWIN_MIB = 128
MIN_FREE_MIB = 2048
MAX_TEMP_C = 75
MAX_PRETEST_POWER_FRACTION = 0.80
MAX_RUN_POWER_FRACTION = 0.95
MAX_RUN_TEMP_C = 80
MAX_TOTAL_GRAPHICS_MIB = 768
MAX_TOTAL_UTIL = 40       # retain at least 60% aggregate SM headroom
# Functional correctness checks may overlap verified display rendering, but
# keep a hard aggregate ceiling and independent per-process / resource caps.
# Benchmarks additionally require stable aggregate and per-process utilization.
MAX_KWIN_SM = 35
MAX_OTHER_GRAPHICS_SM = 30
MAX_UNATTRIBUTED_SM = 15
MAX_UTIL_RANGE = 15
MAX_PROCESS_UTIL_RANGE = 10
NVML_THROTTLE_MASK = sum(1 << bit for bit in range(2, 8))


class _Utilization(ctypes.Structure):
    _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]


class _Memory(ctypes.Structure):
    _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong),
                ("used", ctypes.c_ulonglong)]


class _ProcessInfoV3(ctypes.Structure):
    _fields_ = [("pid", ctypes.c_uint), ("usedGpuMemory", ctypes.c_ulonglong),
                ("gpuInstanceId", ctypes.c_uint), ("computeInstanceId", ctypes.c_uint)]


class _ProcessUtilizationSample(ctypes.Structure):
    _fields_ = [("pid", ctypes.c_uint), ("timeStamp", ctypes.c_ulonglong),
                ("smUtil", ctypes.c_uint), ("memUtil", ctypes.c_uint),
                ("encUtil", ctypes.c_uint), ("decUtil", ctypes.c_uint)]


class Nvml:
    """Small direct NVML binding. Device lookup is by the sole authorized UUID."""
    def __init__(self):
        self.lib = ctypes.CDLL("libnvidia-ml.so.1")
        self._bind("nvmlInit_v2", [], ctypes.c_int)
        self._bind("nvmlShutdown", [], ctypes.c_int)
        handle_name = ("nvmlDeviceGetHandleByUUID_v2" if hasattr(self.lib, "nvmlDeviceGetHandleByUUID_v2")
                       else "nvmlDeviceGetHandleByUUID")
        self._bind(handle_name, [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int)
        self._bind("nvmlDeviceGetUUID", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint], ctypes.c_int)
        self._bind("nvmlDeviceGetName", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint], ctypes.c_int)
        self._bind("nvmlDeviceGetUtilizationRates", [ctypes.c_void_p, ctypes.POINTER(_Utilization)], ctypes.c_int)
        self._bind("nvmlDeviceGetMemoryInfo", [ctypes.c_void_p, ctypes.POINTER(_Memory)], ctypes.c_int)
        self._bind("nvmlDeviceGetTemperature", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int)
        self._bind("nvmlDeviceGetPowerUsage", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int)
        self._bind("nvmlDeviceGetPowerManagementLimit", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int)
        self._bind("nvmlDeviceGetClockInfo", [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int)
        self._bind("nvmlDeviceGetCurrentClocksThrottleReasons", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulonglong)], ctypes.c_int)
        self._bind("nvmlDeviceGetProcessUtilization",
                   [ctypes.c_void_p, ctypes.POINTER(_ProcessUtilizationSample),
                    ctypes.POINTER(ctypes.c_uint), ctypes.c_ulonglong], ctypes.c_int)
        self._process_functions = {}
        for kind in ("Compute", "Graphics"):
            name = f"nvmlDeviceGet{kind}RunningProcesses_v3"
            if not hasattr(self.lib, name):
                raise RuntimeError(f"required NVML process attribution API unavailable: {name}")
            fn = getattr(self.lib, name)
            fn.restype = ctypes.c_int
            fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(_ProcessInfoV3)]
            self._process_functions[kind.lower()] = fn
        self._bind("nvmlErrorString", [ctypes.c_int], ctypes.c_char_p)
        self._check(self.lib.nvmlInit_v2(), "nvmlInit_v2")
        self.handle = ctypes.c_void_p()
        self._check(getattr(self.lib, handle_name)(GPU_UUID.encode(), ctypes.byref(self.handle)),
                    "NVML RTX UUID lookup")
        self.process_util_last_seen_us = 0
        self.process_util_cache = {}

    def _bind(self, name, args, result):
        fn = getattr(self.lib, name, None)
        if fn is None:
            raise RuntimeError(f"required NVML API unavailable: {name}")
        fn.restype, fn.argtypes = result, args

    def _check(self, code, operation):
        if code:
            raw = self.lib.nvmlErrorString(code)
            detail = raw.decode(errors="replace") if raw else f"NVML error {code}"
            raise RuntimeError(f"{operation} failed: NVML code={code}: {detail}")

    def string(self, name):
        buf = ctypes.create_string_buffer(96)
        self._check(getattr(self.lib, name)(self.handle, buf, len(buf)), name)
        return buf.value.decode(errors="strict")

    def process_pids(self, kind):
        fn = self._process_functions[kind]
        count = ctypes.c_uint(0)
        code = fn(self.handle, ctypes.byref(count), None)
        # NVML_ERROR_INSUFFICIENT_SIZE (7) is expected for a nonempty list.
        if code not in (0, 7):
            self._check(code, f"NVML {kind} process-list size query")
        if count.value == 0:
            return {}
        capacity = count.value
        entries = (_ProcessInfoV3 * capacity)()
        self._check(fn(self.handle, ctypes.byref(count), entries), f"NVML {kind} process list")
        if count.value > capacity:
            raise RuntimeError(f"NVML {kind} process list grew beyond buffer")
        result = {}
        for entry in entries[:count.value]:
            if entry.pid == 0 or str(entry.pid) in result:
                raise RuntimeError(f"NVML {kind} process list contains invalid/duplicate PID")
            mem = None if entry.usedGpuMemory == 0xFFFFFFFFFFFFFFFF else entry.usedGpuMemory // (1024 * 1024)
            result[str(entry.pid)] = {"memory_mib": mem, "gpu_instance": entry.gpuInstanceId,
                                      "compute_instance": entry.computeInstanceId}
        return result

    def process_utilization(self):
        # NVML timestamp is CPU time in microseconds. SM utilization is a
        # combined 3D/compute metric and must be interpreted with process type.
        capacity = 128
        while capacity <= 4096:
            entries = (_ProcessUtilizationSample * capacity)()
            count = ctypes.c_uint(capacity)
            code = self.lib.nvmlDeviceGetProcessUtilization(
                self.handle, entries, ctypes.byref(count), self.process_util_last_seen_us)
            if code == 7:  # NVML_ERROR_INSUFFICIENT_SIZE
                capacity = max(capacity * 2, count.value)
                continue
            self.process_util_status = {
                "api": "nvmlDeviceGetProcessUtilization", "code": code,
                "status": {0: "SUCCESS", 3: "NOT_SUPPORTED", 6: "NOT_FOUND"}.get(code, "ERROR"),
                "returned_count": count.value if code == 0 else 0,
            }
            if code in (3, 6):
                # No samples is not zero utilization. Do not reuse cached
                # activity to disguise unsupported/temporarily absent data.
                self.process_util_cache.clear()
                break
            self._check(code, "NVML per-process utilization samples")
            now_us = time.time_ns() // 1000
            for row in entries[:count.value]:
                item = {"pid": str(row.pid), "time_us": int(row.timeStamp),
                        "sm": int(row.smUtil), "memory_util": int(row.memUtil),
                        "enc": int(row.encUtil), "dec": int(row.decUtil)}
                if item["time_us"] > self.process_util_last_seen_us:
                    self.process_util_last_seen_us = item["time_us"]
                if not 0 <= item["sm"] <= 100 or not 0 <= item["memory_util"] <= 100:
                    raise RuntimeError(f"invalid NVML process utilization sample: {item}")
                if 0 <= now_us - item["time_us"] <= 5_000_000:
                    self.process_util_cache[item["pid"]] = item
            break
        else:
            raise RuntimeError("NVML process-utilization buffer exceeds 4096 entries")
        now_us = time.time_ns() // 1000
        self.process_util_cache = {pid: item for pid, item in self.process_util_cache.items()
                                   if 0 <= now_us - item["time_us"] <= 5_000_000}
        return dict(self.process_util_cache)

    def snapshot(self):
        util, mem = _Utilization(), _Memory()
        temp, power, power_limit, clock = (ctypes.c_uint() for _ in range(4))
        throttle = ctypes.c_ulonglong()
        self._check(self.lib.nvmlDeviceGetUtilizationRates(self.handle, ctypes.byref(util)), "NVML utilization")
        self._check(self.lib.nvmlDeviceGetMemoryInfo(self.handle, ctypes.byref(mem)), "NVML memory")
        self._check(self.lib.nvmlDeviceGetTemperature(self.handle, 0, ctypes.byref(temp)), "NVML temperature")
        self._check(self.lib.nvmlDeviceGetPowerUsage(self.handle, ctypes.byref(power)), "NVML power usage")
        self._check(self.lib.nvmlDeviceGetPowerManagementLimit(self.handle, ctypes.byref(power_limit)), "NVML power limit")
        self._check(self.lib.nvmlDeviceGetClockInfo(self.handle, 0, ctypes.byref(clock)), "NVML graphics clock")
        self._check(self.lib.nvmlDeviceGetCurrentClocksThrottleReasons(self.handle, ctypes.byref(throttle)),
                    "NVML throttle reasons")
        process_util = self.process_utilization()
        return {"time_ns": time.time_ns(), "uuid": self.string("nvmlDeviceGetUUID"),
                "name": self.string("nvmlDeviceGetName"), "gpu_util": util.gpu,
                "memory_util": util.memory, "vram_total_mib": mem.total // (1024 * 1024),
                "vram_free_mib": mem.free // (1024 * 1024), "vram_used_mib": mem.used // (1024 * 1024),
                "temperature_c": temp.value, "power_w": power.value / 1000.0,
                "power_limit_w": power_limit.value / 1000.0, "graphics_clock_mhz": clock.value,
                "throttle_reasons": throttle.value, "compute_pids": self.process_pids("compute"),
                "graphics_pids": self.process_pids("graphics"),
                "process_util_samples": process_util,
                "process_util_capability": dict(self.process_util_status)}

    def close(self):
        self._check(self.lib.nvmlShutdown(), "nvmlShutdown")


def parse_pmon_text(text):
    records = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 12 or not fields[0].isdigit() or not fields[1].isdigit():
            continue
        pid, kind = fields[1], fields[2]
        def metric(raw):
            if raw in ("-", "N/A", "n/a"):
                return None
            try:
                value = int(raw)
            except ValueError as exc:
                raise RuntimeError(f"invalid pmon metric {raw!r}: {line}") from exc
            if not 0 <= value <= 100:
                raise RuntimeError(f"out-of-range pmon utilization {raw!r}: {line}")
            return value
        if pid in records:
            raise RuntimeError(f"duplicate PID in target GPU pmon output: {pid}")
        records[pid] = {"type": kind, "sm": metric(fields[3]), "memory_util": metric(fields[4]),
                        "fb_mib": int(fields[9]) if fields[9].isdigit() else None,
                        "command_display_only": " ".join(fields[11:])}
    if not records:
        # An empty list is valid only when NVML also reports no contexts; caller checks.
        return {}
    return records


def pmon_sample():
    result = subprocess.run(["nvidia-smi", "pmon", "-i", GPU_UUID, "-c", "1", "-s", "um"],
                            text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f"target-only nvidia-smi pmon failed: {result.stderr.strip()}")
    return parse_pmon_text(result.stdout)


def reconcile_process_sets(pmon, compute_pids, graphics_pids, nexus_pid=None):
    """Require exact external agreement while tolerating only a verified NEXUS sampling race."""
    compute = set(compute_pids)
    graphics = set(graphics_pids)
    nvml = compute | graphics
    nexus = str(nexus_pid) if nexus_pid is not None else None
    pmon_external = set(pmon) - ({nexus} if nexus else set())
    nvml_external = nvml - ({nexus} if nexus else set())
    if pmon_external != nvml_external:
        raise RuntimeError(
            f"external NVML/pmon process sets disagree: "
            f"pmon={sorted(pmon_external)} NVML={sorted(nvml_external)}"
        )
    if nexus and (nexus in pmon or nexus in nvml):
        if nexus in graphics or (nexus in pmon and "C" not in pmon[nexus]["type"]):
            raise RuntimeError("NEXUS PID is not attributed exclusively to a compute context")
        pmon = dict(pmon)
        pmon.setdefault(nexus, {"type": "C", "sm": None, "memory_util": None,
                                "fb_mib": compute_pids.get(nexus, {}).get("memory_mib")})
    return pmon


import kwin_identity


def proc_details(pid):
    ident = procguard.proc_identity(int(pid))
    result = {"pid": ident[0], "starttime": ident[1], "uid": ident[2], "comm": ident[3],
            "exe": ident[4], "cmdline": ident[5], "ppid": ident[6], "cgroup": ident[7],
            "identity_method": ident[8]}
    if result["exe"] == DISPLAY_KWIN:
        evidence, token = kwin_identity.collect(result)
        result.update(kwin_evidence=evidence, kwin_identity_token=token)
    return result


def _same_user_session(proc):
    return proc["uid"] == os.geteuid() and f"/user-{os.geteuid()}.slice/" in proc["cgroup"]


def command_tokens(proc):
    # Some applications overwrite argv with a single space-delimited process
    # title in /proc/PID/cmdline. Preserve exact exe/PID/cgroup checks and
    # tokenize only that observed title for the required role marker.
    return [token for raw in proc["cmdline"] for token in raw.split()]


def classify_process(proc, pmon_type, pmon_pid_set):
    exe, args, pid, ppid = proc["exe"], proc["cmdline"], proc["pid"], proc["ppid"]
    has_graphics = "G" in pmon_type
    has_compute = "C" in pmon_type
    if not pmon_type or any(ch not in "CG+" for ch in pmon_type):
        raise RuntimeError(f"unknown NVIDIA pmon process type PID={pid}: {pmon_type!r}")
    if exe == DISPLAY_KWIN:
        if not has_graphics:
            raise RuntimeError(f"KWin PID={pid} lacks graphics attribution")
        procguard.verify_kwin_service(pid, ppid, proc["uid"], proc["comm"], args,
                                      proc["cgroup"], proc["starttime"])
        return "kde-kwin-mixed-graphics" if has_compute else "kde-kwin-graphics"
    if not has_graphics:
        return "external-compute" if has_compute else "unknown"
    if has_compute:
        raise RuntimeError(f"mixed compute/graphics context is not an approved compositor PID={pid} exe={exe!r}")
    if not _same_user_session(proc):
        raise RuntimeError(f"graphics PID={pid} is outside the current user session cgroup")
    if exe == "/usr/bin/Xwayland":
        parent = proc_details(ppid)
        if parent["exe"] != DISPLAY_KWIN or parent["pid"] != ppid or parent["cgroup"] != proc["cgroup"]:
            raise RuntimeError(f"Xwayland PID={pid} is not directly parented by the verified KWin process")
        return "kde-xwayland-graphics"
    if (exe == "/usr/bin/plasmashell" and proc["comm"] == "plasmashell" and
            proc["cgroup"].endswith("/plasma-plasmashell.service")):
        return "kde-plasmashell-graphics"
    tokens = command_tokens(proc)
    if (exe == "/usr/bin/coolercontrol" and tokens and tokens[0] == exe and
            "/app-org.coolercontrol.CoolerControl@" in proc["cgroup"]):
        return "verified-user-graphics"
    if (exe == "/usr/lib/chromium/chromium" and tokens and tokens[0] == exe and
            "--type=gpu-process" in tokens and proc["comm"] == "chromium"):
        parent = proc_details(ppid)
        if parent["exe"] != exe or not _same_user_session(parent):
            raise RuntimeError(f"Chromium GPU PID={pid} has an unverified parent")
        if has_compute:
            raise RuntimeError(f"Chromium GPU PID={pid} has a mixed compute/graphics context")
        return "verified-user-graphics"
    raise RuntimeError(f"unrecognized graphics process PID={pid} exe={exe!r}; refusing name-only classification")


def confirmed_exited_nexus_procfs_race(pid, error, nexus_pid, nexus_exe,
                                      exited_nexus_identity):
    """Accept only ENOENT/ESRCH for the just-reaped, previously verified child PID."""
    return (exited_nexus_identity is not None and pid == nexus_pid == exited_nexus_identity.get("pid")
            and exited_nexus_identity.get("exe") == nexus_exe
            and isinstance(error, OSError) and error.errno in (errno.ENOENT, errno.ESRCH))


def collect(nvml=None, nexus_pid=None, nexus_exe=None, exited_nexus_identity=None):
    own_nvml = nvml is None
    device = nvml or Nvml()
    try:
        pmon = pmon_sample()
        snap = device.snapshot()
        if snap["uuid"] != GPU_UUID or snap["name"] != GPU_NAME:
            raise RuntimeError(f"NVML device identity mismatch: {snap['name']} {snap['uuid']}")
        pmon = reconcile_process_sets(pmon, snap["compute_pids"], snap["graphics_pids"], nexus_pid)
        process_util = snap["process_util_samples"]
        stale_activity = [row for pid, row in process_util.items()
                          if pid not in pmon and pid != str(nexus_pid) and
                          (row["sm"] > 0 or row["memory_util"] > 0)]
        if stale_activity:
            raise RuntimeError(f"recent NVML utilization PID lacks active pmon identity: {stale_activity}")
        process_rows = []
        exited_nexus_pids = set()
        for pid, item in sorted(pmon.items(), key=lambda entry: int(entry[0])):
            try:
                proc = proc_details(pid)
            except OSError as exc:
                if confirmed_exited_nexus_procfs_race(
                        int(pid), exc, nexus_pid, nexus_exe, exited_nexus_identity):
                    exited_nexus_pids.add(int(pid))
                    continue
                raise
            if nexus_pid is not None and int(pid) == nexus_pid:
                if exited_nexus_identity is not None:
                    raise RuntimeError(f"exited NEXUS PID={pid} still has a procfs identity; refusing possible PID reuse")
                if proc["exe"] != nexus_exe or "C" not in item["type"]:
                    raise RuntimeError(f"NEXUS PID={pid} executable/type attribution mismatch")
                category = "nexus-cuda"
            else:
                category = classify_process(proc, item["type"], set(pmon))
            compute = pid in snap["compute_pids"]
            graphics = pid in snap["graphics_pids"]
            # A C+G/KWin record is explicitly mixed; its SM percentage is not
            # labelled CUDA utilization. C-only external contexts are blocked.
            if compute and not graphics and category != "nexus-cuda":
                category = "external-compute"
            memory = snap["graphics_pids"].get(pid, {}).get("memory_mib")
            if memory is None:
                memory = snap["compute_pids"].get(pid, {}).get("memory_mib")
            utilization = process_util.get(pid)
            process_rows.append({**proc, **item, "category": category,
                                 "nvml_compute_context": compute,
                                 "nvml_graphics_context": graphics,
                                 "nvml_sm": utilization["sm"] if utilization else None,
                                 "nvml_memory_util": utilization["memory_util"] if utilization else None,
                                 "nvml_sample_time_us": utilization["time_us"] if utilization else None,
                                 "vram_mib": memory})
        snap["processes"] = process_rows
        # Recheck identity and context membership, not dynamic VRAM, after
        # collecting samples. An exiting/reused PID is ambiguous, not harmless.
        for row in process_rows:
            try:
                current = proc_details(row["pid"])
            except OSError as exc:
                if confirmed_exited_nexus_procfs_race(
                        row["pid"], exc, nexus_pid, nexus_exe, exited_nexus_identity):
                    exited_nexus_pids.add(row["pid"])
                    continue
                raise
            keys = ("pid", "starttime", "uid", "comm", "exe", "cmdline",
                     "ppid", "cgroup", "identity_method")
            if ({k: current[k] for k in keys} != {k: row[k] for k in keys} or
                    current.get("kwin_identity_token") != row.get("kwin_identity_token")):
                raise RuntimeError(f"GPU PID identity changed during collection: {row['pid']}")
        if exited_nexus_pids:
            process_rows = [row for row in process_rows if row["pid"] not in exited_nexus_pids]
            snap["processes"] = process_rows
            snap["terminated_nexus_pids"] = sorted(exited_nexus_pids)
        for kind in ("compute", "graphics"):
            before = set(snap[kind + "_pids"])
            after = set(device.process_pids(kind))
            if before != after:
                raise RuntimeError(f"NVML {kind} process list changed during collection")
        for row in process_rows:
            if (("C" in row["type"]) != row["nvml_compute_context"] or
                    ("G" in row["type"]) != row["nvml_graphics_context"]):
                raise RuntimeError(f"NVML/pmon context types disagree PID={row['pid']}")
        snap["process_lists_complete"] = True
        snap["identities_rechecked"] = True
        snap["missing_sm_pids"] = [p["pid"] for p in process_rows
                                  if p["sm"] is None and p["nvml_sm"] is None]
        snap["attribution_mode"] = ("alternative-required" if
            snap["gpu_util"] >= 5 and not any(p["sm"] is not None or p["nvml_sm"] is not None
                                             for p in process_rows) else "normal")
        return snap
    finally:
        if own_nvml:
            device.close()


def identity_key(snapshot):
    return tuple((p["pid"], p["starttime"], p["uid"], p["exe"], tuple(p["cmdline"]),
                  p["ppid"], p["cgroup"], p["category"], p["type"],
                  p.get("kwin_identity_token"))
                 for p in snapshot["processes"])


def policy_failures(snapshots, running=False, own_pid=None, mode="benchmark",
                    bounded_functional=False):
    failures = []
    if mode not in ("functional", "benchmark"):
        return [f"unknown GPU guard mode: {mode!r}"]
    if len(snapshots) < 3:
        return ["fewer than three complete GPU attribution samples"]
    required_snapshot_fields = (
        "uuid", "name", "gpu_util", "vram_free_mib", "temperature_c",
        "power_w", "power_limit_w", "throttle_reasons", "processes",
    )
    for index, snap in enumerate(snapshots):
        missing = [key for key in required_snapshot_fields if key not in snap or snap[key] is None]
        if missing:
            failures.append(f"GPU telemetry incomplete in sample {index}: missing {','.join(missing)}")
            return sorted(set(failures))
        if snap["uuid"] != GPU_UUID or snap["name"] != GPU_NAME:
            failures.append(
                f"GPU identity mismatch in sample {index}: {snap['name']} {snap['uuid']}"
            )
            return sorted(set(failures))
        if not isinstance(snap["processes"], list):
            failures.append(f"GPU process attribution unavailable in sample {index}")
            return sorted(set(failures))
        for process in snap["processes"]:
            missing_process = [key for key in (
                "pid", "starttime", "uid", "exe", "cgroup", "category", "type",
            ) if key not in process or process[key] is None]
            if missing_process:
                failures.append(
                    f"GPU process identity incomplete in sample {index}: "
                    f"missing {','.join(missing_process)}"
                )
                return sorted(set(failures))
    def stable_ids(s):
        rows = identity_key(s)
        return tuple(row for row in rows if not (running and own_pid is not None and row[0] == own_pid))
    base_ids = stable_ids(snapshots[0])
    if any(stable_ids(s) != base_ids for s in snapshots[1:]):
        failures.append("GPU process identity set changed during the attribution window")
    util = [s["gpu_util"] for s in snapshots]
    if not running and max(util) > MAX_TOTAL_UTIL:
        failures.append(f"aggregate GPU utilization exceeded {MAX_TOTAL_UTIL}% reserved-headroom policy: {max(util)}%")
    # Aggregate utilization is evidence and retains a hard ceiling in both
    # modes. Its variance is a reproducibility criterion for benchmarks only:
    # short functional validation may overlap verified compositor frame
    # bursts, subject to every independent attribution/resource bound below.
    if not running and mode == "benchmark" and max(util) - min(util) > MAX_UTIL_RANGE:
        failures.append(f"aggregate GPU burst range exceeded {MAX_UTIL_RANGE}%: {min(util)}-{max(util)}%")
    if not running:
        process_ids = {p["pid"] for snap in snapshots for p in snap["processes"]}
        for pid in process_ids:
            values = []
            for snap in snapshots:
                row = next((p for p in snap["processes"] if p["pid"] == pid), None)
                if row is not None:
                    measured = [value for value in (row.get("sm"), row.get("nvml_sm")) if value is not None]
                    if measured:
                        values.append(max(measured))
            is_verified_kwin = (
                row is not None and row["exe"] == DISPLAY_KWIN and
                row["category"] in ("kde-kwin-mixed-graphics", "kde-kwin-graphics")
            )
            # A short correctness run may overlap compositor frame bursts. The
            # compositor is re-identified by PID/starttime/UID/exe/cgroup on
            # every sample; its absolute SM cap, aggregate GPU cap, and all
            # other resource limits remain active. Benchmarks keep the stricter
            # per-process variance policy.
            if (len(values) >= 2 and max(values) - min(values) > MAX_PROCESS_UTIL_RANGE and
                    not (mode == "functional" and is_verified_kwin)):
                failures.append(f"per-process SM burst range exceeded {MAX_PROCESS_UTIL_RANGE}% PID={pid}: {min(values)}-{max(values)}%")
    for s in snapshots:
        absent = not any(p.get("sm") is not None or p.get("nvml_sm") is not None
                         for p in s["processes"])
        alternative = absent and s["gpu_util"] >= 5
        if alternative:
            if mode != "functional" or not bounded_functional:
                failures.append("missing per-process SM requires bounded monitored functional execution; no benchmark fallback")
            if s.get("process_util_capability", {}).get("status") not in ("SUCCESS", "NOT_SUPPORTED", "NOT_FOUND"):
                failures.append("missing SM has no unambiguous NVML capability diagnosis")
            if s.get("process_lists_complete") is not True or s.get("identities_rechecked") is not True:
                failures.append("alternative attribution lacks complete stable process enumeration")
            if not s["processes"]:
                failures.append("GPU activity without any attributable process")
            if s["gpu_util"] > MAX_TOTAL_UTIL:
                failures.append(f"alternative attribution aggregate exceeds {MAX_TOTAL_UTIL}%")
            for p in s["processes"]:
                if p.get("identity_method") != "proc-exe":
                    try:
                        if (mode != "functional" or not bounded_functional or
                                p["exe"] != DISPLAY_KWIN or
                                p["category"] not in ("kde-kwin-graphics", "kde-kwin-mixed-graphics") or
                                p.get("nvml_graphics_context") is not True or
                                "G" not in p["type"]):
                            raise RuntimeError("not a bounded, observed KWin graphics context")
                        token = kwin_identity.validate(p.get("kwin_evidence", {}), p)
                        if token != p.get("kwin_identity_token"):
                            raise RuntimeError("KWin evidence token mismatch")
                    except RuntimeError as exc:
                        failures.append(f"alternative attribution requires real executable verification or complete authorized KWin correlation PID={p['pid']}: {exc}")
                if p.get("vram_mib") is None or p.get("fb_mib") is None:
                    failures.append(f"alternative attribution lacks process memory PID={p['pid']}")
                if (("C" in p["type"]) != p.get("nvml_compute_context") or
                        ("G" in p["type"]) != p.get("nvml_graphics_context")):
                    failures.append(f"alternative attribution context disagreement PID={p['pid']}")
        if s["vram_free_mib"] < MIN_FREE_MIB:
            failures.append(f"RTX free VRAM below {MIN_FREE_MIB} MiB: {s['vram_free_mib']} MiB")
        temp_limit = MAX_RUN_TEMP_C if running else MAX_TEMP_C
        if s["temperature_c"] > temp_limit:
            failures.append(f"GPU temperature exceeds {temp_limit} C: {s['temperature_c']} C")
        power_fraction = s["power_w"] / s["power_limit_w"] if s["power_limit_w"] else float("inf")
        power_limit = MAX_RUN_POWER_FRACTION if running else MAX_PRETEST_POWER_FRACTION
        if power_fraction > power_limit:
            failures.append(f"GPU power draw fraction exceeds {power_limit:.0%}: {power_fraction:.1%}")
        if s["throttle_reasons"] & NVML_THROTTLE_MASK:
            failures.append(f"GPU slowdown/throttle active: mask=0x{s['throttle_reasons']:x}")
        attributed = 0
        external_attributed = 0
        for p in s["processes"]:
            known_sm = [value for value in (p.get("sm"), p.get("nvml_sm")) if value is not None]
            sm = max(known_sm) if known_sm else None
            attributed += sm if sm is not None else 0
            if p["pid"] != own_pid:
                external_attributed += sm if sm is not None else 0
            if p["pid"] == own_pid:
                continue
            if p["category"] == "external-compute":
                failures.append(f"external compute context PID={p['pid']} exe={p['exe']}")
            if p["category"] not in ("kde-kwin-mixed-graphics", "kde-kwin-graphics",
                                       "kde-xwayland-graphics", "kde-plasmashell-graphics",
                                       "verified-user-graphics", "nexus-cuda"):
                failures.append(f"unapproved/unknown GPU process PID={p['pid']} category={p['category']}")
            if p["exe"] == DISPLAY_KWIN:
                if (p["vram_mib"] is not None and p["vram_mib"] > MAX_KWIN_MIB) or (p["fb_mib"] is not None and p["fb_mib"] > MAX_KWIN_MIB):
                    failures.append(f"KWin GPU memory exceeds {MAX_KWIN_MIB} MiB: PID={p['pid']}")
                if sm is not None and sm > MAX_KWIN_SM:
                    failures.append(f"KWin attributed SM activity exceeds {MAX_KWIN_SM}%: {sm}%")
            elif p["category"] == "verified-user-graphics":
                if sm is not None and sm > MAX_OTHER_GRAPHICS_SM:
                    failures.append(f"non-KDE graphics process SM exceeds {MAX_OTHER_GRAPHICS_SM}%: PID={p['pid']} SM={sm}%")
        if not alternative and max(0, s["gpu_util"] - attributed) > MAX_UNATTRIBUTED_SM:
            failures.append(f"unattributed GPU SM activity exceeds {MAX_UNATTRIBUTED_SM}%: total={s['gpu_util']}% attributed={attributed}%")
        if running and external_attributed > MAX_TOTAL_UTIL:
            failures.append(
                f"external attributed SM activity exceeds {MAX_TOTAL_UTIL}% headroom budget: "
                f"{external_attributed}%"
            )
        display_vram = sum(p["vram_mib"] or p["fb_mib"] or 0 for p in s["processes"] if p["pid"] != own_pid)
        if display_vram > MAX_TOTAL_GRAPHICS_MIB:
            failures.append(f"attributed non-NEXUS process VRAM exceeds {MAX_TOTAL_GRAPHICS_MIB} MiB: {display_vram} MiB")
    if running and own_pid is not None:
        own = [p for s in snapshots for p in s["processes"] if p["pid"] == own_pid]
        if own and any(p["category"] != "nexus-cuda" for p in own):
            failures.append("runner PID appeared with an unexpected process classification")
    return sorted(set(failures))


def json_line(snapshot):
    safe = dict(snapshot)
    safe_processes = []
    for process in snapshot["processes"]:
        public = {key: value for key, value in process.items()
                  if key not in ("cmdline", "command_display_only")}
        public["cmdline_sha256"] = hashlib.sha256("\0".join(process["cmdline"]).encode()).hexdigest()
        safe_processes.append(public)
    safe["processes"] = safe_processes
    return json.dumps(safe, sort_keys=True, separators=(",", ":"))


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=6)
    ap.add_argument("--interval", type=float, default=0.0)
    args = ap.parse_args()
    if not 3 <= args.samples <= 10 or args.interval < 0:
        raise RuntimeError("attribution window requires 3-10 pmon samples")
    all_samples = []
    device = Nvml()
    try:
        for i in range(args.samples):
            snap = collect(device)
            all_samples.append(snap)
            print(json_line(snap), flush=True)
            if i + 1 < args.samples and args.interval:
                time.sleep(args.interval)
    finally:
        device.close()
    failures = policy_failures(all_samples)
    print("M224_ATTRIBUTION=" + ("PASS" if not failures else "BLOCKED") +
          " samples=" + str(len(all_samples)) + " max_util=" +
          str(max(s["gpu_util"] for s in all_samples)) + " failures=" + json.dumps(failures), flush=True)
    return 0 if not failures else 13


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"M224_ATTRIBUTION=BLOCKED telemetry_error={exc}", file=sys.stderr, flush=True)
        raise SystemExit(13)
