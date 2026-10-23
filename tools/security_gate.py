#!/usr/bin/env python3
"""Fail closed on publication of private files, host identities or secrets.

Default scans source files selected for Git (tracked and non-ignored untracked).
--index scans the actual staged blobs. --history scans every reachable commit
and invokes Gitleaks on the complete history. Findings never echo file contents.
"""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AREAS = {"src", "include", "consensus", "ai-response-bridge", "oracle-rust",
         "vendor", "proto", "nexus-ui-desktop", "experiments", "scripts",
         "tests", "tools", ".github", ".githooks", "docs"}
ROOT_FILES = {"README.md", "devkit.md", "AGENTS.md", "SECURITY.md", "THIRD_PARTY.md",
              "VERSION", "LICENSE", "CMakeLists.txt", ".gitignore", ".gitleaks.toml"}
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".cu", ".cuh", ".rs", ".py", ".fish",
            ".sh", ".toml", ".lock", ".json", ".proto", ".html", ".css", ".js",
            ".cjs", ".svg", ".spvasm", ".hex", ".md", ".txt", ".yml", ".yaml", ".inc", ".cmake", ".ps1", ".manifest"}
FORBIDDEN_PARTS = {"state", "logs", "cache", "artifacts", "models", "handoff",
                   "agent-api", "node_modules", "target", "build", ".aws", ".codex", ".ssh", "_deps"}
SYNTHETIC_UUID = "GPU-00000000-0000-0000-0000-000000000000"
PATTERNS = {
    "personal home path": re.compile(rb"(?:/home/|/Users/)[A-Za-z0-9_.-]+|[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+"),
    "private node material": re.compile(rb"nodo" + rb"fast|keryx-node-" + rb"fast", re.I),
    "private key block": re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----"),
    "wallet delegation": re.compile(rb"/esig:[0-9a-fA-F]{128}"),
    "GitHub credential": re.compile(rb"gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}"),
    "private network address": re.compile(rb"\b(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3})\b"),
}
# These are upstream protocol fixtures derived from public, repeated test keys.
# The set contains only their hashes. New wallet fixtures require explicit review.
FIXTURE_ADDRESS_HASHES = set()  # populated from the reviewed policy file below
PUBLIC_SCREENSHOT_HASHES = {}  # exact Git paths and SHA-256 digests, from policy


def reviewed_screenshot(name, data, mode):
    """Admit only exact, hash-pinned, metadata-free public gallery PNGs."""
    if name not in PUBLIC_SCREENSHOT_HASHES or mode != "100644":
        return False
    if not 64 <= len(data) <= 256 * 1024:
        return False
    if not data.startswith(b"\x89PNG\r\n\x1a\n") or data[12:16] != b"IHDR":
        return False
    if data[8:12] != b"\x00\x00\x00\r" or not data.endswith(b"IEND\xaeB`\x82"):
        return False
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    if not 1 <= width <= 2000 or not 1 <= height <= 2000:
        return False
    return hashlib.sha256(data).hexdigest() == PUBLIC_SCREENSHOT_HASHES[name]


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def violations(name, data, mode="100644"):
    problems = []
    if name.startswith("docs/screenshots/") and name.endswith(".png"):
        return [] if reviewed_screenshot(name, data, mode) else ["unreviewed or modified public screenshot"]
    p = PurePosixPath(name)
    if p.is_absolute() or ".." in p.parts or any(c in name for c in "\n\r\t\\"):
        problems.append("unsafe filename")
    if mode not in ("100644", "100755"):
        problems.append("symlinks and submodules are forbidden")
    if any(part in FORBIDDEN_PARTS or part.startswith(".env") for part in p.parts):
        problems.append("private/generated path")
    if name not in ROOT_FILES and (len(p.parts) < 2 or p.parts[0] not in AREAS):
        problems.append("unapproved source area")
    if (p.suffix not in SUFFIXES and not (p.suffix == ".patch" and p.parts[0] == "vendor") and p.name not in ("CMakeLists.txt", "LICENSE", "LICENSE-APACHE", "LICENSE-MIT", "RESIDENT_KERNEL_LICENSE-MIT", ".gitignore", "pre-commit", "pre-push")) and name not in ROOT_FILES:
        problems.append("unapproved file type")
    if len(data) > 2 * 1024 * 1024 or b"\0" in data:
        problems.append("binary or oversized file")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        problems.append("non-text file")
    for label, pattern in PATTERNS.items():
        if pattern.search(data):
            problems.append(label)
    for value in re.findall(rb"GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", data):
        if value.decode() != SYNTHETIC_UUID:
            problems.append("physical GPU UUID")
    for value in re.findall(rb"(?:pci:)?[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", data):
        if not re.fullmatch(r"0000:00:0[0-7]\.[01]", value.decode().removeprefix("pci:")):
            problems.append("physical PCI placement")
    for value in re.findall(rb"keryx:[a-z0-9]{30,}", data):
        if hashlib.sha256(value).hexdigest() not in FIXTURE_ADDRESS_HASHES:
            problems.append("unreviewed wallet address")
    return sorted(set(problems))


