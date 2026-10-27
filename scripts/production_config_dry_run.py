#!/usr/bin/env python3
"""Read existing public mining config and invoke only NEXUS dry-run ingress."""
import os
import pathlib
import re
import secrets
import subprocess
import sys
from cryptography.hazmat.primitives.asymmetric import ec

root = pathlib.Path(sys.argv[1])
capture = sys.argv[2]
options = sys.argv[3:]
launcher = (pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/bin/keryx-miner-production.fish").read_text()
payout = re.search(r'--mining-address\s+(\S+)', launcher).group(1)
certificate = re.search(r'set -l CERT ([0-9a-fA-F]{128})', launcher).group(1)
key_path = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy/state/escrow.key"
if key_path.stat().st_mode & 0o077:
    raise SystemExit('escrow key permissions are not private')
secret_bytes = bytes.fromhex(key_path.read_text().strip())
public_x = ec.derive_private_key(int.from_bytes(secret_bytes, 'big'), ec.SECP256K1()).public_key().public_numbers().x.to_bytes(32, 'big').hex()
extra_data = f'NEXUS-REBUILT/escrow:{public_x}/esig:{certificate}/{secrets.token_hex(8)}/ai:v1:'
command = [
    'fish', str(root / 'source/scripts/run-m2-dry-run.fish'),
    'grpc://127.0.0.1:22110', payout, extra_data, capture, *options,
]
raise SystemExit(subprocess.run(command, check=False).returncode)
