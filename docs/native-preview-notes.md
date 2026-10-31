Experimental native CLI for Windows x64 and Linux x86-64 (Ubuntu 24.04 baseline),
NVIDIA RTX 30/40/50 (SM 8.6, 8.9 and 12.0). WSL is not required for Windows.

Packages include the worker, independent verifier, explicitly selected ABI9
inference runtime, CUDA redistributables, licenses, exact source and checksums.
ABI9 retains experimental candidate metadata; mismatched ABI caches fail closed.
Models, trees, keys, node/IPFS data and existing journals are external inputs.

Native H15 tier-0 CUDA paths have local GPU/mock validation; live H15 chain
acceptance and real nonempty node 1.6.6 audit-ledger validation remain separate.
Windows GPU execution and GPU execution of these exact CI archives are not
validated by CPU compilation/tests. The desktop and AMD/Vulkan backends are
not included in these CLI packages. RTX 40/50 execution and low-VRAM cards
remain unvalidated; architecture inclusion does not guarantee sufficient
memory. See the 0.0.4 release notes
for full scope.

Unsigned experimental binaries. Original source GPL-3.0-only; third-party
notices and runtime licenses included. Automatic updates remain disabled.

This refreshed v0.0.4 includes asynchronous H15 submission, a persistent RPC
stream, 1–8 proof workers and bounded post-submit Rust audits. An audit failure
blocks further submissions but cannot recall an already sent candidate. The
release tag has been updated with the owner's authorization; BUILD.json and
the bundled source identify the exact refreshed build commit. See
https://github.com/tonythewizard/nexus-keryx-miner/blob/main/docs/h15-post-submit.md
for ordering, cancellation and validation details.
