#!/usr/bin/env python3
"""Materialize hash-checked protocol inputs and the desktop icon from text."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def windows_icon(png):
    """Wrap the verified PNG unchanged in a single-image Windows ICO."""
    if len(png) < 33 or png[:8] != b"\x89PNG\r\n\x1a\n" or png[12:16] != b"IHDR":
        raise ValueError("Invalid desktop PNG")
    width, height = struct.unpack(">II", png[16:24])
    if not 1 <= width <= 256 or not 1 <= height <= 256 or png[24:26] != b"\x08\x06":
        raise ValueError("Desktop ICO requires an 8-bit RGBA PNG up to 256 pixels")
    header = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0,
                        1, 32, len(png), 22)
    return header + entry + png


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
    png = ROOT / "nexus-ui-desktop/src-tauri/icons/icon.png"
    ico = png.with_suffix(".ico")
    if ico.is_symlink() or not ico.parent.resolve().is_relative_to(ROOT):
        raise ValueError("Unsafe Windows icon destination")
    data = windows_icon(png.read_bytes())
    if not ico.exists() or ico.read_bytes() != data:
        ico.write_bytes(data)
    print(f"Prepared {len(assets)} verified local build inputs and Windows ICO")


if __name__ == "__main__":
    prepare()
