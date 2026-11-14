# Native pool compatibility

Nexus keeps its own worker, GPU scheduler, inference engine and recovery journals.
Pool interoperability does not require advertising another miner's name. The
implementation normally sends `nexus-keryx-miner/0.0.5-dev` and implements the public
wire contract independently; no Supr implementation is incorporated.

Suprnova's H16 login gate also parses a legacy protocol-version prefix. An
H16-capable NVIDIA worker opts into the explicit compatibility token
`keryx-miner/0.6.1-compatible (Nexus/0.0.5-dev; native-H16)` on the three published
Suprnova hosts only. This names protocol compatibility, not an official-miner or
Supr executable version. `mining.hello` continues to identify Nexus. Other pools,
unqualified clients and explicit custom agents retain their native identity.
A live subscribe/authorize probe accepted this token. The updated native NVIDIA
worker then received real Suprnova share acknowledgements with Rust verification
before submission and active metadata acknowledgements. This does not certify
public-pool AI completion, payments, escrow eligibility or another CI archive.

An authorization refusal retains `AUTH_REJECTED` and the pool's bounded error
message. Subsequent job/difficulty notifications cannot grant authorization or
erase the refusal. No capabilities or telemetry are sent before authorization.

The protocol comparison uses Supr v0.16.3, commit
`cd49fbdd745fbc53d6e35475ded3a955f740945a`:

| Requirement | Native implementation | Qualification boundary |
| --- | --- | --- |
| Exact prompt bytes, including NUL | Sized generation and traced generation ABI; encrypted-request regression | Legacy runtime refuses NUL instead of changing the prompt. GPU execution requires a matching runtime. |
| Miner metadata | Schema-v1 `mining.hello`, after subscribe and authorize acknowledgements | An accepted metadata frame does not establish share acceptance. |
| Periodic telemetry | `mining.telemetry`, default 120 seconds, minimum 30 seconds | Measures completed nonce walks; omits stale samples. Metadata errors never become share receipts. |
| Capabilities | `mining.declare_capabilities`, refreshed after authorization | Published only after the native pool dispatcher passes a traced generation and two-seed replay self-test. |
| Pool AI requests | `mining.ai_request`, seven positional fields and optional eighth trace object | Native NVIDIA tier-0 traced answers, durable capture before send, bounded worker-side dispatch; real GPU/mock pool flow tested. |
| Pool audit opening | `mining.ai_opening` and `mining.ai_opening_result` | Native unsigned opening bundles and attention continuations; request/response binding and Rust verifier checks; retained trace reused after worker restart. |

`mining.ai_request` carries task ID, transaction ID, request hash, model ID,
base64 prompt, maximum tokens and reward. The trace extension is parameter 7
(zero based), not parameter 4. Responses use `mining.ai_response`; traced
responses also supply the commitment and encoded answer body. Opening requests
bind the request hash, response transaction, seed, attention path and previous
partials. The pinned codec does not establish `mining.ai_draw` as a universal
replacement for these methods.

Telemetry has a bounded frame size and low-priority nonblocking writes. A partial
frame forces reconnect so it cannot interleave with a share. Optional-method
rejection disables telemetry for that session; reconnect resets its acknowledgement
state. No hostname, device UUID, private key or delegation certificate is sent.

The UI connection test sends only subscribe and hello. It verifies TLS chain and
hostname when configured for TLS. It never authorizes a wallet, advertises models,
submits a share or establishes escrow eligibility.

The operator announcement supplied on 10 November 2026 describes KeryxPool's
escrow distribution trial as requiring Supr 0.16.3, telemetry, NVIDIA and manual
approval, with a 70/30 split and a 22-hour release delay. Nexus cannot infer an
exception or approval from successful protocol probes. These conditions describe
the escrow trial; they do not by themselves establish ordinary mining rejection.

Suprnova's [setup guide](https://krx.suprnova.cc/StartMining.html) recommends Supr
0.16.3 and also describes the official miner. Its recommendation alone does not
establish an exclusive miner-brand requirement. The
[release](https://github.com/ocminer/keryx-miner-supr/releases/tag/v0.16.3) documents
per-card H16 tuning; its development benchmarks are not Nexus measurements.

The native traced adapter retains its existing 60 KiB bound per prompt segment.
Preserving NUL bytes does not establish support for every larger pool request.
Do not set `trace_ready` until complete pool AI transport, bounds, persistence,
reconnect recovery and exact GPU proof replay have been qualified.

The native NVIDIA pool route keeps its own captures for 48 hours. Old records are
archived without deleting history; active storage and input sizes remain bounded.
A duplicate request reuses its immutable answer. An interrupted file publication
is recovered before sending. Records are isolated by pool operator and worker. Published aliases/ports of
the same operator share custody; custom endpoints remain separate. Network
replies stay bound to the transport that received the request, so switching to
a different operator cannot expose the previous pool's answer. AI
result acknowledgements are separate from mining share receipts.

The pool opens the request with its escrow identity and seals/signs the returned
answer and openings. Nexus never needs the pool's key and sends no on-chain
transaction on this path. The adapter serves the H15 traced contract; it does not
implement the optional Supr cross-rig `KRXREC01` custody/import extension or
legacy untraced pool chat. Those are not advertised as supported features.
A `mining.ai_request` the route does not serve, such as untraced chat, is
declined with error 46 (busy), so the pool re-dispatches it instead of waiting
for a timeout. Openings are never declined.

Local qualification covers an actual worker, model and GPU against an isolated
pool simulator, including worker restart, nodeless operation, same-operator aliases and subsequent opening. It is not an
acknowledgement, payment, or escrow approval from either public pool. Broader
hardware tiers and pool-side implementation differences remain separate checks.
