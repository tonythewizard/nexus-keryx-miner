# Developer kit

## Start here

This repository contains the miner's source, tests, vendored protocol code and
build definitions. It has a fresh Git history and no installation state. It is
not a portable binary release or a copy of a running installation. `VERSION`
describes the source baseline; it does not certify a newly compiled executable.

Before changing code, read the relevant subsystem and its tests, then run the
smallest checks that cover the change. Builds and offline tests must not start
mining, use a production GPU or change wallets. Automation-specific guidance
is kept in `AGENTS.md`.

## Architecture and ownership

| Area | Entry points and responsibility |
| --- | --- |
| Worker | `src/model/live_cuda_worker.cpp`: startup admission, job processing, scheduling, proofs, AI priority, telemetry and shutdown |
| Consensus | `consensus/pom_v4.cpp`, `include/keryx/nexus/consensus/`: canonical headers, targets, PoM walks, witness encoding and verification boundaries |
| Model/tree | `src/model/gguf_reader.cpp`: read-only GGUF access, bounded tile reads and Merkle indexing; `gguf_main.cpp` provides inspection tools |
| CUDA | `consensus/cuda_walk.cpp`, `cuda_resident.cpp`, `*.cu`: device code and Driver API host integration |
| Vulkan | `src/gpu/vulkan_walk_engine.cpp`, `vulkan_bda_resident_engine.cpp`: JIT/resident execution and explicit device admission |
| JIT | `experiments/vulkan_jit/ast_lower.py`, `translate.py`, `banked_source.py`: restricted CUDA-source lowering into GLSL/SPIR-V; unsupported patterns must fail closed |
| Lane coordination | `src/gpu/gpu_work_coordinator.cpp`, `secondary_work_lane.cpp`, corresponding headers: nonce ownership, cancellation, pause/drain and preemption |
| Node/RPC | `src/network/grpc_client.cpp`, `grpc_header.cpp`, `live_job_queue.cpp`: templates, chain changes, reconnection and current job leases |
| Submissions | `src/network/submission_pipeline.cpp`, `submit_block_client.cpp`, `include/keryx/nexus/network/`: full block assembly, explicit authorization, deduplication and node results |
| AI service | `src/network/ai_service.cpp`, `ai_response_service.cpp`, `opoi.cpp`, `trace_capture.cpp`: discovery, assignment, deadlines, inference adapters, response processing and trace boundaries |
| Rust bridge | `ai-response-bridge/`: exact formats, signatures, escrow, trace codecs, persistence and recovery; linked as a static library |
| Escrow | `src/network/escrow_watcher.cpp`, `vendor/keryx-miner-v057/src/escrow.rs`: ownership/CSV rules, signing, UTXO reconciliation and claims |
| Independent oracle | `oracle-rust/`: verification against pinned public node crates, separate from the optimized worker |
| Desktop | `nexus-ui-desktop/src-tauri/`, `web/`: Rust/Tauri window and tray, embedded HTML/JS/CSS |
| Presentation | `scripts/nexus_dashboard.py`, `nexus_presenter.py`, `nexus_web_mirror.py`, `nexus_tauri_bridge.py`: shared Python model and typed desktop stdio bridge |
| Device controls | `scripts/nexus_gpu_{devices,identity,registry,selection,switch}.py`, `nexus_workload_admission.py`: stable identity, vendor policy, saved preferences and guarded handoff |
| Setup/wallet | `scripts/nexus_setup.py`, `nexus_wallet_authorization.py`: private profiles, certificate verification and identity protection |
| API/MCP | `scripts/nexus_agent_api.py`, `nexus_mcp.py`, `nexus_mcp_control.py`, `agent_web/`: scoped commands, loopback endpoints and private authentication |
| Service controls | `scripts/nexus_restart_workers.py`, `nexus_stop_workers.py`, `nexus_desktop_commands.py`: Linux service integration and cooperative lifecycle checks |
| Notifications/updates | `scripts/nexus_notifications.py`, `nexus_updates.py`: notification policy and legacy release updater. Its private-repository precondition is incompatible with this public source repository; do not enable it for this distribution |
| Experiments | `experiments/`: probes and performance/reference work, not separate production backends or automatic build acceptance |
| Wire formats | `proto/current/`, `proto/v057/`: generated C++ interfaces come from these definitions at build time |
| Dependencies | `vendor/` and `THIRD_PARTY.md`: vendored source, pinned upstream metadata and retained notices |
| Publication | `tools/security_gate.py`, `.githooks/`, `.github/workflows/`: privacy, secret and history gates |

