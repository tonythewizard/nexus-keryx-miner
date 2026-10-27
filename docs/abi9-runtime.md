# Isolated ABI9 inference runtime

ABI9 is an explicit runtime build candidate. The native adapter accepts ABI3/6/8/9; the standalone configure default remains ABI8. The 0.0.4 native CI explicitly
builds and packages ABI9; its runtime sidecar retains the experimental candidate flag. Building ABI9
is used by the separately validated [native H15 worker](h15-native-worker.md).

The candidate imports the unmodified wrapper and patches from Keryx miner
`c21c873726049195a3a40af6f19a013fc44ebd82`, under the scoped provenance record
`vendor/keryx-trace-abi6/RUNTIME_ABI9_UPSTREAM.json`. The legacy wrapper is kept
at its original path; the ABI9 wrapper is in the `abi9/` subdirectory. Both use
llama.cpp `12127defda4f41b7679cb2477a4b0d65ee6a0c8f` and its pinned archive hash.

The ordered patch chain is:

1. Canonical trace capture.
2. Sequence leaves, attention/recurrent capture and replay, including the
   updated capture bounds and recurrent checkpoints.
3. F32 accumulator tile flash attention.
4. RoPE angles computed in double precision.
5. Committed attention log-softmax mass capture.

`tools/runtime/abi9-patches.json` pins all five patch inputs, the wrapper, and
the before/after hashes of all thirteen touched source files at every stage.
The apply helper recognizes a pristine source tree, a known patch prefix, or
the complete chain. It applies only the missing suffix, checks each stage,
and refuses altered inputs, unknown partial states and incomplete results.
Verification uses full file hashes because later patches modify earlier
patches' hunks; independently reverse-checking every patch on the final tree
would reject a valid ordered chain. Git worktree/index overrides and discovery
of an enclosing repository are excluded during patch application. Explicit LF
attributes preserve hash-pinned inputs on Windows checkouts.

Build on Linux with CUDA Toolkit 13, an SM86 target and serial compilation:

```sh
cmake -S tools/runtime -B .local/tmp/runtime-abi9 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEXUS_RUNTIME_ABI=9
cmake --build .local/tmp/runtime-abi9 --target nexus_inference_runtime --parallel 1
cmake --install .local/tmp/runtime-abi9 --component nexus-runtime \
  --prefix "$PWD/.local/tmp/runtime-abi9-stage"
python3 tools/runtime/check_runtime.py \
  .local/tmp/runtime-abi9-stage/libkeryx-llama.so --expected-abi 9
```

Use persistent temporary directories and an appropriate local build memory
budget. The source-stage checks support an exact ABI8 patch prefix for an
incremental upgrade; use a fresh source/build directory to return to ABI8.
Archive builds have no upstream `.git` metadata; their authoritative source
identity is the pinned archive/patch manifest, rather than an enclosing
repository's Git identity.

The staged `NEXUS_RUNTIME.json` records ABI, wrapper hash, patch-set hash and
upstream pins, with `candidate_only=true` for ABI9. The CLI packager requires this sidecar. ABI9 packaging uses explicit
`--runtime-abi 9` with matching candidate metadata; default ABI8 packaging rejects it.
No automatic update or replacement of an installed inference library occurs.

The runtime check loads the shared library, verifies the actual ABI and named
C exports, and tests rejected null-handle sequence calls with output canaries.
On Linux it verifies that the expected C symbols are defined in the library,
separately from additional legitimate C++-mangled symbols. This check does not
load a model, run a device probe or dispatch inference. Dependency/relocation
closure is an additional Linux check (`ldd -r`).

Validation of this increment: Linux runtime compilation/linking, exact patch
and source verification, ABI9 exports, rejected null handles, ABI mismatch
refusal, ABI8 configuration/probe regression, and 28 distribution tests passed.
The Windows coverage simulates checkout line endings; no new native Windows
build or GPU run is claimed.

The [native ABI9 adapter and current verifier/manifests](abi9-adapter.md) now
provide bounded sequence capture/replay interfaces and offline verification.
Canonical graph/cache settings are set before the wrapper loads. GPU capture parity, real Qwen inference, replay-to-proof assembly and persistent
sequence/opening recovery are connected in the native CUDA tier-0 worker. Other
routes retain their H15 guard; live H15 acceptance remains unvalidated.

The refresh at miner `43b0d439` keeps ABI9 but changes its sequence layout.
An earlier ABI9 library without patch 0005 lacks the mass leaves; it must be
rebuilt and the adapter rejects its incomplete root list. Runtime ABI alone is
insufficient provenance: retain the exact patch manifest and library checksum.

Patch 0005 was revised at `c21c8737` without changing the application ABI or patch
count. The new runtime refuses sequence roots unless each attention layer has
a complete log-softmax-mass stream. Earlier completed patch-0005 sources are a
different revision and fail source verification under the new manifest; use a
fresh source or a verified four-patch prefix. See [upstream-refresh.md](upstream-refresh.md).

The 7a9a3935 miner refresh changes build policy without changing the wrapper or
five source patches: `GGML_CUDA_FORCE_MMQ=ON` forces the quantized kernels that
capture traces. The sidecar records `force_mmq` and `build_policy_miner_commit`
independently of `miner_commit`, which remains the wrapper/patch provenance pin.
Use the newly built candidate's exact library hash for validation; an older ABI9
library does not establish that build setting. Default package policy is unchanged.
