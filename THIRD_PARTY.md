# Third-party source and notices

This project's original contributions are licensed under `GPL-3.0-only`;
see `LICENSE` and `README.md`. Third-party material retains its existing
licenses and notices. Do not replace or remove them with the project license.

| Source | Scope and retained notices |
| --- | --- |
| `vendor/keryx-inference-v057/` | Keryx inference formats and private key schedules at `4f46a984adc3`; `LICENSE`, `inference/UPSTREAM.json` |
| `vendor/keryx-miner-v057/` | Escrow implementation and RPC definitions; `LICENSE-APACHE`, `LICENSE-MIT`, `proto/SELECTED_CHAIN_UPSTREAM.json` |
| `vendor/keryx-trace-abi6/` | Trace formats, reference miner, wrapper source and its canonical-trace build patch; v5 reference has a separate `POM_V5_UPSTREAM.json` pin; isolated ABI9 wrapper/patches use `RUNTIME_ABI9_UPSTREAM.json`; `LICENSE-APACHE`, `LICENSE-MIT`, `UPSTREAM.json` |
| `vendor/keryx-verifiable-inference/` | Trace/sequence/attention/recurrent codec and verifier at `c21c87372604`; `LICENSE`, `UPSTREAM.json`, `SHAPE_UPSTREAM.json` |
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
`a92a343e11388c03cbdf18770d72fdb7a6c2130b`. Fetch it into ignored `_deps/`
using `tools/fetch_upstream.py`; it is not vendored or published here.

Cargo lockfiles record additional dependencies and their upstream locations.
GPU drivers, toolkits, inference runtime libraries and model weights are external
dependencies, not repository contents. The BDA shader is recovered SPIR-V
assembly, rather than an assertion that its original high-level source is present.

## Native preview build inputs

The native Windows preview also builds the public BLAKE2b reference source from
libb2 commit `2c5142f12a2cd52f3ee0a43e50a3a76f75badf85` (CC0), and
llama.cpp commit `12127defda4f41b7679cb2477a4b0d65ee6a0c8f` / b10015
(MIT), applying the retained Keryx canonical trace patch. Archive hashes and
build options are in `tools/cmake/blake2-reference.cmake` and
`tools/runtime/CMakeLists.txt`. Vcpkg inputs are pinned by
`tools/windows/vcpkg.json`; its installed notices accompany packaged components.

The preview packager selects the CUDA runtime and cuBLAS redistributables from
the matching toolkit and includes its NVIDIA EULA. Those DLLs remain subject to
their own terms; they are not GPL-licensed project code. NVIDIA drivers are
neither copied nor bundled. Microsoft runtime DLLs, when included, retain their
own redistribution terms. Original project source is identified by the exact
Git commit in `BUILD.json`; source tags and the pinned public dependency inputs
provide the corresponding build sources. No model license is implied.

The trace/verifier refresh at official miner `7a9a3935` retains upstream files
and notices, including separate residual feed-row proofs. Its node reference
comparison and local native adaptations are recorded in the corresponding
UPSTREAM manifests and `docs/upstream-refresh.md`. The inference wrapper and
five patches remain pinned at c21c8737; the forced quantized-kernel build policy
has an independent miner pin in the runtime sidecar.


The verifiable-inference source refresh at official miner `bc69a4e2` matches all
22 Rust source files of node `77c9037f`; retained ISC notices apply. Native trace
producer adaptations and exact source hashes are recorded in
`ai-response-bridge/src/TRACE_UPSTREAM.json` and
`vendor/keryx-verifiable-inference/UPSTREAM.json`. Runtime and oracle provenance
remain independently pinned; see `docs/upstream-refresh.md`.


Verifier refresh `331cb63a` imports three Rust files from the official miner;
all22 Rust sources match node `abe75b2a` with retained ISC notices. Exact hashes
and independent verifier/producer/runtime provenance are recorded in the existing
UPSTREAM manifests and `docs/upstream-refresh.md`.


Refresh `e237055b` imports the official trace-shape bounds and awaited-opening
RPC fields; all22 verifier Rust files match node `b13faafe` and retain ISC
notices. Native ledger scheduling/persistence and protocol transport are adapted
as recorded in `ai-response-bridge/src/TRACE_UPSTREAM.json`. Runtime/PoM oracle
provenance remains independently pinned. See `docs/upstream-refresh.md`.
