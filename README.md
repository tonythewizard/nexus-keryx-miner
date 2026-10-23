# Nexus Keryx Miner

Developer source tree for a Keryx Proof-of-Model miner: C++20 worker, Rust
verification/signing bridges, CUDA Driver and experimental Vulkan backends,
Python controls and a Tauri desktop.

- Source version: **0.0.3**. Build locally; no executables, models or runtime libraries are distributed.
- Current build baseline: **Linux x86-64**. Windows and macOS require porting; they are not validated targets.
- GPU execution requires an explicit local device profile and compatible inference runtime.
- H15 verifiable inference and scheduling more than two GPU lanes are incomplete.

Start with **[devkit.md](devkit.md)** for architecture, dependencies, builds,
testing, runtime configuration and agent instructions. Run the publication
gates before committing; see [SECURITY.md](SECURITY.md).

The repository is private during preparation. The project license has not yet
been selected. Existing third-party notices remain in place; see
[THIRD_PARTY.md](THIRD_PARTY.md).
