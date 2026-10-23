#!/usr/bin/env python3
import pathlib
import subprocess
import sys

oracle = pathlib.Path(sys.argv[1])
proof = pathlib.Path(sys.argv[2])
data = pathlib.Path(sys.argv[3])
source = proof.read_bytes()
corrupt = bytearray(source)
if len(corrupt) <= 97:
    raise SystemExit("proof too short for canonical v4 tile location")
corrupt[97] ^= 1
bad_tile = data / "negative-corrupt-tile.bin"
truncated = data / "negative-truncated-proof.bin"
bad_tile.write_bytes(corrupt)
truncated.write_bytes(source[:-1])
for mode, candidate in (("reject-tile", bad_tile), ("reject-target", proof),
                        ("reject-tier", proof), ("reject-borsh", truncated)):
    result = subprocess.run([str(oracle), mode, str(candidate)], check=False,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(f"oracle negative test {mode} failed with {result.returncode}")
