# Security policy

## Supported scope

Version 0.0.4 is an experimental source baseline, not a supported binary
release. Maintainers review reports against the current `main` branch; no
security maintenance period or response-time guarantee is promised for older
commits or external models, drivers and services.

## Publication and private data

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
destination. CI runs history scanning again with
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

## Reporting vulnerabilities

**Do not disclose an unpatched vulnerability in a public issue, discussion or
pull request.** Use **Security → Advisories → Report a vulnerability** on this
repository if GitHub private vulnerability reporting is enabled. Include the
affected revision, impact, reproduction steps using synthetic inputs and any
proposed fix. Avoid submitting real credentials, model data or wallet material.

If the private reporting option is unavailable, the repository currently has
no verified confidential contact endpoint. Do not publish exploit details as a
workaround; the maintainer must enable a private reporting channel. Ordinary
non-security bug reports and reviewed fixes may use public issues and pull
requests. No guaranteed response or disclosure deadline is advertised.

If a secret was committed, revoke it immediately, stop publication and review
all reachable history; deleting the current file is insufficient.

## Public source publication

This repository publishes source under GPL-3.0-only, with retained third-party
licenses and notices. Before pushing, rerun privacy/secret/history gates and
review the staged files and commit metadata. Keep operational data private even
though the source repository is public. The pre-push hook still restricts the
destination to this project's GitHub repository and requires history scanning.
The native workflow can publish unsigned experimental build artifacts from
reviewed build outputs. Source gates still exclude runtime data and unapproved
binaries from Git. Package manifests identify the exact source commit and
validation scope.
