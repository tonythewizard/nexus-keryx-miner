# Agent entry point

Read `devkit.md`, then `SECURITY.md`, before changing this project. Read the
specific module and its tests instead of searching unrelated machine data.

- This is a developer repository with experimental CLI build artifacts. Keep runtime data and local
  device profiles outside Git. Do not import other repositories' Git history.
- Install hooks with `git config core.hooksPath .githooks`. Gitleaks is mandatory.
- Run `python3 tools/security_gate.py` before staging selected files and
  `python3 tools/security_gate.py --index` after staging. Never bypass a gate.
- Use `.local/tmp` for temporary test data and out-of-source builds. Generated
  protocol inputs come from `python3 tools/prepare_assets.py` and stay ignored.
- Configure GPU identities locally. Zero UUID/PCI values are inert examples.
  Preserve device identity, capability checks, exclusive ownership and parity.
- Do not launch GPU jobs, mining, submissions, service restarts or wallet
  changes as a side effect of building, inspecting or testing source.
- Preserve proof validation, canonical encoding, job freshness, AI priority,
  signing, journal recovery and duplicate-submission checks.
- Match tests to the change. Distinguish CPU/mock tests, GPU execution and live
  chain acceptance. Never claim a skipped test or an older binary's result.
- Keep upstream notices and pinned provenance. Do not select a project license
  or make the repository public without the owner's instruction.
