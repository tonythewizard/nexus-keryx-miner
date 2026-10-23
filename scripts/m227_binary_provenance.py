#!/usr/bin/env python3
"""Verify the preserved M2.2 CUDA binary without invoking a compiler or GPU."""
from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
SOURCE = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/m22"
EXPECTED = {
    "build/m22/bin/nexus-cuda-real-model": "02ed2722bf0a14c6ef589632b1083d90411691efe2d30b0c805b4e9147123b20",
    "build/cargo/release/nexus-m21-oracle": "d3b72f9eabb858e6a2a21b214a5078f37f856f9dc8b311a4db2c5f6652a967e3",
    "source/consensus/cuda_walk.cu": "f381cadd652a3167d404059678ce1ef3127e1440f3aaaa341185c0282b32ba63",
    "source/src/model/cuda_real_model.cpp": "be25ec4b4086ddd46e090772cbba8964729ba5dd79e7038423759d816a4be1c7",
    "source/include/keryx/nexus/consensus/cuda_engine.hpp": "e8a416f0e5c39127db4d6b26f07230c21ee54577725e537190a1fc95ff37f985",
    "source/include/keryx/nexus/consensus/cuda_walk_contract.hpp": "57ee2a156ea939d13d3d9fed149ef4dc02c610cb698be77ec47939aeb8e0fd96",
    "build/m22/CMakeFiles/nexus_cuda.dir/consensus/cuda_walk.cu.o": "c4930e13decfddcb5e9385a26621acd9d42db18c791875de16a76ae1056c855a",
    "build/m22/CMakeFiles/nexus-cuda-real-model.dir/src/model/cuda_real_model.cpp.o": "4279efddab37b4fa7fc06dd88588994539cb920c7996fce783fcf7d5bdbbe5ec",
    "build/m22/CMakeFiles/nexus-cuda-real-model.dir/cmake_device_link.o": "ea150e2c7efe3cab72b24331b5d415188c71232b2a94b5d4850a55db85dd8861",
    "build/m22/libnexus_cuda.a": "a07583fd0e8f7f205392f739802b1780716a0fe79adc9a25a34ecef0aff07256",
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    problems = []
    for relative, expected in EXPECTED.items():
        path = ROOT / relative
        actual = sha256(path) if path.is_file() else "MISSING"
        if actual != expected:
            problems.append(f"SHA256 mismatch {relative}: expected={expected} actual={actual}")
    cache = (BUILD / "CMakeCache.txt").read_text(errors="replace")
    required = (
        "CMAKE_BUILD_TYPE:STRING=Release",
        "CMAKE_CUDA_ARCHITECTURES:UNINITIALIZED=86",
        "CMAKE_CUDA_COMPILER:FILEPATH=/opt/cuda/bin/nvcc",
        "NEXUS_ENABLE_CUDA:BOOL=ON",
    )
    problems.extend(f"CMake configuration missing {entry}" for entry in required if entry not in cache)

    no_op = subprocess.run(["ninja", "-C", str(BUILD), "-n", "bin/nexus-cuda-real-model"],
                           text=True, capture_output=True, check=False, env=os.environ.copy())
    if no_op.returncode or "no work to do" not in no_op.stdout:
        problems.append(f"Ninja dry-run is not a no-op: {no_op.stdout}{no_op.stderr}")

    cubins = subprocess.run(["/opt/cuda/bin/cuobjdump", "--list-elf",
                             str(BUILD / "bin/nexus-cuda-real-model")],
                            text=True, capture_output=True, check=False, env=os.environ.copy())
    if cubins.returncode or ".sm_86.cubin" not in cubins.stdout:
        problems.append(f"SM 8.6 cubin verification failed: {cubins.stdout}{cubins.stderr}")

    if problems:
        for problem in problems:
            print("M227_BINARY_PROVENANCE=BLOCKED " + problem, file=sys.stderr)
        return 13
    print("M227_BINARY_PROVENANCE=PASS binary_sha256=" + EXPECTED["build/m22/bin/nexus-cuda-real-model"])
    print("M227_BUILD_REUSE=PASS no compilation invoked; Ninja dry-run reports no work")
    print("M227_CUDA_ARCH=PASS sm_86 cubin present; only source files in provenance manifest were compared")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
