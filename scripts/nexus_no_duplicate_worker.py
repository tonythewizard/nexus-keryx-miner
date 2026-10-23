#!/usr/bin/env python3
"""Refuse a candidate launch while any NEXUS CUDA worker executable is alive."""
import os
import sys
from pathlib import Path

matches = []
for proc in Path("/proc").glob("[0-9]*"):
    try:
        pid = int(proc.name)
        exe = os.readlink(proc / "exe").removesuffix(" (deleted)")
        if Path(exe).name in ("nexus-live-cuda-worker", "keryx-miner"):
            matches.append((pid, exe))
    except (OSError, ValueError):
        continue
if matches:
    for pid, exe in matches:
        print(f"NEXUS_DUPLICATE_WORKER=BLOCKED pid={pid} executable={exe}")
    raise SystemExit(13)
print("NEXUS_DUPLICATE_WORKER=PASS no_existing_worker=true")
