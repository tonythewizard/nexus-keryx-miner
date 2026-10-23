# Publication and private data

Only source, dependency lockfiles, reviewed synthetic protocol fixtures and
text-encoded build inputs belong in this repository. Keep keys, delegation
certificates, API credentials, payout configuration, machine profiles, logs,
journals, models, databases and generated artifacts outside Git.

## Required gates

Install Python 3.11+ and Gitleaks 8.30.1+, then:

```sh
git config core.hooksPath .githooks
python3 tools/security_gate.py
# Stage the specific source paths you reviewed.
python3 tools/security_gate.py --index
```

The pre-commit hook scans actual staged blobs, including their scanner config.
The pre-push hook scans the reachable history and checks the exact GitHub
destination and private visibility. CI runs history scanning again with
read-only permissions. Hooks are local Git configuration: each clone must
install them. Do not use `--no-verify` or disable the scanner.

The gate blocks unapproved paths/types, binaries, symlinks, submodules, files
over 2 MiB, personal home paths, physical device IDs, unreviewed wallet addresses,
delegations and common secrets. Gitleaks adds provider-specific secret rules.
Its only exceptions match two exact non-secret code expressions. Comments
containing `gitleaks:allow` do not bypass scans.

Protocol fixture addresses use public repeated test keys and are approved by
exact SHA-256 in `tools/publication-policy.json`. Never fund those addresses.
Adding an exception requires reviewing the fixture's origin, not copying an
installation value into the allowlist. Text-encoded assets have size/hash checks
in `tools/source-assets.json`; their materialized binary forms are ignored.

Automated scanning is a gate, not proof that every possible private datum was
detected. Review changed source and the complete history being pushed. Changes
to hooks, workflows, scanner rules or fixture policy need particular attention.

## Reports and later publication

Report sensitive issues privately to the repository owner. Do not include
credentials or operational evidence in issues, PRs or build logs. If a secret
was committed, stop publication, revoke it and review all reachable history;
deleting the current file is insufficient.

Before making the repository public, select the project license, review
third-party notices, rerun privacy/secret/history gates, inspect GitHub metadata
and artifacts, and deliberately update the private-only push policy. The current
workflow does not publish releases, packages or build artifacts.