The productive path is: current template → leased canonical job → unique nonce
→ full PoM walk → witness → independent Rust verification → real target check
→ current payout/delegation binding → block assembly → RPC submission → chain
reconciliation. A stale job is discarded; an inconsistent binding is an error.
`TARGET_FOUND`, `SUBMIT_SENT`, `RPC_ACKNOWLEDGED`, chain observation and selected
chain confirmation are distinct states.

AI work is discovered and checked for assignment, model and deadline before it
preempts new PoM batches. The worker drains the affected GPU work, runs inference,
signs/submits an authorized response, journals the result and resumes mining.
Preserve journals across restarts. An RPC acknowledgement is not proof of a paid
response. Escrow claims must retain ownership, maturity, UTXO and duplicate-spend
checks; a below-threshold pool may correctly produce no claim.

## Platform boundaries

| Platform | Current status |
| --- | --- |
| Linux x86-64 | Build baseline. Uses POSIX APIs, `/proc`, sysfs, `flock`, dynamic loading and optional systemd user services. |
| Windows | No validated native build. Port process/locking/memory/service code, ELF embedding and compiler flags before treating it as supported. WSL2 is a separate Linux environment, with its own driver/runtime limitations. |
| macOS | No validated build. No NVIDIA CUDA backend; POSIX/Linux differences and Vulkan availability require platform work. The presence of Tauri platform conditionals is not full miner support. |

Compile for your own platform and toolchain. No OS support claim should be
inferred from the repository being source-only. The existing native CUDA kernels
target SM 8.6 and use an ELF x86-64 embedding recipe. Vulkan admission retains
specific vendor/device and feature checks: changing a UUID does not add support
for a new GPU model. Vendor options in the UI are broader than tested execution.
The worker has at most two admitted lanes; arbitrary multi-GPU Rig scheduling is
not implemented. BDA requires its advertised integer/storage/address/subgroup
and cooperative-matrix features; there is no automatic portable fallback.

## Dependencies and local workspace

Base build: CMake 3.25+, a C++20 compiler, Ninja or Make, Python 3.11+, OpenSSL
Crypto development files, the BLAKE3 C library/headers and libb2 headers/library.
Install those with your OS's development packages or a private build prefix.
If CMake cannot find BLAKE3/libb2, pass `BLAKE3_INCLUDE_DIR`, `BLAKE3_LIBRARY`,
`BLAKE2_INCLUDE_DIR`, `BLAKE2_LIBRARY` explicitly. Do not commit local paths.

Additional components:

- gRPC: Protobuf and gRPC C++ development packages with CMake config files,
  `protoc`, the gRPC C++ code generator, libcurl, zlib and Cargo/Rust 1.88+.
- CUDA: compatible NVIDIA driver and CUDA Toolkit 13 for this build definition.
  PoM host calls use the Driver API; the external legacy inference wrapper has
  its own CUDA 12 runtime closure. Keep those requirements separate.
- Vulkan: loader, suitable ICDs, Vulkan headers and Clang/glslang/SPIR-V tools
  for the JIT pipeline. GPU execution still requires exclusive ownership.
- Desktop: Rust/Cargo and the Tauri 2 system dependencies for the target OS
  (on Linux: GTK/WebKitGTK development packages); Python 3 for the stdio adapter.
- MCP: Python packages pinned/constrained in `scripts/requirements-mcp.txt`.
- Publication: Git, Gitleaks 8.30.1+; GitHub CLI for the destination-checking hook.

From a fresh clone:

