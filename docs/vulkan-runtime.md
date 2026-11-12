# Vulkan runtime for AMD GPUs

`nexus-live-vulkan-worker` contains no GPU code of its own. It loads a
`keryx_llama` runtime library (`src/network/opoi.cpp`) and calls it for
inference and for the PoM walk. `tools/runtime-vulkan` builds that library:
`keryx-llama-vk.dll` on Windows, `libkeryx-llama-vk.so` on Linux.

The runtime combines llama.cpp's Vulkan backend, in-process, with the PoM v4/v5
walk and the traced evaluations of keryx-miner-rdna3 (`vendor/keryx-miner-rdna3`,
unmodified and pinned in its `UPSTREAM.json`). It exports ABI 9:

- plain generation, which answers OPoI challenges;
- traced generation with replays and sequence leaves, which answers the pool's
  H15 AI requests (see [Pool AI](#pool-ai));
- the PoM functions.

The worker re-walks every GPU winner on the host and verifies the proof with
the independent Rust verifier before submitting it. A GPU fault therefore costs
shares, never validity. A winner the host walk rejects is logged as
`AMD_POOL_HOST_REWALK_REJECTED` (pool) or `AMD_HOST_REWALK_REJECTED` (solo).

## How the walk reaches the weights

| Device | Walk | Extra VRAM |
| --- | --- | --- |
| llama.cpp enabled integer dot products (expected on RDNA2–RDNA4) | Reads llama.cpp's resident weight buffers in place | Context pad only |
| No accelerated integer dot product (expected on RDNA1) | Streams the canonical layout into a device of its own | A second copy of the weights |

A borrowed `VkDevice` cannot report which features it was created with, so
`tools/runtime-vulkan/src/device.rs` repeats ggml-vulkan's rules. The
matrix-core kernel (int8 16x16x16 cooperative matrices at wave32) runs only
where llama.cpp enabled cooperative matrices: RDNA3 and RDNA4 on AMD's drivers.
Everywhere else the walk uses the `v_dot4` kernel. Before mining from
llama.cpp's buffers, the runtime checks that the walk addresses exactly the
GGUF's canonical chunks: the count must match, and 256 sampled chunks must match
the file byte for byte.

The tier-0 model needs about 6.5 GiB for weights and 424 MiB for the H16 context
pad, plus llama.cpp's buffers. The dedicated-copy path doubles the weights, so
it does not fit 8 GiB cards.

## Validation

Hardware coverage is one device. On an RX 7900 XT (RDNA3), Windows 10 21H2 and
AMD driver 26.10.1, `cargo run --release --example selftest` produced the
following with the tier-0 GGUF:

| Path | Rate |
| --- | --- |
| Matrix cores over llama.cpp's weights | 1.69 M nonces/s |
| `v_dot4` over llama.cpp's weights (`KERYX_POM_V4_KERNEL=dot`) | 1.11 M nonces/s |
| `v_dot4` over a dedicated copy (`GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1 GGML_VK_DISABLE_COOPMAT=1`) | 1.22 M nonces/s |

- All three paths returned identical lowest winners for the same synthetic jobs.
- The resident walk addressed 203,469,888 chunks, the tier-0 anchor count.
- Filling or refilling the context pad took 0.12 s.

`nexus-vulkan-runtime-parity-test` checks the runtime against Nexus's own
consensus code (`consensus/pom_v5.cpp`). It sends synthetic H16-era jobs
through `keryx_llama_pom_mine_v5`, then re-walks every nonce up to each GPU
winner on the host. The winner must meet the target and no earlier nonce may.
It passed for all three paths: 8 jobs and 2,180 host walks each. The test uses
the GPU, so it is not part of `ctest`. Build and run it explicitly:

```sh
cmake --build <build dir> --target nexus-vulkan-runtime-parity-test
nexus-vulkan-runtime-parity-test <runtime library> <tier-0 model.gguf> [jobs]
```

A live run of `nexus-live-vulkan-worker` on Suprnova (`krx.suprnova.cc:4401`)
had 26 shares accepted and none rejected. The Rust verifier checked every proof
before submission, and the host re-walk rejected no GPU winner. The worker
completed about 1.55 M nonces/s. That run did not serve AI requests, so Suprnova
flagged the rig `TRACE_INCAPABLE`.

With `--enable-ai-response`, the traced self-test of [Pool AI](#pool-ai)
produced 271 traced positions. Nexus's Rust verifier accepted the 14 replays it
opened, of per-position and sequence leaves both, and the worker qualified
(`POOL_AI_ROUTE=QUALIFIED`). In a 9-minute Suprnova run with it, 26 shares were
accepted and none rejected, at about 1.54 M nonces/s. The pool sent 35 AI
requests, all untraced, and the worker declined each one. No traced request
arrived in that time, so the pool has not yet exercised the traced answer path.

RDNA1 and RDNA2 have not been run on their own hardware. Their paths were
exercised only by forcing them on RDNA3.

## Build

Requirements:

- Rust 1.88+.
- `glslc` from the Vulkan SDK, found through `GLSLC`, `VULKAN_SDK` or `PATH`.
- libclang for bindgen, through `LIBCLANG_PATH`.
- CMake and Ninja for llama.cpp.

On Windows:

- Use the MSVC Rust toolchain (`+stable-x86_64-pc-windows-msvc`) and build from a
  Developer Prompt, so bindgen finds the C headers.
- Set `BINDGEN_EXTRA_CLANG_ARGS=--target=x86_64-pc-windows-msvc`.
- Keep the Cargo target directory short. llama.cpp's nested shader build
  otherwise exceeds MSVC's 260-character path limit.

```sh
CARGO_TARGET_DIR=<short dir> cargo build --release --locked \
  --manifest-path tools/runtime-vulkan/Cargo.toml
```

From CMake, `-DNEXUS_ENABLE_VULKAN_RUNTIME=ON` runs the same build and copies the
library next to the workers. `NEXUS_VULKAN_RUNTIME_TARGET_DIR` sets the Cargo
target directory.

## Run

The worker loads the official tier-0 model with its sparse tree and
`pom-tree.json` sidecar. In pool mode it needs no node and no IPFS daemon.
Submissions require the independent verifier and explicit authorization:

```sh
nexus-live-vulkan-worker --keryxd-address stratum+tcp://<pool>:<port> \
  --address keryx:<payout> --worker <name> \
  --model <model.gguf> --tree <pom-tree.bin> --meta <pom-tree.json> \
  --vulkan-lib <keryx-llama-vk library> --oracle <nexus-m21-oracle> --enable-submit \
  --enable-ai-response --device GPU-<AMD-device-UUID>
```

`scripts/nexus_launcher.py --device amd --pool-url ... --live --enable-ai-response`
builds the same command. It takes the runtime library from next to the worker unless
`--vulkan-lib` is given.

On Windows the worker's data directory defaults to
`%LOCALAPPDATA%\nexus-keryx-miner` when `NEXUS_DATA_DIR` is unset.

## Pool AI

Pools such as Suprnova send H15 AI requests to every rig. A rig that cannot
answer them is flagged `TRACE_INCAPABLE`, and the pool is penalized for its
shares. To serve them, add `--enable-ai-response` to the worker (or to
`nexus_launcher.py`). At startup the worker then:

1. runs a traced generation on the GPU and opens two seeds of it, which Nexus's
   Rust verifier must accept;
2. declares the model trace-capable to the pool.

If any step fails, the worker prints `POOL_AI_ROUTE=UNQUALIFIED` with the reason
and exits with code 78 instead of mining as a trace-incapable rig. Requests pause
the PoM walk while they run, as OPoI challenges do. Each capture is written to
disk before its answer is sent. Nexus answers only traced requests. Suprnova
also sends untraced chat requests, and the worker declines those with error 46
(busy) so the pool re-dispatches them at once.

Traced answers need the official prompt budget, so with `--enable-ai-response`
the context defaults to 8192 tokens instead of 512 (`KERYX_LLAMA_CTX` overrides
it). The captures live in the data directory, which must be private:

- On Linux, it must be owned by the user, with mode `0700`.
- On Windows, its owner must be the user, and its ACL may grant only the user
  and SYSTEM. A directory under `%LOCALAPPDATA%` inherits an Administrators
  entry, so remove inheritance once:

  ```bat
  icacls "%LOCALAPPDATA%\nexus-keryx-miner" /inheritance:r /grant:r "%USERNAME%:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F"
  icacls "%LOCALAPPDATA%\nexus-keryx-miner\*" /reset /T
  ```

  The second command makes existing files inherit the new ACL.

Environment variables:

| Variable | Effect |
| --- | --- |
| `KERYX_INFER_GPU` | Raw Vulkan device index to use. The default is the first discrete GPU. |
| `KERYX_POM_V4_KERNEL=dot` | Forces the `v_dot4` kernel. |
| `KERYX_VK_LOG` | Runtime log level on stderr. The default is `info`. |

## Physical device admission

The integration worker requires `--device GPU-<UUID>` or
`--device pci:dddd:bb:ss.f` (PCI requires the driver's pci_bus_info extension).
The runtime resolves that physical identity, rejects non-AMD matches, and pins
both inference and the PoM walk before model load. An index or a model name is
not an identity. The worker acquires an exclusive UUID lease before loading
weights. `nexus_launcher.py` passes `--vulkan-device` through; it must be set
explicitly for AMD. Legacy runtime libraries without the device-selection
export are refused by this worker. Existing installed artifacts are unchanged.

AMD pool AI state is now under `state/pool-ai/<device-UUID>/`, independent of
the CUDA worker. Previous trace records must be retained until their audit
obligations have expired; they are never automatically deleted or migrated.
The new admission and tracing need hardware qualification before deployment.
