#!/usr/bin/env python3
"""Inspect or explicitly import a Keryx Wallet escrow delegation certificate.

This helper never signs. Signing remains in Keryx Wallet. It only verifies the
wallet's 128-hex Schnorr certificate with the stock Rust bridge and, after an
explicit import, atomically stores a verified certificate beside local state.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import stat

RECOVERY = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner")))
APP = pathlib.Path(os.environ.get("NEXUS_DATA_DIR", str(pathlib.Path.home() / ".local/share/nexus-keryx-miner"))) / "legacy"
DEFAULT_LAUNCHER = APP / "bin/keryx-miner-production.fish"
DEFAULT_KEY = APP / "state/escrow.key"
DEFAULT_CERT = APP / "state/escrow.cert"
DEFAULT_PROBE = RECOVERY / "build/nexus-ui-inference/bin/nexus-ai-service-probe"
CERT_RE = re.compile(r"^[0-9a-fA-F]{128}$")


def configured_paths(launcher, key_path, cert_path):
    from nexus_setup import active_profile
    profile = active_profile() if launcher == DEFAULT_LAUNCHER and key_path == DEFAULT_KEY else None
    if profile:
        return launcher, pathlib.Path(profile['key_path']), pathlib.Path(profile['cert_path']) if cert_path == DEFAULT_CERT else cert_path
    return launcher, key_path, cert_path


def resolve_config(launcher: pathlib.Path, key_path: pathlib.Path):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from nexus_force_shadow import derive_escrow_pubkey, parse_stock_mining_fields

    from nexus_setup import active_profile
    profile = active_profile() if launcher == DEFAULT_LAUNCHER else None
    if profile and key_path in (DEFAULT_KEY, pathlib.Path(profile['key_path'])):
        key_path = pathlib.Path(profile['key_path'])
    else:
        profile = None
    key_stat = key_path.stat()
    if not stat.S_ISREG(key_stat.st_mode) or key_stat.st_mode & 0o077:
        raise ValueError("existing escrow key is not a private regular file")
    if profile:
        payout, legacy_cert = profile['payout'], read_certificate(pathlib.Path(profile['cert_path']))
    else:
        payout, legacy_cert = parse_stock_mining_fields(launcher.read_text(encoding="utf-8"))
    pubkey = derive_escrow_pubkey(str(key_path))
    if profile and pubkey != profile['pubkey']:
        raise ValueError('configured escrow key changed; launch blocked')
    return payout, pubkey, legacy_cert


def read_certificate(path: pathlib.Path) -> str:
    raw = path.read_text(encoding="ascii").strip()
    if not CERT_RE.fullmatch(raw):
        raise ValueError("wallet certificate must contain exactly 128 hexadecimal characters")
    return raw.lower()


def verify_certificate(payout: str, pubkey: str, cert: str,
                       key_path: pathlib.Path, probe: pathlib.Path) -> bool:
    if len(pubkey) != 64 or not re.fullmatch(r"[0-9a-f]{64}", pubkey):
        return False
    extra = f"/escrow:{pubkey}/esig:{cert}"
    result = subprocess.run(
        [str(probe), "--verify-escrow", payout, extra, str(key_path)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, check=False, timeout=8,
    )
    # The probe emits only public identity/validity metadata; never relay its
    # command line, key contents, or certificate into the UI or persistent logs.
    return result.returncode == 0 and "delegation_valid=true" in result.stdout


def selected_certificate(launcher: pathlib.Path, key_path: pathlib.Path,
                         cert_path: pathlib.Path, probe: pathlib.Path):
    launcher, key_path, cert_path = configured_paths(launcher, key_path, cert_path)
    payout, pubkey, legacy_cert = resolve_config(launcher, key_path)
    if cert_path.exists():
        cert = read_certificate(cert_path)
        if not verify_certificate(payout, pubkey, cert, key_path, probe):
            raise ValueError("stored wallet certificate does not match the configured payout and escrow key")
        source = "LOCAL_WALLET_IMPORT"
    else:
        cert = legacy_cert
        if not verify_certificate(payout, pubkey, cert, key_path, probe):
            raise ValueError("configured wallet delegation is invalid")
        source = "EXISTING_MINER_CONFIG"
    return payout, pubkey, cert, source


def atomic_store(path: pathlib.Path, cert: str):
    if not path.parent.is_dir():
        raise ValueError("persistent escrow state directory is unavailable")
    temporary = path.with_name(f".{path.name}.{os.getpid()}.new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(cert + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
        dirfd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def command_status(args):
    try:
        payout, pubkey, _cert, source = selected_certificate(
            args.launcher, args.key, args.cert_store, args.probe)
        print(json.dumps({"authorization": "AUTHORIZED", "payout_address": payout,
                          "escrow_public_key": pubkey, "certificate_source": source,
                          "verification": "VALID", "error": None}, separators=(",", ":")))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError):
        try:
            payout, pubkey, _ = resolve_config(args.launcher, args.key)
        except Exception:
            payout, pubkey = "N/A", "N/A"
        print(json.dumps({"authorization": "INVALID", "payout_address": payout,
                          "escrow_public_key": pubkey, "certificate_source": "N/A",
                          "verification": "FAILED", "error": "certificate or escrow identity validation failed"}, separators=(",", ":")))
        return 3


def command_import(args):
    try:
        args.launcher, args.key, args.cert_store = configured_paths(args.launcher, args.key, args.cert_store)
        if args.certificate.resolve().is_relative_to(pathlib.Path("/tmp")):
            raise ValueError("temporary paths are not accepted")
        payout, pubkey, legacy_cert = resolve_config(args.launcher, args.key)
        imported = read_certificate(args.certificate)
        if not verify_certificate(payout, pubkey, imported, args.key, args.probe):
            print("WALLET_IMPORT=REJECTED reason=payout_or_escrow_key_mismatch")
            return 3
        if args.cert_store.exists():
            try:
                current_cert = read_certificate(args.cert_store)
                current_valid = verify_certificate(payout, pubkey, current_cert, args.key, args.probe)
            except (OSError, ValueError):
                current_cert, current_valid = "", False
            if current_valid:
                if imported != current_cert:
                    print("WALLET_IMPORT=REJECTED reason=refusing_to_replace_valid_certificate")
                    return 3
                print("WALLET_IMPORT=AUTHORIZED existing_valid_certificate_preserved=true")
                return 0
        else:
            current_cert = legacy_cert
            current_valid = verify_certificate(payout, pubkey, legacy_cert, args.key, args.probe)
        if current_valid:
            if imported != current_cert:
                print("WALLET_IMPORT=REJECTED reason=refusing_to_replace_valid_certificate")
                return 3
            print("WALLET_IMPORT=AUTHORIZED existing_valid_certificate_preserved=true")
            return 0
        atomic_store(args.cert_store, imported)
        print("WALLET_IMPORT=PASS certificate_verified=true identity_preserved=true")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError):
        print("WALLET_IMPORT=REJECTED reason=local_validation_or_storage_failure")
        return 3


def add_paths(parser):
    parser.add_argument("--launcher", type=pathlib.Path, default=DEFAULT_LAUNCHER)
    parser.add_argument("--key", type=pathlib.Path, default=DEFAULT_KEY)
    parser.add_argument("--cert-store", type=pathlib.Path, default=DEFAULT_CERT)
    parser.add_argument("--probe", type=pathlib.Path, default=DEFAULT_PROBE)


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    status = sub.add_parser("status", help="verify the configured wallet delegation read-only")
    add_paths(status)
    status.set_defaults(run=command_status)
    resolve = sub.add_parser("resolve", help="emit the verified payout and template extra-data fields")
    add_paths(resolve)
    resolve.set_defaults(run=lambda args: _resolve(args))
    key_path = sub.add_parser('key-path', help='resolve the configured private key path without printing its contents')
    add_paths(key_path)
    key_path.set_defaults(run=lambda args: _key_path(args))
    importing = sub.add_parser("import", help="verify and explicitly import a wallet-produced certificate")
    add_paths(importing)
    importing.add_argument("--certificate", required=True, type=pathlib.Path)
    importing.set_defaults(run=command_import)
    args = parser.parse_args()
    return args.run(args)


def _resolve(args):
    try:
        payout, pubkey, cert, _source = selected_certificate(
            args.launcher, args.key, args.cert_store, args.probe)
    except (OSError, ValueError, subprocess.SubprocessError):
        print("WALLET_RESOLVE=BLOCKED reason=authorization_invalid", file=sys.stderr)
        return 3
    # Captured by the Fish launcher, never echoed into its persistent log.
    print(payout)
    print(f"/escrow:{pubkey}/esig:{cert}")
    return 0


def _key_path(args):
    try:
        _, key, _ = configured_paths(args.launcher, args.key, args.cert_store)
        print(key)
        return 0
    except (OSError, ValueError):
        print('KEY_PATH=BLOCKED invalid setup configuration', file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
