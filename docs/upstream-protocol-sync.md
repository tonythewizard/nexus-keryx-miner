# Upstream protocol integration

The first integration increment imports the unmodified `inference` crate from
Keryx miner commit `4f46a984adc33fdbf51f390526d41622448c4fc2`. Its source hashes
are recorded in `vendor/keryx-inference-v057/inference/UPSTREAM.json`. Trace
runtime, manifests and bridge verifier have independent provenance; their
current refresh is documented in [upstream-refresh.md](upstream-refresh.md).
The PoM oracle retains its separate node pin `16d1864d835ab4c8359bb23a354351c12088b478`;
the native worker integration is described in [h15-native-worker.md](h15-native-worker.md).

`ai-response-bridge/src/protocol_rules.rs` pins the network rules to miner
`48d31ba7b774301eb1eae8f950169c2bd8392977` and node
`d1ab79b3796d38232fab59bbd126fb5d19fb5776`
(`consensus/core/src/config/params.rs`). The C++ worker queries the Rust matrix:

| Rule | Mainnet | Testnet |
| --- | --- | --- |
| PoM v5, verifiable inference, prefix, audit, chain and seed binding | DAA 140115000 | DAA 3500 |
| Attention/recurrent sequence openings | DAA 140115000 | DAA 3500 |
| Block task | Disabled | Disabled |

These are scheduled protocol gates, not a report of a live network's DAA.
Disabled rules remain disabled at `u64::MAX`. Unknown network IDs are refused.
The legacy worker refuses H15 at startup and when admitting a template. Its
legacy AI service also checks the era before inference, after inference,
before submission and on retries. A crossing refuses further incompatible
work and preserves journal state. The native CUDA tier-0 ABI9 route now uses
capability-based admission with complete trace/audit wiring; unsupported routes
retain these guards.

Private requests now support both official key schedules. Schedule 2 derives
a response key from the individual responder's ECDH secret, preventing other
request recipients from decrypting and reusing its answer. Schedule-1 requests
remain serviceable. The C++ bridge retains the original encrypted request;
Rust reopens it to derive the sealing key without exporting shared/root keys.
The changed C entry points carry `_v2` suffixes so an old bridge cannot silently
satisfy the new calling convention.

A miner checks its response signature, request/responder binding and exact
preservation of the ciphertext returned by the upstream sealing primitive.
Requester-side decryption uses the upstream tagged secret encoding: 32 bytes
for schedule 1, or `0x02` followed by a 32-byte seed for schedule 2. Requester
secrets are never part of a production miner's serving path. CPU tests separately
perform requester decryption of the signed responses and reject wrong keys,
recipients, request IDs, changed ciphertext and undersized buffers.

Targeted checks (use persistent temporary directories and isolated fixture data):

```sh
cargo test --locked --lib --bins --manifest-path ai-response-bridge/Cargo.toml
cargo test --locked --manifest-path ai-response-bridge/Cargo.toml -p keryx-inference
```

Build `nexus-ai-response-service-test` with gRPC enabled and CUDA disabled,
then run `tests/run_ai_response_service_test.py` with that binary, Cargo,
the bridge manifest, Cargo home/target directory and a private temporary test
root. Its public/private response, recovery and H15 crossing tests use mock
inference and loopback RPC/IPFS servers. They do not validate GPU execution,
chain acceptance, the complete ABI9 traced worker or the historical inference fault.


The native CUDA v5 increment and its independent oracle checks are described in
[pom-v5.md](pom-v5.md). Backend implementation and CPU proof validation do not
by itself remove the legacy-worker H15 refusal. The separately validated
native ABI9 route is documented in [h15-native-worker.md](h15-native-worker.md).

The isolated [ABI9 runtime candidate](abi9-runtime.md) now builds and passes
CPU ABI/export checks. The [ABI9 adapter and current trace formats](abi9-adapter.md) are now integrated
with native tier-0 CUDA worker persistence, sequence proofs and GPU validation.
The default CLI runtime remains ABI8; the ABI9 candidate is not packaged.

Latest audit recovery/attention changes and native worker scope: [upstream refresh](upstream-refresh.md). Runtime patch provenance remains independently pinned at c21c8737.
