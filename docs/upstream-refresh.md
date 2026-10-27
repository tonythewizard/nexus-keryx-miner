# Official miner refreshes

## Current refresh: node-released replay duties

Official miner client-policy pin is
[`9586f96dc957b29031514256edf5d1b599d810b2`](https://github.com/Keryx-Labs/keryx-miner/commit/9586f96dc957b29031514256edf5d1b599d810b2),
one commit after `41d33919`. On nodes supporting `GetAwaitedOpenings`, an
expired replay starts only when the node still lists that response. A successful
poll marks membership only for the exact queried IDs. Failed or malformed
responses convey no negative membership; requests before their deadline retain
normal eligibility. Older nodes retain local estimates and backlog behavior.

Explicitly omitted duties retire after the latest phase deadline, preserving
an accepted attention phase while its child draw is pending. Retirement retains
signed response/opening bytes and capture files. Native replay/submission is
synchronous under the existing owner; there is no detached replay task to cancel.
Canonical acceptance and RPC acknowledgement remain distinct. Poll observations
are session state, cleared on restart, relevant reorgs and legacy-node fallback;
durable metadata stays at schema4. Protocol, verifier, inference runtime and
PoM rules are unchanged. Source builds do not alter installed profiles.

## Previous refresh: audit restoration and seed diagnostics

Official miner diagnostic pin is
[`41d33919ccea4001e44da30c8a19a8c29050ea13`](https://github.com/Keryx-Labs/keryx-miner/commit/41d33919ccea4001e44da30c8a19a8c29050ea13),
two commits after `e237055b`. Native logs distinguish `RESTORED_OPENING` from a
new draw, and `NODE_SEEDED` from `NODE_SEED_MISMATCH`. Restored records report
whether a prior submission intent exists; this does not assert RPC or canonical
acceptance. Node seed events are emitted after the updated metadata is durable.

These are diagnostic-only changes: no audit transition, protocol, verifier,
runtime, scheduler, journal layout or PoM rule changed. Existing functional
provenance pins remain independent; the exact diagnostic source hash is recorded
in `TRACE_UPSTREAM.json`. Installed profiles remain unchanged by source updates.


## Previous refresh: node-ledger audit authority and trace bounds

Official miner pin is
[`e237055b2ebd8dc687328793b209ecfd7c0d2c0b`](https://github.com/Keryx-Labs/keryx-miner/commit/e237055b2ebd8dc687328793b209ecfd7c0d2c0b),
three commits after `331cb63a`. All22 verifier Rust sources match node
`b13faafee0a56ca9d77545a18469abd599a5db2d` exactly. The source delta includes
the node's read-only `GetAwaitedOpenings` protocol fields and method IDs.

Traces cannot exceed131072 positions, and generated positions cannot exceed
the request token budget. Parsed context overrides clamp to1..131072. Native
capture/replay retains its stricter32768-position admission and memory limits;
the protocol cap does not authorize allocating an unsupported native context.

The native audit client recognizes node1.6.6 or later and polls awaited duties
at ten-second intervals, with at most1024 response IDs. It maps the node's
derived seed to a known canonical seed block and persists updated deadlines,
accepted DAA, expected piece mask and attention path. Unknown seed blocks wait
for chain coverage. Old signed proofs stay cached but cannot be submitted under
a new seed/path. Node-still-awaited pieces retry their original signed bytes;
RPC acknowledgement never counts as canonical acceptance. Older nodes retain
local estimates and clear stale node authority without changing signed responses.

Local deadline chaining is an estimate without the former queue-cap exemption;
arithmetic overflow blocks progress rather than dropping the duty. Scheduling
also reserves the node's270000-DAA span for the next request, including three
attention phases and capped network backlog, so a ready audit may preempt it.
Canonical restore gaps continue to hold replay/effects. Partial node ledger or
IO failures preserve existing files; the client uses bounded read-only RPC calls.

Native metadata schema4 appends node authority and reads exact schema3/schema2
layouts. Unknown/corrupt records and unsupported signed formats remain preserved.
An older worker may refuse schema4 records; do not rewrite or delete audit state
to force a downgrade. Runtime wrapper, PoM oracle and model/activation pins are
unchanged. CPU/mock, GPU execution and live chain acceptance are distinct;
new artifact results must be recorded separately from installed profiles.


## Previous refresh: bounded output outliers

Verifier pin is official miner
[`331cb63acbf9291628b01fa98c179e1aa8e07942`](https://github.com/Keryx-Labs/keryx-miner/commit/331cb63acbf9291628b01fa98c179e1aa8e07942),
one commit after `bc69a4e2`. All 22 vendored verifier Rust source files match
node `abe75b2ab06ac3827f6ccc85a97930e6e8abc93f` Git blobs exactly.

Attention output checks allow at most four values per 32-value quantization
block beyond the ordinary two-step threshold, with a hard maximum deviation
of three quantization steps. The same rule is used for block roots and direct
MLA outputs. Recurrent output projection input checks allow at most four values
per block beyond the ordinary one-step threshold, capped at two steps. Counts
are per block, not a combined allowance across an output. Nonfinite values,
noncanonical rows, invalid ranges and mismatches against zero-scale blocks
remain rejected. Root mass tolerance and its previous cap remain unchanged.

The native verification bridge reaches these imported checks directly; no
runtime, trace format, signed payload, producer, activation, PoM implementation
or independent oracle change is required. Trace producer provenance remains
`bc69a4e2`; verifier provenance is independently pinned in its `UPSTREAM.json`.
Additional bridge regressions cover exact caps, excessive counts, per-block
allowances, offsets, nonfinite/malformed rows and zero-scale behavior without
patching the upstream verifier. Existing installed profiles are preserved by
this source integration; new worker acceptance is recorded separately.


## Previous refresh: routed residual and capped attention mass tolerance

Source/verifier pin is official miner
[`bc69a4e23faf99d34dcaf5326644c5a5641ca6af`](https://github.com/Keryx-Labs/keryx-miner/commit/bc69a4e23faf99d34dcaf5326644c5a5641ca6af),
five commits after `7a9a3935`. All 22 vendored verifier Rust source files now
match node `77c9037fe028f52b495daeb9c32522e6e54b4e69` exactly, including the
upstream testkit feature visibility. No local verifier source patch remains.

A routed output residual draws expert sums only when the layer has no post-FFN
normalization. The native producer follows the same rule. Expert down rows in a
trimmed last layer accept the canonical layer role, including heads. Attention
root log-sum-exp tolerance is `1/64 + min(abs(max_score), 176)/4096`, for both
block roots and direct MLA verification. The cap limits widening at extreme
scores; upstream tests cover honest kernel rounding and inconsistent mass.

The remaining deltas tidy comments, including the existing half-uniform child
draw; they change no PoM arithmetic, activation, model manifests or deadlines.
Runtime wrapper, patch chain and CUDA-MMQ build policy remain independently
pinned as before. The independent PoM oracle remains unchanged. Existing
installed profiles are preserved by this source integration; source/node parity
and local tests do not establish H15 mainnet acceptance.


## Previous refresh: separate residual proof and audit backlog

Current source/verifier pin is official miner
[`7a9a39355a298250de28ce823a749a30e6f3405f`](https://github.com/Keryx-Labs/keryx-miner/commit/7a9a39355a298250de28ce823a749a30e6f3405f),
nine commits after `48d31ba7`. Native response bundles now carry trace, attention,
optional recurrent and a separate residual piece. The residual additionally
opens one row of the product it adds, and an expert row on routed layers. Each
piece independently binds the response, signer, canonical seed and combined
roots. Historical inline helpers remain separate compatibility entry points;
the native response path requires the new split format.

The verifier follows the miner exactly, with the existing testkit visibility
adaptation. Node reference `aacad384e539e02d501630400fb8a5d463d9d6d7` matches the
changed q8/recurrent/verification bounds. Four miner files (`block_task`, `chain`,
`chainkit`, `opening`) are ahead of that node reference; the manifest records
the difference. Do not infer live H15 node compatibility from a source import.
The independent PoM oracle still uses unmodified node16d1864; no PoM code,
model/manifest bytes or activation gates changed in these commits.

Native audit deadlines chain per signer in canonical seed/response order, using
the official serving-work rates and 36000 DAA cap. Accepted undrawn work shares
the replay budget; future deadlines take priority over overdue work when an AI
request waits. Known restore gaps hold draws. Openings retain immutable bytes
until canonical acceptance, retry at three seconds for the first ten attempts
then sixty seconds through the backlog window. Replay failures back off from
sixty seconds to ten minutes and keep their duty after the second failure.
Unfinished audits retain backlog grace; completed ones retain only the existing
reorg grace. Both accepted and unaccepted undrawn records use 72000 DAA.

Trace files are written once; the native metadata is updated separately. Its
schema 3 reads schema 2 response records, validates bindings and preserves
unsupported signed inline pieces for explicit reconciliation. A traced response
is still fsynced before any submission; IO failures remain fail-closed. A
response accepted ahead of its request can resend the same signed bytes after
125 seconds, then at sixty-second spacing, up to ten sends. Native persistence
retains those bytes across restart as well as within one run.

Runtime patch/wrapper provenance remains c21c8737. The native runtime build now
forces `GGML_CUDA_FORCE_MMQ=ON`, following the official miner build policy, so
trace hooks use quantized kernels. Its sidecar records the build-policy pin and
MMQ setting independently of the patch pin. Reconfiguration continues to verify
the original patch chain on every build; unrecognized sources are never reset
or cleaned as a side effect. Existing installed profiles are not modified by
this source refresh. Validation and platform limits are described in
[h15-native-worker.md](h15-native-worker.md).

## Previous refresh: audit recovery and mainnet attention

Current source/adapter pin is official miner
[`48d31ba7b774301eb1eae8f950169c2bd8392977`](https://github.com/Keryx-Labs/keryx-miner/commit/48d31ba7b774301eb1eae8f950169c2bd8392977),
12 commits after `c21c8737`. Changed verifier files match official node
`d1ab79b3796d38232fab59bbd126fb5d19fb5776`. The runtime patch/wrapper remains
pinned at `c21c8737`; the independent PoM oracle remains at node `16d1864`.

- Attention activates on mainnet and testnet at their respective H15 gates;
  the native manifest query therefore selects ABI9 on both networks.
- Link fragments use the original input-scale-weighted block draw. Unlinked
  inputs enforce peak/median bounds. Attention partial checks handle f16 floors
  and cancellation, root LSE tolerance is 1/64, child draws mix half uniform.
- Every opening builder reserves the 33-byte seeded envelope, including final
  attention/recurrent serialization and chain roots added after construction.
- Audit helpers restore from the last accepted phase or submission anchor,
  continue chain pages and retry transient failures with 5–60 second backoff.
  Accepted undrawn traces have a 72000 DAA horizon; phases awaiting draw retain
  up to two seed delays after their deadline. Legacy positional admission keeps
  its separate fail-closed recovery gate.
- Generation/replay estimates use original per-model moving averages; urgency
  compares queued request plus replay work against the audit deadline margin.
  Parked work preserves handles, transaction bytes and answered IDs; unsent
  opening pieces keep zero attempts, request escrow IDs are bounded, late phase
  acceptance is tracked across reorg and cached phases bind seed plus path.
- The new audit store carries v5 anchors/durations, sequence and attention data
  with v2/v3/v4 readers and an ordered bounded writer. It uses a separate
  `.audit-traces` namespace, atomic synced writes and explicit flush errors;
  corrupt/unknown records are retained and block recovery. Existing positional
  journals are not migrated. This is a safety adaptation of upstream's writer.

The [native CUDA H15 worker](h15-native-worker.md) now connects generation,
replay, signed response/opening persistence, selected-chain recovery and urgent
phase scheduling. Authentic traced GPU proofs, real inference handoff and the
actual worker's H15 shadow path are validated locally. Its v5 proof stream now
selects the official v5 verifier using header DAA. Unsupported routes retain
legacy refusal; the 0.0.4 native CI explicitly selects matching ABI9 packaging. No installed profile or
main build is replaced, and live H15 chain acceptance remains separate.

## Earlier refresh: phase windows and mass coverage

Previous source/adapter pin: official miner
[`c21c873726049195a3a40af6f19a013fc44ebd82`](https://github.com/Keryx-Labs/keryx-miner/commit/c21c873726049195a3a40af6f19a013fc44ebd82),
four commits after `43b0d439`. The verifier reference is official node
`7ff7c704c4b4d7440163a05bf353411e211499e6`. The unchanged PoM oracle remains
independently pinned to node `16d1864`; no PoM code or activation parameter
changed in this delta.

| Commit | Nexus integration |
| --- | --- |
| `25551635` | Original verifier changes: root LSE tolerance 1/32, internal partial tolerance remains 1/256; child weight floor uses the mass-weighted mean and zero outputs get uniform weights |
| `9b0a0e3f` | Phase window prompt contribution capped at 131072 positions; Rust and native C query covered by the `u32::MAX` vector (16107 DAA) |
| `b8cf020b` | Pending-trace retention keeps an accepted attention phase while its child is awaiting draw, even past its prior deadline; unaccepted, completed and reorged phases expire normally |
| `c21c8737` | Original revised runtime patch 0005 tracks per-layer LSE coverage and refuses incomplete mass streams with the native error propagated through the adapter |

The retention API takes borrowed verified phase state. It does not change the
legacy journal schema, submit anything or enable H15. Indeterminate expired
state blocks eviction and preserves the original record. Native phase/worker
persistence wiring remains pending.

The revision still reports ABI9 and still contains five patches. Its exact
patch hash and final source-stage hashes differ from the earlier ABI9 build.
The source helper refuses the previous completed patch-0005 revision without
mutating it: rebuild from a pristine or verified four-patch prefix. Preserve
old runtime artifacts for rollback. The candidate's complete 3171-file source
was checked against the original archive plus original patches; only
`ggml/src/ggml-cuda/q8seq.cu` differs from the previous completed runtime source.

Validation: 97 bridge Rust unit tests plus one signing integration, 135 official
verifier tests (one vector generator ignored), 8 targeted CTest and 28
distribution tests passed. Runtime and worker compile/link, runtime ABI/export/
null-handle checks and ELF closure passed. No new GPU/model execution, native
Windows test, live chain acceptance or deployment is implied by this refresh.

## Previous refresh: committed attention mass

Nexus ports official miner commit
[`43b0d4391dac20b21681cbbd2e9b443227315211`](https://github.com/Keryx-Labs/keryx-miner/commit/43b0d4391dac20b21681cbbd2e9b443227315211),
one commit after the previous `4f46a984` pin. This refresh addresses the complete
12-file upstream change at the source/adapter boundary; it does not enable the
unfinished H15 worker or modify an installed miner.

| Official change | Nexus integration |
| --- | --- |
| Runtime patch 0005 | Original patch, five-stage application chain plus pristine state, pinned hashes, rebuilt isolated ABI9 candidate |
| Engine sequence count | Native count includes attention log-mass leaves; old ABI9 root coverage is rejected |
| Trace deadlines | Exact replay-scaled phase window in Rust and additive C query |
| Client child draw | Weighted draw helper uses official verifier weights and chain seed; active worker scheduling is still pending |
| Sequence opening builder | Official span planning, mass capture/proof assembly, child weight extraction, attention/recurrent construction behind the resident index trait |
| Verifier and synthetic fixtures | Current `attn_blocks`, `chain`, `chainkit`, `opening`, `seq` and `support`, with official tests |
| Inference test child selection | Covered by official weighted draw and phase/proof tests; real GPU inference is pending |

The log-softmax mass binds the attention output to the partials that a phase
commits. Child selection mixes uniform sampling with contribution weights; an
accepted intermediate phase returns its root, child count and weights. A
subsequent phase's window includes response replay cost. The ABI remains 9,
so keep the exact patch manifest and library checksum with the candidate.

The associated official-node reference is
`16d1864d835ab4c8359bb23a354351c12088b478`. The independent oracle uses a separate
pinned checkout; previous checkouts are preserved. Node implementation changes
are reference/dependency work only. Network activation parameters and public
manifest bytes are unchanged by this refresh. Private-response crypto and PoM
source retain their independent provenance pins because they did not change.

H15 admission and candidate packaging remain blocked. Sequence persistence,
phase/reorg recovery and native worker wiring must be completed before GPU
capture parity, real inference handoff and live acceptance can be established.
