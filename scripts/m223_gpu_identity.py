#!/usr/bin/env python3
"""Fail-closed hardware gate for the primary NVIDIA GPU only.

The identity query itself is scoped by UUID to the one authorized primary NVIDIA GPU;
the utility does not enumerate other installed adapters.
"""
import os
import argparse
import csv
import subprocess
import sys

GPU_NAME = "NVIDIA GeForce primary NVIDIA GPU"
GPU_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", "GPU-00000000-0000-0000-0000-000000000000")
GPU_CC = "8.6"


def validate_csv(text: str) -> dict:
    rows = list(csv.reader(text.splitlines()))
    matches = []
    seen_uuids = set()
    for raw in rows:
        row = [field.strip() for field in raw]
        if len(row) != 3:
            raise ValueError(f"unparseable nvidia-smi identity row: {row!r}")
        name, uuid, cc = row
        if uuid in seen_uuids:
            raise ValueError(f"duplicate GPU UUID reported: {uuid}")
        seen_uuids.add(uuid)
        if uuid == GPU_UUID:
            matches.append((name, uuid, cc))
    if len(matches) != 1:
        raise ValueError(f"authorized primary NVIDIA GPU UUID not uniquely present: {len(matches)} matches")
    name, uuid, cc = matches[0]
    if name != GPU_NAME or cc != GPU_CC:
        raise ValueError(f"authorized GPU identity mismatch: name={name!r} UUID={uuid} CC={cc!r}")
    return {"name": name, "uuid": uuid, "compute_capability": cc}


def query(nvidia_smi: str) -> str:
    result = subprocess.run(
        [nvidia_smi, "--id=" + GPU_UUID, "--query-gpu=name,uuid,compute_cap", "--format=csv,noheader"],
        text=True, capture_output=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"nvidia-smi identity query failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nvidia-smi", default="nvidia-smi")
    parser.add_argument("--fish", action="store_true", help="print the verified UUID for Fish command substitution")
    args = parser.parse_args()
    identity = validate_csv(query(args.nvidia_smi))
    if args.fish:
        print(identity["uuid"])
    else:
        print("M223_GPU_IDENTITY=PASS " + " ".join(f"{key}={value}" for key, value in identity.items()))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"M223_GPU_IDENTITY=BLOCKED {exc}", file=sys.stderr)
        raise SystemExit(13)
