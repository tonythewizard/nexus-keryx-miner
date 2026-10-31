# GeForce RTX 30, 40 and 50 CUDA candidate

The source build includes native SM 8.6 (RTX 30), SM 8.9 (RTX 40) and
SM 12.0 (RTX 50) images for both the PoM walk and resident solver. The
Driver API selects the matching image from each embedded fatbin. Symbol
names retain their historical `cubin` spelling for host linkage only.
The inference runtime uses the same architecture list from
`tools/cmake/cuda-architectures.cmake`; rebuild it as well as the worker.
CUDA Toolkit 13 or newer is required, with a compatible NVIDIA driver.

Device admission still requires an explicitly configured
`NEXUS_PRIMARY_GPU_UUID` and `NEXUS_PRIMARY_GPU_NAME`, an exact match to the
physical device, one visible UUID in `CUDA_VISIBLE_DEVICES`, and exclusive
ownership. UUID selection does not waive model, architecture, context or
proof checks. Unsupported architectures fail closed. This does not add
arbitrary multi-GPU scheduling.

Architecture inclusion is not device validation. Desktop and laptop cards
with these capabilities can differ substantially in VRAM and power limits.
Memory admission, model residency, AI preemption, byte parity, independent
Rust proof verification and target checks remain mandatory. Smaller cards
may correctly fail memory admission; no new low-VRAM fallback is provided.

Offline validation for this change: both kernel images compiled for all
three architectures using CUDA 13.4; cuobjdump confirmed all three native
images in each fatbin. Host CUDA sources passed C++ syntax checking. The
CPU configuration test exercises all three admitted architectures, rejects
other architectures and rejects missing/mismatched configured model names.
No GPU jobs, inference, mining, submissions or service restarts were run.
On 2026-10-31 the isolated Linux worker and ABI9 inference runtime both built
successfully. The runtime ABI/export/null-handle checks and 31 distribution
tests passed. Native inference images were inspected as SM86, SM89 and
SM120a (the pinned llama.cpp selects the Blackwell-specific variant).
Hardware parity on RTX 40/50 remains unvalidated. Existing
SM86-only binaries and historical M2.x diagnostic/build scripts do not gain
support from these source changes. Production activation requires a newly
built worker/runtime pair and device-specific validation.

Architecture mapping: https://developer.nvidia.com/cuda/gpus
