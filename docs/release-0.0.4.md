# Nexus v0.0.4

This release brings the upstream H15 protocol integration, native ABI9 CUDA
worker path, recovery fixes and complete-session logs into the public source
baseline. The Windows x64 and Linux x86-64 downloads remain unsigned experimental
CLI packages for NVIDIA SM 8.6, with external models, trees, node/IPFS and keys.

## Changes

- Native v5 proof verification, traced sequence/attention/recurrent inference,
  audit phase scheduling, durable trace/opening recovery and node-ledger
  compatibility through official miner functional pin `9586f96d`. The subsequent
  official `e12cc421` commit changes only its package version to 0.5.8.
- Explicit ABI9 inference runtime packaging, retaining its experimental sidecar
  flag and pinned wrapper/patch provenance. ABI8 and ABI9 caches are separated;
  mismatched or missing runtime metadata fails packaging.
- Atomic AI journal recovery preserves prior ambiguous receipts on read/parse
  failure. Request IDs and transaction bindings are checked before writes.
  Journal lookups avoid temporary string allocations.
- CPU policy tests run without Vulkan; Release test assertions remain enabled.
  Queue/RPC/audit/presentation paths avoid redundant work and copies.
- LOG/SUBMIT and ERROR retain the current miner session, rebuild history when
  reopening the UI, and provide bounded Older/Newer/Latest-live pages. SUBMIT
  shows block and AI response outcomes; intermediate and escrow GetBlock noise
  is excluded. Scrolling keeps history fixed while new events are retained.

## Downloads and validation scope

The archives include the CLI worker, independent Rust verifier, matching ABI9
inference library, permitted CUDA runtime libraries, licenses, launcher, exact
build source and checksums. Verify the adjacent `.sha256` before extracting.
The desktop UI is available in source and is not included in these CLI archives.

Linux development validation includes real GPU inference, H15 shadow/recovery
and independent proof checks, plus H14 mainnet acceptance for the functional
candidate. Those local results do not certify GPU execution of every CI-built
archive. Windows GPU execution, live H15 chain acceptance and a real nonempty
node 1.6.6 audit ledger remain unvalidated. RPC acknowledgements are not chain
finality or proof of a paid AI response. Arbitrary multi-GPU scheduling, native
Windows desktop, AMD/macOS packages and automatic installation are not provided.

Existing signed journals must be preserved. No models, keys, wallet/delegation
profiles, logs, node databases or private installation data are distributed.
Automatic updates remain disabled for the public source distribution.
