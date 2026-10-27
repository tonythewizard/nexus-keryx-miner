# ABI9 native adapter and trace formats

The native inference adapter accepts application ABI3/6/8/9. ABI9 requires all
three sequence exports before loading a model; missing functions fail closed.
Legacy positional capture and ABI8 context/token/multiple-replay methods remain
available. Before opening the wrapper, the host sets
`GGML_CUDA_DISABLE_GRAPHS=1` and `LLAMA_ATTN_ROT_DISABLE=1`.

`StockLlamaRuntime::configure_sequence` accepts manifest rows `(layer, kind,
slot, heads, head_dim)`. It checks dimensions, unique layer/slot coverage and
bounded head counts. An empty layout with zero parameters disables capture.
Configuration failure blocks further traced generation until configuration is
restored. Generation and token tracing attach the complete sequence root list.
`replay_sequence` returns the requested byte spans and each leaf's packed chunk
chaining values. It bounds span counts, integer ranges, returned lengths and the
combined host allocation to 64 MiB. These methods retain the runtime mutex and
require the caller's existing exclusive GPU admission.

The bridge's verifier crate is pinned to official miner
`7a9a39355a298250de28ce823a749a30e6f3405f`, with the documented testkit visibility
adaptation. It includes separate residual openings and authenticated feed rows.
Node reference aacad384 matches three changed bound files; four miner files are
ahead of that reference, as recorded in the provenance manifest. The imported
code covers attention phases, recurrent points, sequence/chunk proofs and
extended opening codecs. The five attention manifests are text-encoded, hash-checked build inputs.

The read-only `nexus_ai_h15_manifest` C interface returns the manifest, sequence
layout and minimum application ABI for an explicit network/tier/acceptance DAA.
Both networks select attention manifests (ABI9) at their H15 activation gates. Queries and short buffers return required sizes without writing
payload buffers. Invalid networks, tiers and inactive eras are refused. This
query never authorizes a worker or signs/submits a transaction.

The signed verifier checks the positional/sequence root split against the
response's combined commitment. Positional-only entry points refuse sequence
roots. New Rust entry points verify signatures, response IDs and exact seed
blocks before passing attention phases or recurrent pieces to the official
verifier. Attention `Next` retains the distinction between an intermediate phase
and audit completion; prior accepted paths/roots remain caller-owned context.

Native CUDA tier-0 worker generation, replay-to-proof assembly, durable phase
recovery and actual GPU inference handoff are now connected and tested; see
[the native H15 worker](h15-native-worker.md) for scope and validation boundaries.
The default package stays ABI8; ABI9 remains candidate-only and packaging
refuses it. Existing positional trace records retain their byte format.

The softmax-mass refresh adds one mass leaf per key/value head of each attention
layer: `2 * heads * (attention + recurrent) + heads * attention`. Opening codecs
require its chunk proof in the root phase. The verifier returns authenticated
child contribution weights with `Next`; `draw_attention_child` uses those
weights and the official chain seed. The next phase window includes prompt and
generation replay costs. `trace_sequence.rs` ports the official span planner,
CV/byte proof assembly and attention/recurrent builders behind the resident
weight-index trait. CPU tests bind captured spans to committed leaves and check
that every tier's output-phase plan includes the mass span. Native worker scheduling and persistence are described in the linked worker document.

The latest refresh caps the prompt term of the attention phase window at
131072 positions. Root mass comparison uses the upstream 1/64 tolerance while
internal partial comparison retains 1/256. Contribution weight floors use a
mass-weighted mean; all-zero outputs select uniform weights. Native incomplete
mass-stream errors reject generation/token capture instead of returning roots.
The borrowed pending-trace retention policy preserves an accepted phase awaiting
its draw up to two seed delays past the phase deadline; malformed state cannot silently delete a record.

Latest audit recovery/attention changes: [upstream refresh](upstream-refresh.md). Runtime patch provenance remains independently pinned at c21c8737.
