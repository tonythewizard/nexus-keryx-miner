# Native CUDA H15 worker

The native CUDA tier-0 worker connects the miner `7a9a3935` trace builders and
verifier to the ABI9 runtime pinned at `c21c8737`. H15 mainnet admission requires
that runtime, native CUDA inference/mining and the supported single-device
route. Other backends retain their legacy-era refusal. The 0.0.4 native CI explicitly builds and packages ABI9, preserving its
experimental runtime metadata. The standalone runtime configure default remains ABI8.

## Generation and proofs

Private H15 requests use the official Qwen system prompt and request-specific
prefix, positional leaves, attention/recurrent sequence roots and attention
mass leaves. The signed response binds the combined roots and token root;
its encrypted plaintext carries the official token encoding. The existing
request assignment, responder signature, deadline, ciphertext and transaction
identity checks remain in force.

The native replay callback borrows the verified GGUF/Merkle index and runs only
under the worker's exclusive inference admission. Chain, attention, recurrent and separate residual
builders verify their output against the pinned official verifier. The residual piece also authenticates a product feed row and, for
routed layers, an expert row. Attention
child selection uses the canonical draw block and authenticated contribution
weights; an intermediate phase does not complete an audit.

The worker reduces the ABI9 task context before allocating the 624 MiB tier-0
PoM v5 pad. AI admission drains PoM and releases its resident engine/pad before
restoring the full inference context. Mining rebuilds the pad after inference.
A model-only pad fill tolerates template replacement; shutdown, AI work and
device deselection still cancel it between bounded fill slices.

The productive v5 path compares the GPU's 257 states and 256 offsets with an
independent CPU walk and builds canonical empty pad witnesses with 1–8 proof
workers. The refreshed H15 fast path submits after CPU/GPU, model, Merkle,
header and target checks, then audits through the independent Rust stream;
see [post-submit ordering and failure semantics](h15-post-submit.md). The Rust
stream selects v4/v5 by the header DAA and enforces official v5 geometry using
the unmodified node core pinned at `16d1864`. Target failure remains a valid
nonwinner outcome, separate from successful proof verification.

## Persistence and recovery

Captures use a separate `.audit-traces` namespace with an ownership marker;
existing positional journals are not imported or reset. The trace writer is
bounded and ordered. Native schema-3 metadata reads schema-2 response records and has its own version/checksum and binds the
model, signer, response, request, signed response bytes, phases and attempts.
Capture and metadata are synced before the response can reach the node. A
partial pre-effect write is preserved; corrupt, unknown or mismatched state
blocks recovery instead of deleting evidence.

Startup authenticates stored responses, reconstructs trace roots and verifies
stored active opening signatures/proofs. A saved response binds the response
journal to its existing transaction ID and prevents fresh generation after a
crash. Retries reuse the exact signed response/opening bytes, with durable
attempt intent. Ordinary responses keep their original deadline and 100-attempt
cap; openings use three-second spacing for ten attempts, then sixty seconds
through the 36000 DAA backlog extension. A response accepted before its request
can resend after 125 seconds and at sixty-second spacing for up to ten sends. Mempool
presence refunds a reserved attempt; an unknown outcome refunds later unsent
pieces. Permanent response rejection is retained without a background retry.
Only selected-chain acceptance advances the audit, independently of RPC ACKs.

Read-only chain restore starts at the last accepted phase, response acceptance
or submission anchor. It processes at most eight added headers per poll with
a two-second RPC budget and bounded pages. Transient failures retain duties
and use 5–60 second backoff. Mining pauses while restore is incomplete. Actual
canonical blocks determine delayed seeds, including gaps in DAA scores.
Reorg reconciliation invalidates affected phases, keeps a bounded seed/path
cache and can reuse identical signed bodies on reinclusion. Retired duties
release host memory while their original files remain on disk.

Audit deadlines chain per signer in seed/response order. Scheduling compares
measured generation/replay work, including accepted undrawn work, with the next
request and the cumulative earliest-deadline margin. Failed replays retain their
duty with sixty-second to ten-minute backoff. Private prompt estimates use bounded opening
and decompression. Known startup requests and response-admission retry waits
also service urgent audit duties.

## Validation scope

Linux native CUDA checks use an explicitly leased idle device, an authentic
tier-0 model/tree and isolated synthetic identities. They cover traced short
and long prompts, root/child attention and recurrent proofs, durable restart,
seed reorg and cached reinclusion. A separate residual piece authenticates a product/expert feed row. An actual private request also traverses
real generation, sealing/signing, native durable commit and recovery after a
simulated lost acknowledgement; its submission effect is entirely in memory.
Real inference is exercised between PoM v5
pad release/rebuild cycles, with full CPU/GPU parity and independent official
Rust verification of the resulting proofs.

A separate manual shadow harness runs the actual worker against a loopback RPC
fixture at H15 DAA. Submissions are disabled and the fixture rejects/records
mutation RPCs. This proves worker execution and local verification, not node
consensus acceptance of a block. CPU tests additionally exercise native ABI
bounds, fsync-before-send, duplicate/recovery behavior and v5 stream mutations.
The `nexus-h15-native-test` and `nexus-h15-worker-rpc` targets are manual,
excluded from the default build and automatic GPU testing.

These results do not establish live H15 mainnet acceptance, testnet worker
admission, Vulkan/JIT/BDA v5, Windows GPU execution or the cause of a historical
AI request failure. Installed profiles, shortcuts and the selected main build
are deployment state and are not changed by this source increment.

The runtime build forces the quantized CUDA kernels (`GGML_CUDA_FORCE_MMQ=ON`)
and records that policy separately from its unchanged patch provenance. The
new verifier follows the miner ahead of four files in the node reference;
see [upstream-refresh.md](upstream-refresh.md). This does not certify live H15
acceptance against the current node or other GPU architectures.


The subsequent `bc69a4e2` refresh aligns routed residual draws and trimmed expert
rows, and uses the capped score-dependent attention mass tolerance. Its verifier
Rust sources match node `77c9037f`; see [refresh details](upstream-refresh.md).
Runtime ABI9/MMQ and independent PoM oracle pins remain unchanged. Earlier GPU
and live results refer to their exact recorded artifacts, not automatically to
a rebuilt worker.


The `331cb63a` verifier refresh imports the bounded per-block attention/recurrent
output outlier checks unchanged from the official miner/node. Native bridge
regressions cover the hard limits and invalid inputs; see
[refresh details](upstream-refresh.md). Runtime, producer and oracle provenance
remain unchanged. Validation of any rebuilt artifact must be recorded separately
from the earlier mainnet result.


Refresh `e237055b` adds bounded node1.6.6 awaited-opening polling and durable
schema4 authority, with schema3/schema2 reads and legacy-node estimates.
Trace/context protocol caps and generated-token budgets are enforced while
native capture remains limited to32768 positions. See
[refresh details](upstream-refresh.md). A schema4 record is preserved if an
older rollback worker cannot read it; forced downgrade/reset is unsupported.
