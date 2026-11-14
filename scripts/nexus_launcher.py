#!/usr/bin/env python3
"""
NEXUS Universal Miner Launcher (Cross-Platform: Windows & Linux)
Manages and orchestrates native C++20 workers (CUDA / Vulkan) without systemd or shell dependencies.
"""

import argparse
import os
import platform
from pathlib import Path
import signal
import subprocess
import sys
import time

IS_WINDOWS = platform.system() == "Windows"


def find_executable(base_dir: Path, name: str) -> Path:
    exe_name = f"{name}.exe" if IS_WINDOWS else name
    candidates = [
        base_dir / "bin" / exe_name,
        base_dir / "bin" / "Release" / exe_name,
        base_dir / "build" / "bin" / exe_name,
        base_dir / "build" / "bin" / "Release" / exe_name,
        base_dir / "build" / "linux" / "bin" / exe_name,
        base_dir / "build" / "windows-worker" / "bin" / "Release" / exe_name,
        base_dir / "build" / "windows-worker" / "bin" / exe_name,
        base_dir / "oracle-rust" / "target" / "release" / exe_name,
        base_dir.parent / "KERYX-NEXUS-RECOVERY" / "artifacts" / "native-worker" / "nexus-live-cuda-worker",
        base_dir.parent / "KERYX-NEXUS-RECOVERY" / "artifacts" / "vulkan-native" / "nexus-live-vulkan-worker",
    ]
    for c in candidates:
        if c.is_file() and (IS_WINDOWS or os.access(c, os.X_OK)):
            return c.resolve()
    # Check PATH
    import shutil
    p = shutil.which(exe_name)
    if p:
        return Path(p).resolve()
    raise FileNotFoundError(f"Native worker '{exe_name}' not found. Build the project first via CMake.")


def parse_args():
    parser = argparse.ArgumentParser(description="Keryx NEXUS Cross-Platform Universal Miner")
    parser.add_argument("--address", type=str, required=True,
                        help="Keryx mining address (keryx:...)")
    parser.add_argument("--keryxd-address", type=str, default="127.0.0.1", help="keryxd node host")
    parser.add_argument("--port", type=int, default=22110, help="keryxd node RPC port (default: 22110)")
    parser.add_argument("--device", type=str, choices=["auto", "nvidia", "amd", "dual"], default="auto",
                        help="GPU selection: nvidia (CUDA), amd (Vulkan), dual, or auto-detect")
    parser.add_argument("--models-dir", type=str, default="", help="Path to models directory containing Qwen3.5-9B")
    parser.add_argument("--escrow-key-file", type=str, default="", help="Path to escrow.key")
    parser.add_argument("--escrow-cert", type=str, default="",
                        help="Escrow certificate hex")
    parser.add_argument("--ctx", type=int, default=8192, help="Keryx LLM Context window (default: 8192)")
    parser.add_argument("--vulkan-batch", type=int, default=2048, help="Batch size for Vulkan worker (default: 2048)")
    parser.add_argument("--pool-url", type=str, default="", help="Stratum pool URL (optional, e.g. stratum+tcp://pool.keryx.net:4444)")
    parser.add_argument("--pool-worker", type=str, default="", help="Stratum worker name (optional, defaults to address)")
    parser.add_argument("--pool-password", type=str, default="x", help="Stratum pool password (e.g. d=4 for fixed difficulty, default: x)")
    parser.add_argument("--vulkan-device", default="", help="Explicit AMD GPU UUID or pci:dddd:bb:ss.f identity")
    parser.add_argument("--vulkan-lib", type=str, default="",
                        help="Vulkan runtime library (default: keryx-llama-vk next to the Vulkan worker)")
    parser.add_argument("--oracle", type=str, default="",
                        help="Independent Rust verifier (default: nexus-m21-oracle from the build)")
    parser.add_argument("--live", action="store_true",
                        help="Authorize the Vulkan worker to submit shares/blocks (verified by --oracle first)")
    parser.add_argument("--enable-ai-response", action="store_true",
                        help="Let the Vulkan worker serve the pool's traced AI (H15) requests")
    return parser.parse_args()


def detect_available_gpus():
    has_nvidia = False
    has_amd = False
    if IS_WINDOWS:
        # Check via WMIC or PowerShell
        try:
            res = subprocess.run(["wmic", "path", "win32_VideoController", "get", "name"],
                                 capture_output=True, text=True, timeout=5)
            out = res.stdout.lower()
            has_nvidia = any(name in out for name in ("nvidia", "geforce", "rtx"))
            has_amd = any(name in out for name in ("amd", "radeon"))
        except Exception:
            has_nvidia = True
    else:
        # Linux detection
        if Path("/dev/nvidia0").exists() or shutil_which("nvidia-smi"):
            has_nvidia = True
        if list(Path("/sys/class/drm").glob("card*")) and any("amdgpu" in p.read_text() for p in Path("/sys/class/drm").glob("card*/device/uevent") if p.is_file()):
            has_amd = True
    return has_nvidia, has_amd


def shutil_which(cmd):
    import shutil
    return shutil.which(cmd) is not None


