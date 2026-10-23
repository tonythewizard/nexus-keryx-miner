#!/usr/bin/env python3
"""Compare NEXUS OPoI adapter with the official runtime ABI in separate processes."""
from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import sys

GPU_UUID = "GPU-00000000-0000-0000-0000-000000000000"
MODEL_SHA256 = "946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6"
RUNTIME_SHA256 = "c07e7e78d133aeca33a30fedb8d6420e5374c76015cc06ca29022d454ae8ee7d"
MODEL = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/models/Qwen3.5-9B-abliterated/model.gguf"
RUNTIME = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/src/keryx-miner-official/keryxmine-v0.5.7-PoM/libkeryx-llama.so"
NONCE = "0011aaff"


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: opoi_independent_reference.py RUNNER PERSISTENT_OUTPUT_DIR")
    runner = pathlib.Path(sys.argv[1]).resolve()
    output_dir = pathlib.Path(sys.argv[2]).resolve()
    if not runner.is_file() or not MODEL.is_file() or not RUNTIME.is_file():
        raise SystemExit("OPoI runner, official GGUF, or stock runtime is missing")
    if digest(MODEL) != MODEL_SHA256 or digest(RUNTIME) != RUNTIME_SHA256:
        raise SystemExit("OPoI model/runtime SHA-256 does not match the attested inputs")
    output_dir.mkdir(parents=True, exist_ok=True)
    temp = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "work/tmp"
    cache = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "cache/python"
    temp.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "TMPDIR": str(temp), "TMP": str(temp), "TEMP": str(temp),
        "CUDA_CACHE_PATH": "/opt/nexus-data/cache/cuda",
        "CUDA_DEVICE_ORDER": "PCI_BUS_ID", "CUDA_VISIBLE_DEVICES": GPU_UUID,
        "NEXUS_CUDA_DEVICE_UUID": GPU_UUID, "KERYX_LLAMA_CTX": "8192",
        "PYTHONDONTWRITEBYTECODE": "1", "PYTHONPYCACHEPREFIX": str(cache),
    })
    paths = {mode: output_dir / f"opoi-{mode}-{os.getpid()}.bin" for mode in ("official", "nexus")}
    for mode in ("official", "nexus"):
        command = [str(runner), mode, str(MODEL), str(RUNTIME), NONCE, str(paths[mode])]
        result = subprocess.run(command, env=env, check=False)
        if result.returncode != 0:
            print(f"OPOI_REFERENCE_PARITY=FAIL mode={mode} exit={result.returncode}", file=sys.stderr)
            return result.returncode or 1
    official = paths["official"].read_bytes()
    nexus = paths["nexus"].read_bytes()
    if not official or official != nexus:
        print("OPOI_REFERENCE_PARITY=FAIL reason=output-bytes-differ", file=sys.stderr)
        return 1
    print("OPOI_REFERENCE_PARITY=PASS "
          f"gpu_uuid={GPU_UUID} model_sha256={MODEL_SHA256} runtime_sha256={RUNTIME_SHA256} "
          f"nonce={NONCE} output_bytes={len(official)} output_sha256={hashlib.sha256(official).hexdigest()} "
          f"official_output={paths['official']} nexus_output={paths['nexus']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
