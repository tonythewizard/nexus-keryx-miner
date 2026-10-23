#!/usr/bin/env python3
"""Compile the trusted banked CUDA source at launch; never initialize a GPU."""
import os
import json
from pathlib import Path
import sys

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "experiments/vulkan_jit"))
from translate import compile_kernel
from walk_probe import validate_manifest
from banked_source import generate


def prepare():
    root = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
    cache = root / "vulkan-jit/cache"
    derived, origin = generate(SOURCE / "consensus/cuda_portable_resident.cu", cache)
    compiled = compile_kernel(derived, "pom_v4_banked_resident_walk_kernel", cache, 256, True)
    compiled["origin"] = origin
    validate_manifest(compiled, banked=True)
    manifest = json.loads(Path(compiled["manifest"]).read_text())
    return compiled, manifest


if __name__ == "__main__":
    if sys.argv[1:] != ["--runtime-lines"]:
        raise SystemExit("usage: nexus_prepare_secondary.py --runtime-lines")
    compiled, manifest = prepare()
    print(compiled["spirv"])
    print(manifest["spirv_sha256"])
