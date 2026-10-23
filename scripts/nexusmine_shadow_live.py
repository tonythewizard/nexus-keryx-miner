#!/usr/bin/env python3
"""One-template live gRPC -> guarded CUDA -> official Rust shadow run.

The persistent capture is the audit handoff. This finite command does not use
the TLS mock and does not claim a persistent multi-job CUDA context.
Submission is impossible in this command.
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import secrets
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
MANIFEST = SOURCE / "reports/evidence/m27-build-attestation.json"
PROD_LAUNCHER = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/bin/keryx-miner-production.fish"
ESCROW_KEY = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/state/escrow.key"


def persistent_env() -> dict[str, str]:
    env = os.environ.copy()
    for key, relative in {
        "TMPDIR": "work/tmp", "TMP": "work/tmp", "TEMP": "work/tmp",
        "CUDA_CACHE_PATH": "cache/cuda", "CARGO_TARGET_DIR": "build/cargo",
        "CARGO_HOME": "cache/cargo", "XDG_CACHE_HOME": "cache/xdg",
        "CCACHE_DIR": "cache/ccache", "PIP_CACHE_DIR": "cache/pip",
        "PYTHONPYCACHEPREFIX": "cache/python",
    }.items():
        path = ROOT / relative
        path.mkdir(parents=True, exist_ok=True)
        env[key] = str(path)
    env.update({"PYTHONDONTWRITEBYTECODE": "1", "CMAKE_BUILD_PARALLEL_LEVEL": "1",
                "CARGO_BUILD_JOBS": "1", "CUDA_DEVICE_ORDER": "PCI_BUS_ID",
                "CUDA_VISIBLE_DEVICES": UUID, "NEXUS_CUDA_DEVICE_UUID": UUID,
                "KERYX_ALLOW_KWIN_PROCFS_FALLBACK": "1"})
    if any(not pathlib.Path(env[key]).is_relative_to(ROOT) for key in ("TMPDIR", "TMP", "TEMP")):
        raise RuntimeError("temporary directory escaped the persistent workspace")
    return env


def run(argv: list[str], log_path: pathlib.Path, env: dict[str, str], timeout: int) -> int:
    with log_path.open("w", encoding="utf-8") as log:
        child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                 env=env, start_new_session=True)
        deadline = time.monotonic() + timeout
        while child.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if child.poll() is not None:
            return child.returncode
        child.send_signal(signal.SIGINT)
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("owned child did not stop cooperatively; no SIGKILL sent") from exc
        return 124


def template_request() -> tuple[str, str]:
    launcher = PROD_LAUNCHER.read_text(encoding="utf-8")
    payout = re.search(r"--mining-address\s+(\S+)", launcher)
    certificate = re.search(r"set -l CERT ([0-9a-fA-F]{128})", launcher)
    if not payout or not certificate:
        raise RuntimeError("cannot identify configured payout and escrow certificate")
    if ESCROW_KEY.stat().st_mode & 0o077:
        raise RuntimeError("escrow key permissions are not private")
    from cryptography.hazmat.primitives.asymmetric import ec
    secret = bytes.fromhex(ESCROW_KEY.read_text(encoding="ascii").strip())
    key = ec.derive_private_key(int.from_bytes(secret, "big"), ec.SECP256K1())
    public_x = key.public_key().public_numbers().x.to_bytes(32, "big").hex()
    del secret, key
    extra = (f"NEXUS-REBUILT/escrow:{public_x}/esig:{certificate.group(1)}/"
             f"{secrets.token_hex(8)}/ai:v1:")
    return payout.group(1), extra


def main() -> int:
    args = sys.argv[1:]
    runtime, nonce_count = 120, 1
    while args:
        if len(args) < 2:
            print("usage: nexusmine shadow [--max-runtime-seconds 120] [--max-nonces 1..8]", file=sys.stderr)
            return 2
        option, raw = args[:2]
        args = args[2:]
        if option == "--max-runtime-seconds":
            runtime = int(raw)
        elif option == "--max-nonces":
            nonce_count = int(raw)
        else:
            print(f"unsupported shadow option: {option}", file=sys.stderr)
            return 2
    if not 1 <= nonce_count <= 8:
        print("SHADOW_LIVE=BLOCKED max-nonces must be 1..8", file=sys.stderr)
        return 2
    if not 15 <= runtime <= 120:
        print("SHADOW_LIVE=BLOCKED runtime must be 15..120 seconds (GPU guard limit)", file=sys.stderr)
        return 2
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = ROOT / "artifacts/shadow-m27" / stamp
    logs = ROOT / "logs/m27-shadow" / stamp
    out.mkdir(parents=True, exist_ok=False)
    logs.mkdir(parents=True, exist_ok=False)
    env = persistent_env()
    required = (MODEL, TREE, METADATA, INDEX, ORACLE, CUDA, CLIENT, MANIFEST)
    if not all(path.is_file() for path in required):
        raise RuntimeError("required model, index, oracle, CUDA worker, client or manifest missing")
    check = subprocess.run([sys.executable, "-B", str(SOURCE / "scripts/check_persistent_env.py"), str(ROOT)],
                           env=env, text=True, capture_output=True)
    (logs / "persistent-path-check.log").write_text(check.stdout + check.stderr)
    if check.returncode:
        raise RuntimeError("persistent path validation failed")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    expected_cuda = next((item.get("sha256") for item in manifest.get("binary_and_objects", [])
                          if item.get("path", "").endswith("/bin/nexus-cuda-real-model")), None)
    cuda_hash = hashlib.sha256(CUDA.read_bytes()).hexdigest()
    if not expected_cuda or cuda_hash != expected_cuda:
        raise RuntimeError("worker SHA-256 differs from the recorded M2.3 attestation")
    identity = subprocess.run([sys.executable, "-B", str(SOURCE / "scripts/m223_gpu_identity.py"), "--fish"],
                              env=env, text=True, capture_output=True)
    (logs / "gpu-identity.log").write_text(identity.stdout + identity.stderr)
    if identity.returncode or identity.stdout.strip() != UUID:
        raise RuntimeError("fail-closed primary NVIDIA GPU UUID check failed")

    payout, extra = template_request()
    capture = out / "fresh-live-template.capture"
    capture_log = logs / "live-grpc-capture.log"
    rc = run([str(CLIENT), "--dry-run", "--endpoint", "grpc://127.0.0.1:22110",
              "--pay-address", payout, "--extra-data", extra, "--capture", str(capture),
              "--max-templates", "1"], capture_log, env, timeout=45)
    if rc:
        raise RuntimeError(f"direct gRPC acquisition failed rc={rc}")
    text = capture_log.read_text(encoding="utf-8", errors="replace")
    if ("NODE_NETWORK name=mainnet" not in text or "synced=true" not in text or
            "LIVE_TEMPLATE generation=" not in text):
        raise RuntimeError("capture log does not attest a fresh synchronized Mainnet job")
    if "inference_challenge=present" in text:
        raise RuntimeError("OPoI challenge present; CUDA mining refused until it is answered")

    memory = subprocess.run([sys.executable, "-B", str(SOURCE / "scripts/m22_memory_budget.py"),
                              "--phase", "cuda", "--logical-batch", str(nonce_count)], env=env,
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (logs / "memory-preflight.log").write_text(memory.stdout)
    if memory.returncode:
        raise RuntimeError("existing CUDA memory preflight blocked this live job")
    output = out / "cuda-proof"
    output.mkdir()
    guard_log = logs / "cuda-safety-and-worker.log"
    command = [sys.executable, "-B", str(SOURCE / "scripts/m224_gpu_guard.py"),
               "--mode", "functional", "--allow-exe", str(CUDA), "--samples", "6",
               "--max-runtime-seconds", str(runtime), "--", str(CUDA), str(capture),
               str(MODEL), str(TREE), str(METADATA), str(nonce_count), str(output), "1"]
    rc = run(command, guard_log, env, timeout=runtime + 40)
    if rc:
        raise RuntimeError(f"GPU guard/CUDA worker did not complete successfully rc={rc}")

    batch_verify = subprocess.run([str(ORACLE), "verify-cuda-batch", str(MODEL), str(INDEX),
                                   str(output / f"batch-{nonce_count}.nxcb")], env=env,
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (logs / "rust-batch-verify.log").write_text(batch_verify.stdout)
    if batch_verify.returncode:
        raise RuntimeError("official Rust batch verifier rejected live CUDA results")
    first_nonce = 0x4b455259584d3231
    proof_logs = []
    for index in range(nonce_count):
        nonce = first_nonce + index
        result = subprocess.run([str(ORACLE), "verify-real",
                                str(output / f"proof-nonce-{nonce}.context"),
                                str(output / f"proof-nonce-{nonce}.trace"),
                                str(output / f"proof-nonce-{nonce}.bin")], env=env,
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        proof_logs.append(f"nonce={nonce} exit={result.returncode}\n{result.stdout}")
        if result.returncode:
            (logs / "rust-proof-verify.log").write_text("\n".join(proof_logs))
            raise RuntimeError(f"official Rust proof verifier rejected nonce={nonce}")
    (logs / "rust-proof-verify.log").write_text("\n".join(proof_logs))
    manifest_path = out / "sha256.manifest"
    files = sorted(path for path in out.rglob("*") if path.is_file() and path != manifest_path)
    manifest_path.write_text("".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(out)}\n"
                                      for p in files))
    print(f"NEXUS_SHADOW_LIVE=PASS fresh_grpc=true nonce_count={nonce_count} "
          f"cuda_sha256={cuda_hash} "
          f"rust=PASS artifacts={out} logs={logs} submit=DISABLED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"NEXUS_SHADOW_LIVE=BLOCKED detail={error} submit=DISABLED", file=sys.stderr)
        raise SystemExit(13)
