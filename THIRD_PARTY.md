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

License filenames are retained from the imported source. The files named
`LICENSE-MIT` in the miner and trace directories, and
`consensus/RESIDENT_KERNEL_LICENSE-MIT`, actually contain the Apache License
2.0 text, identical to the retained `LICENSE-APACHE` files. Their names alone
do not establish an MIT licensing option. This filename/content mismatch also
exists in the upstream miner at commit
`f2561aedeb649da7b2c46fc4e845e487c9a7d9e5`; its Cargo manifest declares
`MIT/Apache-2.0`, and the retained Apache 2.0 text provides that licensing path.
The inference and verifiable-inference license files contain the ISC license.
Preserve these notices alongside the license for this project's contributions.

The independent oracle needs the unmodified public Keryx node source at commit
`70ed12af172ac8dab9004b6ef11848f581b2a060`. Fetch it into ignored `_deps/`
using `tools/fetch_upstream.py`; it is not vendored or published here.

Cargo lockfiles record additional dependencies and their upstream locations.
GPU drivers, toolkits, inference runtime libraries and model weights are external
dependencies, not repository contents. The BDA shader is recovered SPIR-V
assembly, rather than an assertion that its original high-level source is present.