def main():
    args = parse_args()
    root_dir = Path(__file__).resolve().parent.parent

    # Resolve default paths if not specified
    models_dir = Path(args.models_dir) if args.models_dir else (root_dir.parent / "KERYX" / "models")
    escrow_key = Path(args.escrow_key_file) if args.escrow_key_file else (root_dir.parent / "KERYX" / "state" / "escrow.key")

    print("=================================================================")
    print(f" NEXUS Universal Miner (OS: {platform.system()} {platform.machine()})")
    print(f" Target Node: grpc://{args.keryxd_address}:{args.port}")
    print(f" Mining Address: {args.address}")
    print(f" LLM Context Window: {args.ctx} (Safe mainnet floor)")
    print("=================================================================")

    target_gpus = args.device
    if target_gpus == "auto":
        nv, amd = detect_available_gpus()
        if nv and amd:
            target_gpus = "dual"
        elif nv:
            target_gpus = "nvidia"
        elif amd:
            target_gpus = "amd"
        else:
            target_gpus = "nvidia"
        print(f"Auto-detected GPUs: {target_gpus.upper()}")

    processes = []

    def terminate_all(*_):
        print("\n[Launcher] Shutting down workers gracefully...")
        for p, name in processes:
            if p.poll() is None:
                print(f"[Launcher] Terminating {name} (PID: {p.pid})...")
                p.terminate()
        for p, name in processes:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        print("[Launcher] All workers stopped cleanly.")
        sys.exit(0)

    signal.signal(signal.SIGINT, terminate_all)
    signal.signal(signal.SIGTERM, terminate_all)

    env = os.environ.copy()
    env["KERYX_LLAMA_CTX"] = str(args.ctx)
    if args.pool_url:
        env["NEXUS_MINING_MODE"] = "pool"
        env["NEXUS_POOL_URL"] = args.pool_url
        env["NEXUS_POOL_WORKER"] = args.pool_worker or args.address
        env["NEXUS_POOL_PASSWORD"] = args.pool_password

    # 1. NVIDIA Worker (CUDA)
    if target_gpus in ("nvidia", "dual"):
        try:
            cuda_bin = find_executable(root_dir, "nexus-live-cuda-worker")
            cmd = [
                str(cuda_bin),
                "--mining-address", args.address,
                "--keryxd-address", args.keryxd_address,
                "--port", str(args.port),
                "--models-dir", str(models_dir),
                "--escrow-key-file", str(escrow_key),
                "--escrow-cert", args.escrow_cert,
            ]
            print(f"[Launcher] Launching NVIDIA CUDA Worker: {cuda_bin.name}")
            p_nv = subprocess.Popen(cmd, env=env)
            processes.append((p_nv, "NVIDIA Worker"))
        except Exception as e:
            print(f"[Launcher] Warning: Could not launch NVIDIA worker: {e}", file=sys.stderr)

    # 2. AMD / Vulkan Worker
    if target_gpus in ("amd", "dual"):
        try:
            if not args.vulkan_device:
                raise ValueError("--vulkan-device must name the AMD GPU UUID or PCI identity")
            vk_bin = find_executable(root_dir, "nexus-live-vulkan-worker")
            vk_env = env.copy()
            vk_env["NEXUS_VULKAN_BATCH_SIZE"] = str(args.vulkan_batch)
            vk_env["KERYX_VK_WALK_NPG"] = "2"
            vk_env["KERYX_VK_V5_DOT"] = "1"
            if not IS_WINDOWS:
                if Path("/usr/share/vulkan/icd.d/radeon_icd.json").exists():
                    vk_env["VK_DRIVER_FILES"] = "/usr/share/vulkan/icd.d/radeon_icd.json"
                    vk_env["VK_ICD_FILENAMES"] = "/usr/share/vulkan/icd.d/radeon_icd.json"
                vk_env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"

            node_url = args.pool_url if args.pool_url else f"grpc://{args.keryxd_address}:{args.port}"
            runtime_name = "keryx-llama-vk.dll" if IS_WINDOWS else "libkeryx-llama-vk.so"
            vulkan_lib = Path(args.vulkan_lib) if args.vulkan_lib else vk_bin.parent / runtime_name
            cmd = [
                str(vk_bin),
                "--keryxd-address", node_url,
                "--models-dir", str(models_dir),
                "--escrow-key", str(escrow_key),
                "--escrow-cert", args.escrow_cert,
                "--address", args.address,
                "--extra-data", "NEXUS v0.0.5",
                "--vulkan-lib", str(vulkan_lib.resolve()),
                "--device", args.vulkan_device,
            ]
            if args.pool_url:
                cmd.extend([
                    "--worker", args.pool_worker or args.address,
                    "--pool-password", args.pool_password,
                ])
            if args.live:
                oracle = Path(args.oracle) if args.oracle else find_executable(root_dir, "nexus-m21-oracle")
                cmd.extend(["--oracle", str(oracle.resolve()), "--enable-submit"])
            if args.enable_ai_response:
                cmd.append("--enable-ai-response")
            print(f"[Launcher] Launching AMD/Cross-Vendor Vulkan Worker: {vk_bin.name}")
            p_vk = subprocess.Popen(cmd, env=vk_env)
            processes.append((p_vk, "AMD Vulkan Worker"))
        except Exception as e:
            print(f"[Launcher] Warning: Could not launch AMD Vulkan worker: {e}", file=sys.stderr)

    if not processes:
        print("[Launcher] Error: No worker could be started. Exiting.", file=sys.stderr)
        sys.exit(1)

    print("[Launcher] Workers active. Press Ctrl+C to stop.")

    # Supervise loop
    while True:
        for p, name in processes:
            code = p.poll()
            if code is not None:
                print(f"[Launcher] Worker {name} exited with code {code}.")
                terminate_all()
        time.sleep(1)


if __name__ == "__main__":
    main()
