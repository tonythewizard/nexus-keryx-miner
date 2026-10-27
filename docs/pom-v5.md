# PoM v5 backend and oracle

The C++ v5 reference and native CUDA SM 8.6 backend implement the official
state-dependent walk over weight tiles and a deterministic context pad. The
Rust oracle uses unmodified node source at
`16d1864d835ab4c8359bb23a354351c12088b478`, fetched into an ignored versioned
directory. Existing older dependency checkouts are left intact. The miner CPU
reference has its separate `vendor/keryx-trace-abi6/POM_V5_UPSTREAM.json` pin.

| Tier | Pad tiles | Pad MiB |
| --- | ---: | ---: |
| 0 | 638976 | 624 |
| 1 | 3213312 | 3138 |
| 2 | 4804608 | 4692 |
| 3 | 6471680 | 6320 |
| 4 | 2450432 | 2393 |

`official_v5_pad` binds the seed to tier, model ID and model root. The CPU
implementation and CUDA kernel both use 1920 ARX rounds per 32-byte entry.
The next tile address includes the pre-transition state fold; the v4 offset-only
witness reconstruction explicitly refuses a prepared v5 pad.

`CudaResidentSearch::prepare_v5` requires exclusive, drained PoM admission.
It checks available VRAM, including descriptors and a 64 MiB reserve, fills the
pad directly on the GPU in slices of at most 2 MiB, and checks cancellation
between synchronized slices. It retains no host copy of the pad. A cancelled
or failed preparation leaves no pad marked ready. Weight descriptors stop at
the last complete weight tile; incomplete tail chunks stay in the model root
and are excluded from the walk before appending the pad.

Callers must close PoM admission when AI becomes pending, drain the running
batch, call `release_v5`, run inference, and rebuild with `prepare_v5` before
resuming. This releases the pad without resetting the shared model context.
The existing preemption coordinator enforces this ownership order. The tests
exercise success and exception restoration using GPU scratch work as a mock
for inference; those checks do not attest an actual Qwen response.

`run` refuses a missing pad in the mainnet v5 era or a v5 pad in an earlier
era. `diagnose_v5` returns one bounded full state/offset trace from the same
search kernel. The test harness compares it with the independent CPU walk,
then submits an offline proof artifact to the official Rust verifier. The
oracle validates the canonical proof, all states/offsets, seed/nonce binding,
weight paths, empty pad witnesses, final state, PoW pin and target. Its
`verify-v5-real` mode additionally enforces official tier/model/root/chunk/pad
anchors; `verify-v5-fixture` permits explicitly synthetic geometry.

Build `nexus-pom-v5-test` with CUDA disabled for CPU checks, or with CUDA and
gRPC enabled for the native GPU checks. No test starts a node or miner.

```sh
python3 tests/run_pom_v5_test.py BINARY ORACLE PRIVATE_TEST_ROOT
# Read-only use of an existing authentic model and tree:
python3 tests/run_pom_v5_test.py BINARY ORACLE PRIVATE_TEST_ROOT \
  --cpu-real MODEL TREE JSON_SIDECAR
```

GPU modes are explicit (`--gpu` for synthetic weights, or `--gpu-real MODEL
TREE JSON_SIDECAR`). They require an idle authorized device, exact matching
`NEXUS_PRIMARY_GPU_UUID`/`CUDA_VISIBLE_DEVICES`, an existing private
`NEXUS_DATA_DIR`, its exclusive device lease, and no other miner or CUDA
compute workload. The verified system KDE compositor may keep its display
context; its memory remains part of the VRAM admission budget. Generated
proofs and logs stay in the private test directory.

The [native H15 worker](h15-native-worker.md) now connects v5 admission and
proof verification with ABI9 traced AI service and durable audit recovery.
Native Linux tier-0 GPU inference handoff and actual worker shadow execution
are validated. Vulkan/JIT/BDA v5, testnet worker admission, Windows GPU execution
and live H15 chain acceptance remain separate work. Successful offline proof
verification is not chain acceptance.

Validation of this increment on Linux/SM 8.6 covered twelve complete synthetic
CUDA traces and three complete tier-0 model traces with the official 624 MiB
pad. Every trace's 257 states and 256 offsets matched the CPU reference and
passed the pinned node verifier. The harness also checked selective target
screening, fifteen proof/context mutation types, two GPU-memory handoff cycles
per case, fill cancellation, oversized-pad refusal and v4 regression after
release. These results apply to the tested backend/harness; the production
worker was compiled and linked, without a new live worker session.

The real-model CPU/GPU harness also checks the worker's `verify-real-stream`
transport: honest v5 frames pass and eleven context/trace mutations per model
artifact fail. Its v4 branch remains selected below the mainnet H15 gate.
