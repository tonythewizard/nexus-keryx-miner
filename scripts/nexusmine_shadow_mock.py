#!/usr/bin/env python3
"""One-shot gRPC(mock) -> authenticated capture -> guarded CUDA -> Rust shadow path."""
import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

ROOT = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/m22"
UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
MODEL = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/models/Qwen3.5-9B-abliterated/model.gguf"
TREE = MODEL.with_name("pom-tree.bin")
METADATA = MODEL.with_name("pom-tree.json")
INDEX = ROOT / "artifacts/proofs/m22/20261009T002432/canonical-index-cpp.bin"
ORACLE = ROOT / "build/cargo/release/nexus-m21-oracle"
CUDA = BUILD / "bin/nexus-cuda-real-model"
CLIENT = BUILD / "bin/keryx-nexus-live"
MOCK = BUILD / "bin/nexus-grpc-mock"
REPLAY = BUILD / "bin/keryx-nexus-replay"
MANIFEST = SOURCE / "reports/evidence/m233-build-and-proof-20261010.json"

def run(argv, log_path, env, timeout=None):
    with log_path.open("w") as log:
        p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env)
        if timeout is None:
            return p.wait()
        deadline = time.monotonic() + timeout
        while p.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if p.poll() is not None:
            return p.returncode
        p.send_signal(signal.SIGINT)
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("owned child ignored SIGINT/SIGTERM; no SIGKILL sent") from exc
        raise RuntimeError("owned child exceeded deadline and was asked to stop cooperatively")

