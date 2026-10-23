#!/usr/bin/env python3
"""Attest the exact committed CUDA/Rust artifacts after a successful guarded build."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import uuid

ROOT = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/m22"
BIN = BUILD / "bin/nexus-cuda-real-model"
ORACLE = ROOT / "build/cargo/release/nexus-m21-oracle"
MANIFEST = ROOT / "artifacts/build/m2212-cuda-build-provenance.json"


def sha(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def checked(argv: list[str]) -> str:
    result = subprocess.run(argv, text=True, capture_output=True, check=False, env=os.environ.copy())
    if result.returncode:
        raise RuntimeError(f"failed {argv}: {result.stderr.strip()}")
    return result.stdout


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] != "--record":
        print("usage: m2212_cuda_build_provenance.py --record", file=sys.stderr)
        return 2
    commit = checked(["git", "-C", str(SOURCE), "rev-parse", "HEAD"]).strip()
    dirty = checked(["git", "-C", str(SOURCE), "status", "--porcelain"]).strip()
    if dirty:
        raise RuntimeError("refusing build attestation for dirty worktree")
    required = [BIN, ORACLE, BUILD / "CMakeFiles/nexus-cuda-real-model.dir/src/model/cuda_real_model.cpp.o",
                BUILD / "CMakeFiles/nexus_cuda.dir/consensus/cuda_walk.cu.o",
                BUILD / "libnexus_cuda.a", BUILD / "CMakeCache.txt"]
    for path in required:
        if not path.is_file():
            raise RuntimeError(f"missing build artifact: {path}")
    cache = (BUILD / "CMakeCache.txt").read_text(errors="replace")
    for setting in ("NEXUS_ENABLE_CUDA:BOOL=ON", "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_CUDA_ARCHITECTURES:UNINITIALIZED=86"):
        if setting not in cache:
            raise RuntimeError(f"build configuration mismatch: {setting}")
    elf = checked(["/opt/cuda/bin/cuobjdump", "--list-elf", str(BIN)])
    if ".sm_86.cubin" not in elf:
        raise RuntimeError("the CUDA executable does not contain an sm_86 cubin")
    dry = checked(["ninja", "-C", str(BUILD), "-n", "bin/nexus-cuda-real-model"])
    if "no work to do" not in dry.lower():
        raise RuntimeError("Ninja reports pending work; executable is not current")
    data = {
        "schema": 1, "git_commit": commit, "gpu_uuid": os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000"),
        "cuda_arch": "sm_86", "cuda_binary_sha256": sha(BIN), "rust_oracle_sha256": sha(ORACLE),
        "cuda_host_source_sha256": sha(SOURCE / "src/model/cuda_real_model.cpp"),
        "cuda_kernel_source_sha256": sha(SOURCE / "consensus/cuda_walk.cu"),
        "rust_batch_oracle_source_sha256": sha(SOURCE / "oracle-rust/src/cuda_batch.rs"),
        "host_object_sha256": sha(required[2]), "cuda_object_sha256": sha(required[3]),
        "cuda_library_sha256": sha(required[4]), "sm_86_elf_verified": True,
        "ninja_noop_verified": True,
    }
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    if MANIFEST.exists():
        raise RuntimeError(f"refusing to overwrite existing provenance manifest: {MANIFEST}")
    pending = MANIFEST.with_name(MANIFEST.name + ".pending-" + uuid.uuid4().hex)
    with pending.open("w+b") as stream:
        stream.write((json.dumps(data, sort_keys=True, indent=2) + "\n").encode())
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(pending, MANIFEST)
    print("M2212_BUILD_PROVENANCE=PASS " + json.dumps(data, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"M2212_BUILD_PROVENANCE=BLOCKED {exc}", file=sys.stderr)
        raise SystemExit(13)