```sh
mkdir -p .local/tmp
export TMPDIR="$PWD/.local/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" PYTHONDONTWRITEBYTECODE=1
git config core.hooksPath .githooks
python3 tools/prepare_assets.py
python3 tools/security_gate.py
```

`tools/prepare_assets.py` checks `tools/source-assets.json` before decoding the
`.hex` protocol manifests and icon. Decoded files stay ignored. CMake calls it
automatically; run it explicitly before standalone Cargo/desktop builds.
Do not turn generated asset files into tracked files.

## CPU build and checks

```sh
cmake -S . -B build/cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEXUS_ENABLE_GRPC=OFF -DNEXUS_ENABLE_CUDA=OFF \
  -DNEXUS_ENABLE_VULKAN_JIT=OFF
cmake --build build/cpu --parallel 1
ctest --test-dir build/cpu --output-on-failure
```

This configuration builds CPU libraries/probes and deterministic tests. It
does not build or run a mining worker. Tests requiring an independent oracle,
authentic model, captured job, GPU or inference runtime need separately supplied
fixtures. Missing optional fixtures are not successful validation of that path.
Keep build directories separate by compiler/backend; do not mix old archives.

For wire/signing/escrow components, configure another build with
`-DNEXUS_ENABLE_GRPC=ON`. CMake builds `ai-response-bridge` with its lockfile.
The resulting ingress/probe tools are distinct from the CUDA mining worker.

## Independent Rust verification

```sh
python3 tools/fetch_upstream.py
cargo build --locked --release --manifest-path oracle-rust/Cargo.toml
mkdir -p .local/escrow-tests
export NEXUS_ESCROW_TEST_DIR="$PWD/.local/escrow-tests"
cargo test --locked --manifest-path ai-response-bridge/Cargo.toml
```

The oracle manifest resolves public node crates from ignored `_deps/keryx-node`.
The fetch helper pins an official upstream commit and refuses to replace a
different or dirty existing checkout. No locally modified node is included.
To register oracle-dependent CTest fixtures, pass the absolute built oracle path
as `-DNEXUS_RUST_ORACLE=...` when configuring. Authentic GGUF/tree/capture tests
also require `NEXUS_TEST_MODEL`, `NEXUS_TEST_TREE`, `NEXUS_TEST_TREE_META` and,
where used, `NEXUS_LIVE_CAPTURE_DIR`, `NEXUS_TEST_LAUNCHER`, `NEXUS_TEST_ESCROW_KEY`.
Use isolated test identities, not production wallet files.

## GPU build and runtime configuration

Keep the real hardware profile in environment variables and an ignored CMake
cache. Set `NEXUS_PRIMARY_GPU_UUID` to the intended NVIDIA UUID, and optionally
`NEXUS_SECONDARY_GPU_ID` to the selected `pci:...` identity. The committed zero
values are inert fixtures. C++ uses local CMake definitions; Python reads the
same names from the environment. Keep both values consistent.

```sh
# Set these variables privately before running this configuration.
cmake -S . -B build/gpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEXUS_ENABLE_GRPC=ON -DNEXUS_ENABLE_CUDA=ON \
  -DNEXUS_PRIMARY_GPU_UUID="$NEXUS_PRIMARY_GPU_UUID" \
  -DNEXUS_RECOVERY_ROOT="$NEXUS_DATA_DIR"
cmake --build build/gpu --target nexus-live-cuda-worker --parallel 1
```

`NEXUS_DATA_DIR` is a private persistent directory outside the checkout; Python
defaults to `~/.local/share/nexus-keryx-miner`. `NEXUS_RECOVERY_ROOT` is the
historical CMake spelling for the same runtime root, compiled into the worker.
The CPU-only build defaults to an isolated `build/cpu/runtime` instead.

The native worker's exact argument contract is in
`include/keryx/nexus/live_worker_config.hpp`. It requires endpoint, payout,
delegation extra data, model/tree/metadata, oracle, output directory, nonce and
batch limits, mode and explicit submission/AI flags. Live mode requires AI
responses. Claims require additional explicit authorization. Do not paste real
arguments into issues, CI or shared logs. `CUDA_VISIBLE_DEVICES` and
`NEXUS_POM_DEVICE_ID` must match the configured authorized UUID; native code
checks the actual identity again. Select Vulkan only through its explicit
backend/device/ICD and capability gates, without silent CUDA fallback.

