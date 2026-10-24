Native CLI preview for Windows x64 and Linux x86-64 (Ubuntu 24.04 baseline).
NVIDIA SM 8.6 only. Windows binaries use native APIs; WSL is not required.

Packages include the worker, independent Rust verifier, matching inference
runtime, CUDA redistributables, licenses, explicit-profile launcher, exact
source commit and SHA-256 checksums. Verify the accompanying `.sha256` file.
Models, trees, wallets, keys, node/IPFS data and existing escrow journals are
external inputs. See `windows.md` inside each archive for setup and limitations.

Validation covers native compilation and CPU/API tests. Real Windows GPU proof
parity, inference preemption and accepted on-chain submissions are not validated.
The desktop and AMD/Vulkan backends are not part of these CLI packages.
**H15 support remains incomplete; this preview does not replace a validated
post-H15 miner such as SUPR 0.15.0.**

These are unsigned experimental binaries. Original project source is GPL-3.0-only;
third-party notices and runtime licenses are included. Automatic updates remain off.
