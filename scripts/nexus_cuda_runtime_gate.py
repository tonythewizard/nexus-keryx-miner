#!/usr/bin/env python3
"""Fail closed unless PoM is Driver-only and stock llama resolves CUDA 12."""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

CUDA12 = re.compile(r"libcudart\.so\.12(?:\s|$)")
CUDA13 = re.compile(r"libcudart\.so\.13(?:\s|$)")


def run(*argv: str) -> str:
    # readelf/ldd translate diagnostic labels according to the caller's locale.
    # The systemd --user manager may retain an Italian locale even when an
    # interactive shell uses English; keep this security gate deterministic.
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    result = subprocess.run(argv, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(argv)}\n{result.stdout}")
    return result.stdout


def dynamic_needed(path: Path) -> tuple[str, str]:
    header = run("readelf", "-h", str(path))
    dynamic = run("readelf", "-d", str(path))
    if "ELF64" not in header or "Advanced Micro Devices X86-64" not in header:
        raise RuntimeError(f"not an x86_64 ELF64 binary: {path}")
    needed = "\n".join(re.findall(r"Shared library: \[([^]]+)\]", dynamic))
    return header, needed


def resolved(path: Path) -> str:
    closure = run("ldd", "-r", str(path))
    if "not found" in closure or "undefined symbol" in closure:
        raise RuntimeError(f"unresolved ELF dependency for {path}:\n{closure}")
    return closure


def main() -> int:
    if len(sys.argv) != 3:
        print("CUDA_RUNTIME_GATE=BLOCKED usage: nexus_cuda_runtime_gate.py WORKER STOCK_LLAMA", file=sys.stderr)
        return 2
    worker, llama = (Path(arg).resolve(strict=True) for arg in sys.argv[1:])
    try:
        _, worker_needed = dynamic_needed(worker)
        if "libcuda.so.1" not in worker_needed:
            raise RuntimeError("worker does not directly depend on the CUDA Driver API library")
        if CUDA12.search(worker_needed) or CUDA13.search(worker_needed):
            raise RuntimeError("worker has a dynamic CUDA Runtime dependency")
        worker_closure = resolved(worker)
        if CUDA12.search(worker_closure) or CUDA13.search(worker_closure):
            raise RuntimeError("worker transitive ELF closure contains a CUDA Runtime")
        if "libcuda.so.1 => " not in worker_closure:
            raise RuntimeError("CUDA Driver API library is not resolved")

        _, llama_needed = dynamic_needed(llama)
        if "libcudart.so.12" not in llama_needed:
            raise RuntimeError("stock llama does not declare CUDA Runtime 12")
        if CUDA13.search(llama_needed):
            raise RuntimeError("stock llama declares CUDA Runtime 13")
        llama_closure = resolved(llama)
        expected_runtime = str(llama.parent / "libcudart.so.12")
        if f"libcudart.so.12 => {expected_runtime}" not in llama_closure:
            raise RuntimeError("stock CUDA Runtime 12 did not resolve through the wrapper's own directory")
        if CUDA13.search(llama_closure):
            raise RuntimeError("stock llama transitive closure contains CUDA Runtime 13")
        for library in ("libcublas.so.12", "libcublasLt.so.12"):
            if library not in llama_closure or "not found" in llama_closure:
                raise RuntimeError(f"stock CUDA 12 math dependency is not resolved: {library}")
    except (OSError, RuntimeError) as exc:
        print(f"CUDA_RUNTIME_GATE=BLOCKED detail={exc}", file=sys.stderr)
        return 13
    print("CUDA_RUNTIME_GATE=PASS worker=CUDA_DRIVER_API llama=CUDA_RUNTIME_12 mixed_runtime=NONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
