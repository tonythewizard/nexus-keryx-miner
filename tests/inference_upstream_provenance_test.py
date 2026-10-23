"""Verify pinned source snapshots without network, GPU, credentials or temporary files."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "f2561aedeb649da7b2c46fc4e845e487c9a7d9e5"
count = 0
for base, prefix in [
    ("vendor/keryx-trace-abi6", ""),
    ("vendor/keryx-verifiable-inference", "verifiable-inference/"),
    ("vendor/keryx-inference-v057/inference", "inference/"),
]:
    directory = ROOT / base
    metadata = json.loads((directory / "UPSTREAM.json").read_text())
    assert metadata["commit"] == PIN, base
    for upstream_path, upstream_hash in metadata["files"].items():
        assert upstream_path.startswith(prefix)
        local = upstream_path[len(prefix):]
        expected = metadata.get("local_patches", {}).get(local, {}).get("sha256", upstream_hash)
        actual = hashlib.sha256((directory / local).read_bytes()).hexdigest()
        assert actual == expected, (base, local, actual, expected)
        count += 1
print(f"UPSTREAM_PROVENANCE=PASS commit={PIN} files={count}")
