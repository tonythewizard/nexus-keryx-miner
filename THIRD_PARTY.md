# Third-party source and notices

No project-wide license is selected yet. Do not replace or remove the licenses
already attached to third-party source while choosing the project's license.

| Source | Scope and retained notices |
| --- | --- |
| `vendor/keryx-inference-v057/` | Keryx inference formats; `LICENSE`, `inference/UPSTREAM.json` |
| `vendor/keryx-miner-v057/` | Escrow implementation and RPC definitions; `LICENSE-APACHE`, `LICENSE-MIT`, `proto/SELECTED_CHAIN_UPSTREAM.json` |
| `vendor/keryx-trace-abi6/` | Trace formats, reference miner, wrapper source and its canonical-trace build patch; `LICENSE-APACHE`, `LICENSE-MIT`, `UPSTREAM.json` |
| `vendor/keryx-verifiable-inference/` | Trace codec/verifier; `LICENSE`, `UPSTREAM.json`, `SHAPE_UPSTREAM.json` |
| `consensus/` | Resident-kernel notice in `RESIDENT_KERNEL_LICENSE-MIT` |
| `proto/current/` | Upstream wire definitions and `VERIFIABLE_INFERENCE_UPSTREAM.json` |
| `ai-response-bridge/assets/trace-manifests/` | Text-encoded protocol manifests; `UPSTREAM.json` |

The independent oracle needs the unmodified public Keryx node source at commit
`70ed12af172ac8dab9004b6ef11848f581b2a060`. Fetch it into ignored `_deps/`
using `tools/fetch_upstream.py`; it is not vendored or published here.

Cargo lockfiles record additional dependencies and their upstream locations.
GPU drivers, toolkits, inference runtime libraries and model weights are external
dependencies, not repository contents. The BDA shader is recovered SPIR-V
assembly, rather than an assertion that its original high-level source is present.
