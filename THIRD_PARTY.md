# Third-party source and notices

This project's original contributions are licensed under `GPL-3.0-only`;
see `LICENSE` and `README.md`. Third-party material retains its existing
licenses and notices. Do not replace or remove them with the project license.

| Source | Scope and retained notices |
| --- | --- |
| `vendor/keryx-inference-v057/` | Keryx inference formats; `LICENSE`, `inference/UPSTREAM.json` |
| `vendor/keryx-miner-v057/` | Escrow implementation and RPC definitions; `LICENSE-APACHE`, `LICENSE-MIT`, `proto/SELECTED_CHAIN_UPSTREAM.json` |
| `vendor/keryx-trace-abi6/` | Trace formats, reference miner, wrapper source and its canonical-trace build patch; `LICENSE-APACHE`, `LICENSE-MIT`, `UPSTREAM.json` |
| `vendor/keryx-verifiable-inference/` | Trace codec/verifier; `LICENSE`, `UPSTREAM.json`, `SHAPE_UPSTREAM.json` |
| `consensus/` | Resident-kernel notice in `RESIDENT_KERNEL_LICENSE-MIT` |
| `proto/current/` | Upstream wire definitions and `VERIFIABLE_INFERENCE_UPSTREAM.json` |
| `ai-response-bridge/assets/trace-manifests/` | Text-encoded protocol manifests; `UPSTREAM.json` |

### Imported license filenames

The imported `LICENSE-MIT` files under `vendor/keryx-miner-v057/` and
`vendor/keryx-trace-abi6/`, as well as
`consensus/RESIDENT_KERNEL_LICENSE-MIT`, contain **Apache License 2.0** text,
not MIT license text. The names have been preserved for upstream traceability;
they must not be cited as evidence that those copies carry an MIT license.
The same mismatch appears in upstream miner commit
`f2561aedeb649da7b2c46fc4e845e487c9a7d9e5`. Its Cargo manifest declares
`MIT/Apache-2.0`, but the actual retained notices provide the Apache 2.0 text.
The inference and verifiable-inference `LICENSE` files contain ISC terms.

Do not rewrite imported notices or assume that the project's GPL-3.0-only
license replaces third-party terms. Before packaging binaries, review the
applicable notices and obligations for the exact sources and dependency
versions used in that build.

The independent oracle needs the unmodified public Keryx node source at commit
`70ed12af172ac8dab9004b6ef11848f581b2a060`. Fetch it into ignored `_deps/`
using `tools/fetch_upstream.py`; it is not vendored or published here.

Cargo lockfiles record additional dependencies and their upstream locations.
GPU drivers, toolkits, inference runtime libraries and model weights are external
dependencies, not repository contents. The BDA shader is recovered SPIR-V
assembly, rather than an assertion that its original high-level source is present.