Runtime inputs are external: official compatible model weights and Merkle tree,
an independent oracle, an inference wrapper matching the expected application
ABI, its CUDA/Vulkan dependency closure, verified wallet authorization and a
synced node. Set `NEXUS_ESCROW_KEY_FILE`, `NEXUS_OPOI_LIBRARY` and other runtime
adapter variables as required by the selected code path. Never substitute
`libllama.so` for the application wrapper or symlink incompatible CUDA SONAMEs.
Read-only model-root health probes additionally use `NEXUS_NODE_EXECUTABLE`.

Add `-DNEXUS_ENABLE_VULKAN_JIT=ON` in a separate build for Vulkan components;
optionally supply `NEXUS_VULKAN_HEADERS`. `consensus/vulkan-bda/` contains exact
recovered SPIR-V assembly and its manifest. The latest original GLSL is absent;
do not claim high-level source provenance or hardware support beyond the checks.

Building does not authorize launching a GPU, submitting transactions, replacing
services or stopping another miner. Test new GPU paths on an explicitly allocated
idle device, with parity and inference preemption checks on the exact executable.

## Desktop, API and local service integration

```sh
python3 tools/prepare_assets.py
cargo build --locked --manifest-path nexus-ui-desktop/src-tauri/Cargo.toml
# Set NEXUS_DATA_DIR privately and create it with owner-only permissions.
export NEXUS_SOURCE_DIR="$PWD"
nexus-ui-desktop/src-tauri/target/debug/nexus-desktop
```

The desktop embeds web assets; rebuilding is required after HTML/JS/CSS changes.
Python presentation/control modules are loaded from `NEXUS_SOURCE_DIR`. The
desktop bridge requires a matching existing `NEXUS_DATA_DIR`. Opening the window
is separate from starting mining. Controller modules contain Linux user-service
integration; no host-specific unit files, production launchers, shortcuts or
autostart configuration are shipped. Configure and validate a private service
adapter before using restart/stop/setup controls against a real installation.

The API defaults to loopback port 3340; MCP uses 3341 and separate enable state.
The current generated agent key grants diagnostics, development and live
control scopes together. Treat it as a privileged credential stored in private
local storage, never Git. Do not
publish credentials or expose listeners remotely as part of a source build.
Automatic binary updates default OFF. The updater requires an authenticated
*private* GitHub repository, so update checks cannot succeed against this
public source repository. Leave updates disabled; use reviewed source updates
and a fresh build. The update UI is not a portable installer. Older controllers
may still expect an installation-specific artifact layout.

## Tests and reporting

`tests/` contains C++ behavior tests, mock RPC/IPFS fixtures, Python controller
tests, DOM tests and explicit GPU harnesses. Some installation-policy regression
fixtures refer to older launcher/service contracts; adapt them with the relevant
integration instead of assuming they describe a ready-to-run installation.

```sh
python3 -m unittest discover -s tests/publication -v
node --test tests/desktop/tauri_dom_test.cjs
python3 tools/security_gate.py
# After staging only reviewed source paths:
python3 tools/security_gate.py --index
```

For Python controller tests, set persistent `TMPDIR` and an isolated data root,
then run the specific module. For C++ use CTest filters (`-R`) or the target's
test binary. Pure policy tests can run without CUDA initialization. Distinguish
syntax checks, deterministic CPU/mock tests, real GPU execution, oracle proof
parity and live network acceptance. A successful CPU build certifies none of the
last three. Full H15 traced inference, mainnet activation and arbitrary GPU
scheduling are not established by the available adapters or UI options.

Before a commit, review the diff and staged files. The security gate scans the
actual staged bytes; the push hook and CI scan reachable history as well. Refer
to `SECURITY.md` for scanner requirements, reviewed exceptions and publication.
