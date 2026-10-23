#!/usr/bin/env python3
"""Exclusive offline AMD replay; persistent paths and delegated test scope required."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(SOURCE / "scripts"), str(SOURCE / "tests")]
from nexus_gpu_devices import refresh_capabilities
from nexus_vulkan_runtime import session
from scoped_file_cache import ScopedFileCache
from translate import compile_kernel
from walk_probe import validate_manifest
from banked_source import generate


def main():
    parser = argparse.ArgumentParser()
    for name in ("binary", "model", "tree", "context", "trace", "oracle", "evidence"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--tune-batches", action="store_true")
    args = parser.parse_args()
    if not os.environ.get("TMPDIR") or Path(os.environ["TMPDIR"]).resolve().is_relative_to("/tmp"):
        raise ValueError("Persistent TMPDIR required")
    for proc in Path("/proc").glob("[0-9]*/exe"):
        try:
            if proc.resolve(strict=True).name in ("nexus-live-cuda-worker", "keryx-miner"):
                raise ValueError("An existing miner is active")
        except (OSError, RuntimeError):
            continue
    args.evidence.mkdir(parents=True, exist_ok=False)
    cache = SOURCE.parent / "amd-resident-correctness/jit-cache"
    # Source-derived AST/IR and SPIR-V validation, not a fixed GLSL replacement.
    derived, origin = generate(SOURCE / "consensus/cuda_portable_resident.cu", cache)
    resident = compile_kernel(derived,"pom_v4_banked_resident_walk_kernel",cache,128,True)
    resident["origin"] = origin
    gathered = compile_kernel(SOURCE / "consensus/cuda_walk_kernels.cu",
                              "pom_v4_walk_kernel", cache, 128, True)
    validate_manifest(resident, banked=True)
    validate_manifest(gathered)
    r = json.loads(Path(resident["manifest"]).read_text())
    g = json.loads(Path(gathered["manifest"]).read_text())
    modules = args.evidence / "gathered-module.txt"
    modules.write_text(gathered["spirv"] + "\n" + g["spirv_sha256"] + "\n")
    refresh_capabilities()
    with ScopedFileCache(), session("pci:0000:00:00.0", "offline-walk") as gpu:
        command = [args.binary, resident["spirv"], r["spirv_sha256"], args.model,
                   args.tree, args.context, args.trace, args.evidence / "proof", modules]
        if args.tune_batches:
            command.append("--tune-batches")
        with (args.evidence / "native.log").open("w") as log:
            subprocess.run(list(map(str, command)), env=gpu.environment(), stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        with (args.evidence / "rust.log").open("w") as log:
            subprocess.run([str(args.oracle), "verify-real", str(args.context), str(args.trace),
                            str(args.evidence / "proof/resident.proof")], stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    (args.evidence / "artifacts.json").write_text(json.dumps({
        "modules": [resident, gathered],
        "sha256": {str(p): hashlib.file_digest(p.open("rb"), "sha256").hexdigest()
                   for p in (args.binary, args.oracle, args.evidence / "proof/resident.proof")},
    }, indent=2) + "\n")
    print((args.evidence / "native.log").read_text())
    print((args.evidence / "rust.log").read_text())


if __name__ == "__main__":
    main()
