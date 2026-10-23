# Native Windows CLI preview

This port builds an x64 Windows executable, independent Rust verifier and CUDA
inference DLL. It uses Windows APIs directly; WSL and MSYS2 are not required.
It is an experimental CLI distribution. The Linux desktop/service controls and
AMD/Vulkan execution are not included in the Windows package.

The GPU path is restricted to NVIDIA **SM 8.6** by the existing kernels and
runtime admission. Selecting a different UUID does not add support for another
GPU architecture. This source baseline has incomplete H15 support. Do not treat
this preview as an equivalent replacement for SUPR 0.15.0 or as a validated
post-H15 mainnet miner. CPU tests and a successful build do not establish GPU
proof parity, inference preemption or accepted on-chain submissions on Windows.

The companion Linux archive uses Ubuntu 24.04 as its build baseline, requires
compatible glibc/libstdc++ and a CUDA 13 NVIDIA driver, and uses the same explicit
Python launcher. Its GPU execution is also unvalidated as a packaged artifact.

## Package contents and requirements

The ZIP contains `nexus-live-cuda-worker.exe`, `nexus-m21-oracle.exe`,
`keryx-llama.dll`, the matching CUDA redistributables, a Python launcher,
licenses, `BUILD.json` (exact source commit and validation scope) and checksums.
It does not contain GPU drivers, model weights, Merkle trees, wallets, keys,
node/IPFS databases or an existing escrow journal. Keep private data outside the
extracted archive and outside the Git checkout.

Use Windows 10/11 x64 with a driver compatible with CUDA 13, an SM 8.6 NVIDIA
GPU, Python 3.11+ for the optional launcher and the Microsoft Visual C++ x64
runtime (included DLLs when available). A synced compatible node and local
Kubo API are external requirements. The model ID, model checksum, tree format,
wallet delegation, signing and recovery checks remain enforced by the worker.

Verify the archive with PowerShell:

```powershell
Get-FileHash .\nexus-keryx-miner-*-windows-x86_64-cuda-sm86-preview.zip -Algorithm SHA256
```

Compare the value with the accompanying `.sha256` file. After extraction,
`BUILD.json` identifies the corresponding source. From the extracted directory,
this command only prints compiled capabilities and does not initialize a GPU:

```powershell
.\nexus-live-cuda-worker.exe --inference-capabilities
```

## Explicit local configuration

Copy `profile.example.json` to private storage and fill its fields locally.
Use absolute paths for persistent data, model, tree, metadata, existing escrow
key and output directory. Obtain your selected UUID through `nvidia-smi -L`.
The all-zero example is inert and rejected. The launcher pins both
`NEXUS_PRIMARY_GPU_UUID` and `CUDA_VISIBLE_DEVICES` to your explicit selection;
the native backend checks the physical UUID and architecture again.

Existing escrow state must have a matching snapshot and journal and pass
recovery and permission checks. The historical backup import is deliberately
not a first-run initializer: do not copy another installation's journals or
create empty state to conceal missing recovery data. On Windows, the native
file adapter checks the owner and DACL; other accounts' access is rejected
except for SYSTEM. New native files receive a protected owner/SYSTEM DACL.

Once the external inputs and private state are ready, a bounded shadow run is:

```powershell
py -3 .\nexus_run.py --profile 'D:\NexusPrivate\profile.json'
```

This **does use the selected GPU** and inference runtime, while disabling block
and AI-response submissions. It is an explicit runtime test, not a build step.
The launcher enables live submissions only with `--live`, and escrow claims
additionally require `--enable-escrow-claims`. Read the H15 limitation above
before considering a live session. The launcher never generates a wallet,
replaces a key, imports state, starts a node or installs a background service.

The worker holds a native, machine-wide exclusive device mutex. Its verifier
runs as an owned child with an inheritance allowlist, private byte pipes,
bounded messages, a deadline and a kill-on-close job. Missing or invalid proof
responses fail closed. DLL dependencies are loaded from the explicit runtime
directory and Windows system directories, excluding arbitrary working-directory
DLL lookup. Model reads use native mapping and independent positional reads;
strict streaming uses aligned unbuffered I/O.

## Build from source

Install Visual Studio 2022 with Desktop development with C++, CMake 3.25+,
Python 3.11+, Git and Rust 1.88+. Build dependencies are pinned in
`tools/windows/vcpkg.json`; the workflow pins the vcpkg checkout too.

```powershell
git clone https://github.com/microsoft/vcpkg.git .local/vcpkg
git -C .local/vcpkg checkout 23b8a8e7409c2d6346114a88e834165bde79a51c
& .local/vcpkg/bootstrap-vcpkg.bat -disableMetrics
cmake -S . -B build/windows -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$pwd/.local/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_MANIFEST_DIR="$pwd/tools/windows" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build/windows --config Release --parallel 2
ctest --test-dir build/windows -C Release --output-on-failure
```

For the CUDA worker, install CUDA Toolkit 13 and configure a separate directory
with `-DVCPKG_MANIFEST_FEATURES=worker -DNEXUS_ENABLE_GRPC=ON
-DNEXUS_ENABLE_CUDA=ON -DBUILD_TESTING=OFF`; build the explicit
`nexus-live-cuda-worker` target. Build the oracle with `tools/fetch_upstream.py`
and `cargo build --release --locked --manifest-path oracle-rust/Cargo.toml`.
The runtime build is `cmake -S tools/runtime -B build/runtime -A x64`, followed
by `cmake --build build/runtime --config Release --target nexus_inference_runtime`.
The runtime fetches pinned llama.cpp source and applies the retained canonical
trace patch. Build outputs remain ignored.

`scan-root` and the oracle's full root audit retain Linux-specific host/process
health guards and explicitly refuse on Windows. The independent proof verifier
and the GGUF/Merkle parser do not bypass validation to compensate.
