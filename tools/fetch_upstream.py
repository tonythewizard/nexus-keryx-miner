#!/usr/bin/env python3
"""Fetch only the pinned, public, unmodified oracle dependency."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
URL = "https://github.com/Keryx-Labs/keryx-node.git"
COMMIT = "16d1864d835ab4c8359bb23a354351c12088b478"


def main():
    destination = ROOT / "_deps/keryx-node-16d1864d835a"
    if destination.exists():
        head = subprocess.check_output(["git", "-C", str(destination), "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(["git", "-C", str(destination), "status", "--porcelain"], text=True)
        if head != COMMIT or dirty:
            raise SystemExit("Existing upstream checkout differs; leave it intact and inspect it")
        print("Pinned upstream already present")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", str(destination)], check=True)
    subprocess.run(["git", "-C", str(destination), "remote", "add", "origin", URL], check=True)
    subprocess.run(["git", "-C", str(destination), "fetch", "--depth=1", "origin", COMMIT], check=True)
    subprocess.run(["git", "-C", str(destination), "checkout", "--detach", COMMIT], check=True)


if __name__ == "__main__":
    main()
