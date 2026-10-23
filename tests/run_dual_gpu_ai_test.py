#!/usr/bin/env python3
"""Run the real CUDA/Qwen + AMD extension only with exclusive GPU ownership.

Arguments match run_ai_response_service_test.py. Supply --real-inference and
--pom-cuda; this wrapper generates and validates --pom-vulkan itself.
It never stops a worker or the node. Use a persistent memory-limited,
Delegate=memory service named nexus-dual-*.service. Scoped file-cache reclaim
is enabled by default; NEXUS_TEST_FILE_CACHE_RECLAIM=0 retains the old profile.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
from contextlib import nullcontext

from scoped_file_cache import ScopedFileCache

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "scripts"))
sys.path.insert(0, str(SOURCE / "experiments/vulkan_jit"))
from nexus_gpu_devices import refresh_capabilities
from nexus_vulkan_runtime import session
from walk_probe import validate_manifest


def main() -> int:
    args = sys.argv[1:]
    reclaim = os.environ.get("NEXUS_TEST_FILE_CACHE_RECLAIM", "1")
    if reclaim not in ("0", "1"):
        raise ValueError("NEXUS_TEST_FILE_CACHE_RECLAIM must be 0 or 1")
    if len(args) < 6 or "--real-inference" not in args or "--pom-cuda" not in args:
        raise ValueError("Supply the existing real CUDA/Qwen test arguments")
    if "--pom-vulkan" in args:
        raise ValueError("The wrapper owns the JIT module arguments")
    if os.environ.get("CUDA_VISIBLE_DEVICES") != "GPU-00000000-0000-0000-0000-000000000000":
        raise ValueError("The authorized primary NVIDIA GPU UUID must be exclusive")
    # Match the working 8 GiB production profile. Leaving this unset makes the
    # stock runtime start at 32768 tokens and can consume PoM's VRAM reserve.
    if os.environ.get("KERYX_LLAMA_CTX") != "8192":
        raise ValueError("Set KERYX_LLAMA_CTX=8192 to match the production context budget")
    for executable in Path("/proc").glob("[0-9]*/exe"):
        try:
            name = executable.resolve(strict=True).name
        except (OSError, RuntimeError):
            continue
        if name in ("nexus-live-cuda-worker", "keryx-miner"):
            raise ValueError("An existing miner is active; no second NVIDIA CUDA process is allowed")
    temporary = Path(os.environ.get("TMPDIR", "")).resolve()
    if not os.environ.get("TMPDIR") or temporary == Path("/tmp") or Path("/tmp") in temporary.parents:
        raise ValueError("A persistent TMPDIR outside /tmp is required")
    root = Path(args[5]).resolve()
    before = set(root.glob("run-*/native.log"))
    refresh_capabilities()
    with session("pci:0000:00:00.0", "offline-walk") as gpu:
        banked = os.environ.get("NEXUS_TEST_AMD_RESIDENT") == "1"
        module = gpu.prepare_module(SOURCE / ("consensus/cuda_portable_resident.cu" if banked else "consensus/cuda_walk_kernels.cu"),
                                    "pom_v4_resident_walk_kernel" if banked else "pom_v4_walk_kernel",128,bounded_indices_proven=True,banked_model_tiles=banked)
        validate_manifest(module,banked=banked)
        manifest = json.loads(Path(module["manifest"]).read_text())
        command = [sys.executable, "-B", str(SOURCE / "tests/run_ai_response_service_test.py"),
                   *args, "--pom-vulkan", module["spirv"], manifest["spirv_sha256"]]
        # Test-only reclaim of mapped file cache. The guard validates
        # its dedicated delegated cgroup before any native model/GPU execution.
        guard = ScopedFileCache() if reclaim == "1" else nullcontext()
        with guard:
            result = subprocess.run(list(map(str, command)), env=gpu.environment(), check=False)
        if result.returncode:
            return result.returncode
    created = set(root.glob("run-*/native.log")) - before
    if len(created) != 1:
        raise ValueError("Expected exactly one new native test log")
    log = created.pop().read_text()
    if ("AMD_AI_LIFECYCLE=PASS" not in log or "AI_RESPONSE_MOCK_E2E=PASS" not in log
            or log.count("AI_REQUEST_POM_RESUME=PASS") != 2):
        raise ValueError("Combined test did not complete both real preemption cycles")
    print("DUAL_GPU_QWEN_INTEGRATION=PASS mainnet_test_transactions=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"DUAL_GPU_QWEN_INTEGRATION=BLOCKED reason={exc}", file=sys.stderr)
        raise SystemExit(1)
