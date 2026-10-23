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

## License

Copyright (c) 2026 Nexus Keryx Miner contributors.

Except for third-party material covered by its retained notices, this project
is licensed under the GNU General Public License, version 3 only
(`GPL-3.0-only`); see [LICENSE](LICENSE). You may redistribute and modify it
under those terms. It is provided without warranty, including any implied
warranty of merchantability or fitness for a particular purpose.

Commercial use and resale are permitted without a mandatory payment to the
project owner. Distribution of covered binaries requires making the
corresponding source available as specified by the GPL. Operating a modified
version as a network service alone does not trigger a source-sharing obligation.
The license does not require submitting changes as a pull request here.

Third-party code retains its own licenses and attribution notices; see
[THIRD_PARTY.md](THIRD_PARTY.md). External model weights, drivers and runtime
libraries are not relicensed by this project.