def snapshot(index=False):
    if index:
        for item in git("ls-files", "--stage", "-z").split(b"\0"):
            if not item:
                continue
            meta, path = item.split(b"\t", 1)
            mode, oid, stage = meta.decode().split()
            if stage != "0":
                raise ValueError("Unmerged index")
            yield path.decode(), git("cat-file", "blob", oid), mode
    else:
        names = set(git("ls-files", "-z", "--cached", "--others", "--exclude-standard").split(b"\0")) - {b""}
        for raw in sorted(names):
            name = raw.decode()
            path = ROOT / name
            if path.is_symlink():
                yield name, b"", "120000"
            elif path.is_file():
                yield name, path.read_bytes(), "100644"


def history():
    # HEAD also covers an initial local branch before it has any remote refs.
    commits = git("rev-list", "--all", "HEAD").decode().splitlines()
    seen = set()
    for commit in commits:
        for item in git("ls-tree", "-rz", "--full-tree", commit).split(b"\0"):
            if not item:
                continue
            meta, path = item.split(b"\t", 1)
            mode, kind, oid = meta.decode().split()
            key = (path, oid, mode)
            if key in seen:
                continue
            seen.add(key)
            yield path.decode(), git("cat-file", "blob", oid) if kind == "blob" else b"", mode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--index", action="store_true")
    parser.add_argument("--history", action="store_true")
    args = parser.parse_args()
    if args.index and args.history:
        parser.error("Choose index or history")
    import json
    global FIXTURE_ADDRESS_HASHES, PUBLIC_SCREENSHOT_HASHES
    # Read this policy from Git too; the index/history must not depend on edits
    # to a working-tree allowlist that will not be published.
    if args.index:
        policy = git("show", ":tools/publication-policy.json")
    elif args.history:
        policy = git("show", "HEAD:tools/publication-policy.json")
    else:
        policy = (ROOT / "tools/publication-policy.json").read_bytes()
    parsed_policy = json.loads(policy)
    FIXTURE_ADDRESS_HASHES = set(parsed_policy["upstream_fixture_address_sha256"])
    PUBLIC_SCREENSHOT_HASHES = dict(parsed_policy.get("public_screenshot_sha256", {}))
    records = list(history() if args.history else snapshot(args.index))
    count = 0
    failed = False
    for name, data, mode in records:
        count += 1
        for reason in violations(name, data, mode):
            print(f"BLOCKED {name}: {reason}", file=sys.stderr)
            failed = True
    if failed:
        return 1
    executable = shutil.which("gitleaks")
    if not executable:
        print("BLOCKED: install Gitleaks; secret scanning is mandatory", file=sys.stderr)
        return 1
    env = os.environ.copy()
    for key in ("GITLEAKS_CONFIG", "GITLEAKS_CONFIG_TOML"):
        env.pop(key, None)
    # Materialize only source selected for publication, so untracked runtime
    # output is neither scanned nor used as evidence for staged source.
    git_dir = Path(git("rev-parse", "--absolute-git-dir").decode().strip())
    with tempfile.TemporaryDirectory(prefix="publication-", dir=git_dir) as directory:
        staging = Path(directory)
        if args.history:
            config = git("show", "HEAD:.gitleaks.toml")
        else:
            for name, data, _ in records:
                output = staging / name
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(data)
            config = (staging / ".gitleaks.toml").read_bytes()
        scanner_config = staging / "scanner.toml"
        scanner_config.write_bytes(config)
        ignore = staging / "empty-ignore"
        ignore.write_text("")
        common = ["--redact", "--no-banner", "--no-color", "--ignore-gitleaks-allow",
                  "--config", str(scanner_config), "--gitleaks-ignore-path", str(ignore)]
        command = [executable, "git", str(ROOT), "--log-opts=--all HEAD"] if args.history else [executable, "dir", str(staging)]
        result = subprocess.run([*command, *common], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if result.returncode:
        print("BLOCKED: Gitleaks failed or found a secret (run it with --redact for details)", file=sys.stderr)
        return 1
    print(f"PASS publication gate: {count} text source blobs; privacy and Gitleaks checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
