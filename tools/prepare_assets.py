#!/usr/bin/env python3
"""Materialize hash-checked protocol inputs and the desktop icon from text."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare():
    assets = json.loads((ROOT / "tools/source-assets.json").read_text())
    for name, expected in assets.items():
        output = ROOT / name
        if Path(name).is_absolute() or ".." in Path(name).parts or output.is_symlink():
            raise ValueError("Unsafe asset destination")
        if not output.parent.resolve().is_relative_to(ROOT):
            raise ValueError("Asset destination escaped checkout")
        data = bytes.fromhex(output.with_name(output.name + ".hex").read_text())
        if len(data) != expected["bytes"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
            raise ValueError("Source asset integrity failure: " + name)
        if output.exists() and output.read_bytes() == data:
            continue
        output.write_bytes(data)
    print(f"Prepared {len(assets)} verified local build inputs")


if __name__ == "__main__":
    prepare()