def main():
    if len(sys.argv) != 2:
        print("usage: nexusmine_shadow_mock.py VERIFIED_MAINNET_CAPTURE", file=sys.stderr)
        return 2
    capture = pathlib.Path(sys.argv[1]).resolve(strict=True)
    try:
        capture.relative_to((ROOT / "artifacts").resolve())
    except ValueError:
        print("SHADOW=BLOCKED capture must be under persistent artifacts/", file=sys.stderr)
        return 13
    if not all(p.is_file() for p in (MODEL, TREE, METADATA, INDEX, ORACLE, CUDA, CLIENT, MOCK, REPLAY, MANIFEST)):
        print("SHADOW=BLOCKED required model, build, capture index, or oracle artifact is missing", file=sys.stderr)
        return 13
    manifest = json.loads(MANIFEST.read_text())
    expected = next((x.get("sha256") for x in manifest.get("binary_and_objects", [])
                     if x.get("path", "").endswith("/bin/nexus-cuda-real-model")), None)
    actual = hashlib.sha256(CUDA.read_bytes()).hexdigest()
    if not expected or expected != actual:
        print("SHADOW=BLOCKED CUDA binary does not match the M2.3 attestation", file=sys.stderr)
        return 13

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = ROOT / "artifacts/shadow-mock" / stamp
    work = ROOT / "work/m24" / stamp
    logs = ROOT / "logs" / "m24" / stamp
    for directory in (out, work, logs, ROOT / "cache/cuda", ROOT / "cache/cargo",
                      ROOT / "cache/xdg", ROOT / "cache/ccache", ROOT / "cache/python",
                      ROOT / "work/tmp"):
        directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"TMPDIR": str(ROOT / "work/tmp"), "TMP": str(ROOT / "work/tmp"),
                "TEMP": str(ROOT / "work/tmp"), "CUDA_CACHE_PATH": str(ROOT / "cache/cuda"),
                "CARGO_TARGET_DIR": str(ROOT / "build/cargo"), "CARGO_HOME": str(ROOT / "cache/cargo"),
                "XDG_CACHE_HOME": str(ROOT / "cache/xdg"), "PYTHONPYCACHEPREFIX": str(ROOT / "cache/python"),
                "CUDA_DEVICE_ORDER": "PCI_BUS_ID", "CUDA_VISIBLE_DEVICES": UUID,
                "NEXUS_CUDA_DEVICE_UUID": UUID, "KERYX_ALLOW_KWIN_PROCFS_FALLBACK": "1",
                "PYTHONDONTWRITEBYTECODE": "1"})
    if subprocess.run(["python3", "-B", str(SOURCE / "scripts/check_persistent_env.py"), str(ROOT)],
                      env=env, check=False).returncode:
        print("SHADOW=BLOCKED persistent path validation failed", file=sys.stderr)
        return 13

    cert, key = work / "mock-cert.pem", work / "mock-key.pem"
    tls_log = logs / "grpc-mock.log"
    client_capture = out / "grpc-template.capture"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj",
                    "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    with tls_log.open("a") as mocklog:
        server = subprocess.Popen([str(MOCK), str(cert), str(key), str(capture)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=mocklog, text=True, env=env, bufsize=1)
        try:
            line = server.stdout.readline().strip()
            match = re.fullmatch(r"PORT=(\d+)", line)
            if not match:
                raise RuntimeError("captured-template gRPC mock failed to start: " + line)
            endpoint = "grpcs://127.0.0.1:" + match.group(1)
            env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = str(cert)
            rc = run([str(CLIENT), "--dry-run", "--endpoint", endpoint,
                      "--pay-address", "kaspa:offline-shadow-mock",
                      "--extra-data", "NEXUS-M24-OFFLINE-MOCK", "--capture", str(client_capture),
                      "--max-templates", "1"], logs / "grpc-client.log", env, timeout=30)
            if rc != 0:
                raise RuntimeError("gRPC client failed; see persistent grpc-client.log")
        finally:
            if server.poll() is None:
                server.stdin.write("\n"); server.stdin.flush(); server.wait(timeout=5)
    replay_rc = run([str(REPLAY), str(client_capture)], logs / "capture-replay.log", env)
    if replay_rc != 0:
        print("SHADOW=FAIL captured gRPC response failed BLAKE3/protobuf replay", file=sys.stderr)
        return 1
    print(f"SHADOW_GRPC_CAPTURE=PASS origin=OFFLINE_MOCK source_capture_sha256={hashlib.sha256(capture.read_bytes()).hexdigest()} output={client_capture}")

    preflight_rc = run(["python3", "-B", str(SOURCE / "scripts/m22_memory_budget.py"),
                        "--phase", "cuda", "--logical-batch", "1"], logs / "memory-preflight.log", env)
    if preflight_rc != 0:
        print(f"SHADOW_CUDA=BLOCKED memory preflight rc={preflight_rc} logs={logs}")
        return 13
    guard = ["python3", "-B", str(SOURCE / "scripts/m224_gpu_guard.py"),
             "--mode", "functional", "--allow-exe", str(CUDA), "--samples", "6",
             "--max-runtime-seconds", "120", "--", str(CUDA), str(client_capture), str(MODEL),
             str(TREE), str(METADATA), "1", str(out), "1"]
    # The guard itself enforces a 120-second CUDA-child limit and performs SIGINT/SIGTERM cleanup.
    rc = run(guard, logs / "gpu-guard-and-worker.log", env, timeout=None)
    if rc != 0:
        print(f"SHADOW_CUDA=BLOCKED guard_or_worker_rc={rc} logs={logs} submit=DISABLED")
        return rc
    rc = run([str(ORACLE), "verify-cuda-batch", str(MODEL), str(INDEX), str(out / "batch-1.nxcb")],
             logs / "rust-batch-verify.log", env)
    if rc == 0:
        rc = run([str(ORACLE), "verify-real", str(out / "proof-batch-1.context"),
                  str(out / "proof-batch-1.trace"), str(out / "proof-batch-1.bin")],
                 logs / "rust-proof-verify.log", env)
    if rc != 0:
        print(f"SHADOW_RUST=FAIL verifier_rc={rc} logs={logs} submit=DISABLED")
        return 1
    print(f"SHADOW_GRPC_TO_CUDA=PASS template_origin=OFFLINE_MOCK cuda_sha256={actual} proof=RUST_VERIFIED artifacts={out} submit=DISABLED")
    print(f"SHADOW_LOGS={logs}")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as e:
        print(f"SHADOW=BLOCKED bounded child timeout: {e.cmd}", file=sys.stderr)
        raise SystemExit(124)
    except Exception as e:
        print(f"SHADOW=FAIL {e}", file=sys.stderr)
        raise SystemExit(1)
