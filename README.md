# Nexus Keryx Miner

Nexus is a source-only, experimental Keryx Proof-of-Model miner. Its components
include a C++20 worker, Rust proof/signing bridges, a native CUDA backend, an
experimental Vulkan backend, Python service controls and a Tauri desktop.

## Project status

- **Version:** 0.0.3 (source baseline, not a certified release binary).
- **Platform:** Linux x86-64 is the current build baseline. Windows and macOS
  have not been validated.
- **CUDA:** existing kernels target NVIDIA SM 8.6; device admission and the
  external inference runtime must be configured locally.
- **Vulkan:** experimental and hardware/capability-gated. A successful build
  or available UI option does not establish support for a GPU.
- **Incomplete work:** H15 verifiable inference and scheduling for more than
  two GPU lanes.

No executables, GGUF models, runtime libraries or installation credentials are
distributed in this repository. Building the source does not configure a
wallet, connect a node or start mining.

## Getting started

Read [devkit.md](devkit.md) for dependencies, the offline CPU build, native
GPU build options and test boundaries. The CPU-only checks are a starting
point; they do not validate GPU operation or mainnet acceptance.

For development, keep all temporary files and builds in persistent, ignored
directories under the checkout. Production profiles, wallets, model data and
logs must remain outside Git.

## Security and contributions

Please read [SECURITY.md](SECURITY.md) before reporting a vulnerability or
publishing changes. Use private disclosure for security issues; ordinary
bug reports and proposed fixes can use GitHub issues or pull requests,
without real wallet material or machine configuration.

Original contributions are released under **GPL-3.0-only**; see [LICENSE](LICENSE).
Commercial use and redistribution are permitted subject to the license's
requirements, including source availability when applicable. Third-party
components retain their own notices and licenses, documented in
[THIRD_PARTY.md](THIRD_PARTY.md). External drivers, models and runtime libraries
are not licensed by this repository.
