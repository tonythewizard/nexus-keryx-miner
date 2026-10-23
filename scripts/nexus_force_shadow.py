#!/usr/bin/env python3
"""Bounded, telemetry-only resource observer for explicit M2.8 shadow runs.

This mode intentionally does not make admission decisions from pressure,
available RAM/swap, GPU utilization, VRAM headroom, or process load. Device
identity, binary provenance, finite runtime, consensus validation inside the
worker, and cooperative shutdown remain mandatory.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import time

import m22_gpu_guard as hostguard
import m224_gpu_monitor as monitor

GPU_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
GPU_NAME = "NVIDIA GeForce primary NVIDIA GPU"
WORKER_SHA256 = "b03a3b5e517b9a8b6923aac4ac124f360ae8e9c855206da19f41a3935c802eb2"
MAX_NONCES = 32
MAX_RUNTIME_SECONDS = 120
stop_requested = False


def signal_stop(_signum, _frame):
    global stop_requested
    stop_requested = True


def validate_identity(cuda_visible, requested_uuid, reported_uuid, reported_name):
    if cuda_visible != GPU_UUID or requested_uuid != GPU_UUID:
        raise RuntimeError("CUDA device selection is not restricted to the authorized UUID")
    if reported_uuid != GPU_UUID or reported_name != GPU_NAME:
        raise RuntimeError("NVML identity does not match the authorized primary NVIDIA GPU")


def validate_limits(nonce_limit, max_runtime):
    if not 1 <= nonce_limit <= MAX_NONCES:
        raise ValueError(f"nonce limit must be 1..{MAX_NONCES}")
    if not 1 <= max_runtime <= MAX_RUNTIME_SECONDS:
        raise ValueError(f"runtime limit must be 1..{MAX_RUNTIME_SECONDS} seconds")


def validate_endpoint(endpoint):
    if endpoint not in ("grpcs://127.0.0.1:22110", "grpc://127.0.0.1:22110"):
        raise ValueError("force-shadow permits only the configured 127.0.0.1:22110 endpoint")


def parse_stock_mining_fields(launcher_text):
    address = re.search(r"--mining-address\s+(\S+)", launcher_text)
    cert = re.search(r"^set -l CERT\s+([0-9a-fA-F]{128})\s*$", launcher_text, re.MULTILINE)
    if address is None or cert is None:
        raise RuntimeError("could not resolve payout address and escrow certificate from stock launcher")
    return address.group(1), cert.group(1).lower()


def derive_escrow_pubkey(key_path):
    raw = pathlib.Path(key_path).read_text(encoding="ascii").strip()
    if len(raw) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in raw):
        raise RuntimeError("stock escrow key has invalid format")
    secret = bytes.fromhex(raw)
    # SEC1 ECPrivateKey(secp256k1), fed over stdin so the key is never written
    # to a temporary file or included in a child command line.
    der = (b"\x30\x2e\x02\x01\x01\x04\x20" + secret +
           b"\xa0\x07\x06\x05\x2b\x81\x04\x00\x0a")
    result = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-pubout", "-outform", "DER"],
        input=der, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=os.environ.copy(), check=False,
    )
    if result.returncode != 0 or len(result.stdout) < 65 or result.stdout[-65] != 4:
        raise RuntimeError("could not derive the stock escrow x-only public key")
    return result.stdout[-64:-32].hex()


def resolve_stock_mining_fields(launcher_path, escrow_key_path):
    payout, cert = parse_stock_mining_fields(pathlib.Path(launcher_path).read_text(encoding="utf-8"))
    pubkey = derive_escrow_pubkey(escrow_key_path)
    return payout, f"/escrow:{pubkey}/esig:{cert}"


def cgroup_memory_events():
    try:
        row = next(line for line in pathlib.Path("/proc/self/cgroup").read_text().splitlines()
                   if line.startswith("0::"))
        rel = row[3:].lstrip("/")
        path = pathlib.Path("/sys/fs/cgroup") / rel / "memory.events"
        result = {}
        for line in path.read_text().splitlines():
            key, raw = line.split()
            result[key] = int(raw)
        return result
    except (OSError, StopIteration, ValueError):
        return None


def process_gpu_pids(snapshot):
    combined = {}
    for kind in ("compute_pids", "graphics_pids"):
        for pid, info in snapshot[kind].items():
            combined.setdefault(pid, {"kinds": [], "memory_mib": info.get("memory_mib")})
            combined[pid]["kinds"].append(kind.removesuffix("_pids"))
    return combined


def host_telemetry_tuple(host):
    """Format resource readings as observations only; unavailable stays null."""
    return {
        "memory_available_kib": host[0],
        "swap_free_kib": host[1],
        "psi_memory_full_avg10": host[2],
        "psi_status": ("UNAVAILABLE" if host[2] is None else
                       "WARNING" if host[2] > 0.10 else "OK"),
        "swap_in_pages": host[3],
        "swap_out_pages": host[4],
        "swap_total_kib": host[6],
    }


def telemetry_record(device, child=None, phase="sample"):
    try:
        host = hostguard.host_memory()
        host_record = host_telemetry_tuple(host)
        host_error = None
    except Exception as exc:
        host_record = host_telemetry_tuple((None, None, None, None, None, None, None))
        host_error = f"{type(exc).__name__}: {exc}"
    actual_uuid = device.string("nvmlDeviceGetUUID")
    actual_name = device.string("nvmlDeviceGetName")
    validate_identity(os.environ.get("CUDA_VISIBLE_DEVICES"),
                      os.environ.get("NEXUS_CUDA_DEVICE_UUID"), actual_uuid, actual_name)
    try:
        gpu = device.snapshot()
        validate_identity(os.environ.get("CUDA_VISIBLE_DEVICES"),
                         os.environ.get("NEXUS_CUDA_DEVICE_UUID"), gpu["uuid"], gpu["name"])
        gpu_error = None
        pids = process_gpu_pids(gpu)
    except Exception as exc:
        # Metric/API failures are recorded; the worker's own CUDA identity and
        # CUDA API results remain authoritative. A confirmed UUID mismatch is
        # not treated as a missing metric.
        if "identity does not match" in str(exc):
            raise
        gpu = None
        gpu_error = f"{type(exc).__name__}: {exc}"
        pids = {}
    child_memory = None
    if child is not None and child.poll() is None:
        try:
            child_memory = hostguard.process_memory(child.pid)
        except OSError as exc:
            child_memory = {"procfs_error": f"{type(exc).__name__}:{exc.errno}"}
    record = {
        "event": "FORCE_SHADOW_TELEMETRY",
        "phase": phase,
        "monotonic_ns": time.monotonic_ns(),
        **host_record,
        "host_telemetry_error": host_error,
        "memory_events": cgroup_memory_events(),
        "worker_pid": child.pid if child else None,
        "worker_memory": child_memory,
        "gpu": gpu,
        "gpu_processes_uuid_scoped": pids,
        "gpu_telemetry_error": gpu_error,
    }
    return record


def emit(log, device, child=None, phase="sample"):
    record = telemetry_record(device, child, phase)
    line = json.dumps(record, sort_keys=True, separators=(",", ":"))
    log.write(line + "\n")
    log.flush()
    print(line, flush=True)
    return record


def terminate_cooperatively(child):
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
                raise RuntimeError("worker did not stop after SIGINT/SIGTERM; SIGKILL was not sent") from exc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--nonce-limit", required=True, type=int)
    parser.add_argument("--production-launcher", required=True, type=pathlib.Path)
    parser.add_argument("--escrow-key-file", required=True, type=pathlib.Path)
    parser.add_argument("--model", required=True, type=pathlib.Path)
    parser.add_argument("--tree", required=True, type=pathlib.Path)
    parser.add_argument("--metadata", required=True, type=pathlib.Path)
    parser.add_argument("--oracle", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--log", required=True, type=pathlib.Path)
    parser.add_argument("--max-runtime", type=int, default=MAX_RUNTIME_SECONDS)
    args = parser.parse_args()
    validate_limits(args.nonce_limit, args.max_runtime)
    validate_endpoint(args.endpoint)
    if os.environ.get("CUDA_VISIBLE_DEVICES") != GPU_UUID:
        raise RuntimeError("CUDA_VISIBLE_DEVICES must contain only the authorized GPU UUID")
    if os.environ.get("NEXUS_CUDA_DEVICE_UUID") != GPU_UUID:
        raise RuntimeError("NEXUS_CUDA_DEVICE_UUID does not match the authorized GPU UUID")
    pay_address, extra_data = resolve_stock_mining_fields(
        args.production_launcher, args.escrow_key_file)
    binary = args.binary.resolve(strict=True)
    if not os.access(binary, os.X_OK):
        raise RuntimeError("worker binary is not executable")
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != WORKER_SHA256:
        raise RuntimeError("worker binary SHA-256 differs from the M2.8 attested executable")
    for artifact in (args.model, args.tree, args.metadata, args.oracle):
        if not artifact.is_file():
            raise RuntimeError(f"required model/verifier artifact missing: {artifact}")
    args.output.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    worker_log_path = args.log.with_name(args.log.stem + ".worker.log")

    device = monitor.Nvml()
    child = None
    worker_env = os.environ.copy()
    # Shadow never inherits an ambient submit opt-in. The production worker
    # still routes verified winners through SubmitBlockClient, whose explicit
    # gate rejects this mode before opening a channel.
    worker_env["NEXUS_SUBMIT_ENABLED"] = "0"
    worker_env.pop("NEXUS_SUBMIT_ENDPOINT", None)
    with args.log.open("a", encoding="utf-8") as log, worker_log_path.open("a", encoding="utf-8") as worker_log:
        try:
            emit(log, device, phase="before-launch")
            command = [str(binary), args.endpoint, pay_address, extra_data,
                       str(args.model), str(args.tree), str(args.metadata), str(args.oracle),
                       str(args.output), str(args.nonce_limit), "4", "shadow", "--no-submit"]
            child = subprocess.Popen(command, env=worker_env, stdout=worker_log,
                                     stderr=subprocess.STDOUT, bufsize=1)
            print(f"FORCE_SHADOW_LAUNCHED=PASS pid={child.pid} binary_sha256={digest} "
                  f"uuid={GPU_UUID} nonce_limit={args.nonce_limit} "
                  f"worker_log={worker_log_path} submit=DISABLED", flush=True)
            started = time.monotonic()
            last_sample = 0.0
            while child.poll() is None:
                if stop_requested:
                    terminate_cooperatively(child)
                    break
                if time.monotonic() - started >= args.max_runtime:
                    terminate_cooperatively(child)
                    raise TimeoutError(f"bounded force-shadow exceeded {args.max_runtime}s")
                now = time.monotonic()
                if now - last_sample >= 0.5:
                    emit(log, device, child, "running")
                    last_sample = now
                time.sleep(0.1)
            if child.poll() is None:
                child.wait(timeout=10)
            rc = child.returncode
            emit(log, device, child, "after-exit")
            print(f"FORCE_SHADOW_WORKER_EXIT={rc} cooperative_stop="
                  f"{str(stop_requested).lower()} worker_log={worker_log_path} "
                  f"submit=DISABLED", flush=True)
            return rc if rc is not None else 1
        finally:
            if child is not None and child.poll() is None:
                terminate_cooperatively(child)
            device.close()


def cli():
    signal.signal(signal.SIGINT, signal_stop)
    signal.signal(signal.SIGTERM, signal_stop)
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FORCE_SHADOW=FAIL error={type(exc).__name__}:{exc} submit=DISABLED", file=sys.stderr, flush=True)
        raise SystemExit(1)


if __name__ == "__main__":
    cli()
