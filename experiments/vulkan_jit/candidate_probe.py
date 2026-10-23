#!/usr/bin/env python3
"""Run the bounded AMD candidate probe: mainnet reads, loopback writes only."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "scripts"))
from nexus_gpu_devices import refresh_capabilities
from nexus_vulkan_runtime import session
from walk_probe import validate_manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("native", "config", "model", "tree", "meta", "oracle", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    temporary = Path(os.environ.get("TMPDIR", "")).resolve()
    if not os.environ.get("TMPDIR") or temporary == Path("/tmp") or Path("/tmp") in temporary.parents:
        parser.error("TMPDIR must select a persistent directory outside /tmp")
    if args.out.exists():
        parser.error("--out must be a new persistent evidence directory")
    refresh_capabilities()
    with session("pci:0000:00:00.0", "offline-walk") as gpu:
        module = gpu.prepare_module(
            SOURCE / "consensus/cuda_walk_kernels.cu", "pom_v4_walk_kernel", 128,
            bounded_indices_proven=True,
        )
        validate_manifest(module)
        manifest = json.loads(Path(module["manifest"]).read_text())
        command = [args.native, args.config, args.model, args.tree, args.meta,
                   module["spirv"], manifest["spirv_sha256"], args.oracle, args.out]
        # The native RPC context also has a 10 second capture deadline. No
        # worker, NVIDIA context, Qwen runtime or mainnet submission is started.
        result = subprocess.run([str(v) for v in command], env=gpu.environment(),
                                text=True, capture_output=True, timeout=90)
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "native.log").write_text(result.stdout + result.stderr)
        for line in (result.stdout + result.stderr).splitlines():
            if line.startswith(("REAL_TEMPLATE_AMD=", "AMD_CANDIDATE_PIPELINE=", "AMD_THREADED_LIFECYCLE=")):
                print(line)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
