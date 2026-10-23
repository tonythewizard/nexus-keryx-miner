"""Private first-run mining identity setup; never applies changes to a live worker."""
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import stat
from nexus_agent_api import ROOT, atomic_json, worker_identity

STATE = ROOT / 'state/setup'
PROFILE = STATE / 'mining.json'
DRAFT = STATE / 'draft.json'


def private_read(path, limit=4096):
    path = Path(path)
    if not path.is_absolute() or path.resolve().is_relative_to(Path('/tmp')) or path.resolve().is_relative_to(Path('/var/tmp')):
        raise ValueError('Use an absolute persistent file path')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_size > limit:
            raise ValueError('File must belong to you, be private (0600), and within size bounds')
        return stream.read()


def active_profile():
    try:
        obj = json.loads(private_read(PROFILE))
    except FileNotFoundError:
        return None
    if not isinstance(obj, dict) or obj.get('schema_version') != 1 or obj.get('verified') is not True:
        raise ValueError('Invalid mining configuration; launch blocked')
    if not isinstance(obj.get('payout'), str) or not re.fullmatch(r'keryx:[a-z0-9]{40,120}', obj['payout']):
        raise ValueError('Invalid payout configuration')
    if not isinstance(obj.get('pubkey'), str) or not re.fullmatch(r'[0-9a-f]{64}', obj['pubkey']):
        raise ValueError('Invalid escrow public identity')
    for name in ('key_path', 'cert_path'):
        if not isinstance(obj.get(name), str) or not obj[name] or len(obj[name]) > 1024:
            raise ValueError('Invalid private configuration path')
        private_read(obj[name])
    return obj


def draft():
    try:
        obj = json.loads(private_read(DRAFT))
        if not isinstance(obj, dict): raise ValueError('Invalid setup draft')
        if any(obj.get(k) is not None and not isinstance(obj[k], str) for k in ('payout','key_path','cert_path','pubkey')):
            raise ValueError('Invalid setup draft fields')
        return obj
    except FileNotFoundError:
        profile = active_profile()
        if profile: return {k: profile[k] for k in ('payout', 'key_path', 'cert_path', 'pubkey')}
        from nexus_wallet_authorization import DEFAULT_LAUNCHER, DEFAULT_KEY, DEFAULT_CERT, resolve_config
        try:
            payout, pubkey, _ = resolve_config(DEFAULT_LAUNCHER, DEFAULT_KEY)
            return {'payout': payout, 'key_path': str(DEFAULT_KEY), 'pubkey': pubkey,
                    'cert_path': str(DEFAULT_CERT) if DEFAULT_CERT.exists() else None,
                    'legacy_certificate': True}
        except (OSError, ValueError, RuntimeError):
            return {}


def public_view():
    obj = draft()
    return {k: obj.get(k) for k in ('payout', 'key_path', 'cert_path', 'pubkey')}


def protect_existing(payout=None, pubkey=None):
    from nexus_wallet_authorization import DEFAULT_LAUNCHER, DEFAULT_KEY, resolve_config
    try:
        current_pay, current_pub, _ = resolve_config(DEFAULT_LAUNCHER, DEFAULT_KEY)
    except (OSError, ValueError, RuntimeError):
        current_pay = current_pub = None
    # The current installation's existing economic identity remains protected,
    # even when the user has stopped mining. Setup does not authorize rotation.
    if payout and current_pay and payout != current_pay:
        raise ValueError('Existing payout is protected; current identity preserved')
    if pubkey and current_pub and pubkey != current_pub:
        raise ValueError('Existing escrow identity is protected; current key preserved')
    for worker in worker_identity():
        if worker['kind'] != 'nexus-live-cuda-worker': continue
        argv = Path('/proc', str(worker['pid']), 'cmdline').read_bytes().split(b'\0')
        if len(argv) < 4: raise ValueError('Active worker identity unavailable')
        public = re.search(rb'/escrow:([0-9a-f]{64})', argv[3])
        if (payout and argv[2].decode() != payout) or (pubkey and (not public or public[1].decode() != pubkey)):
            raise ValueError('Active worker identity is protected; no live changes')


def store_text(path, text):
    from nexus_wallet_authorization import atomic_store
    if path.exists():
        if private_read(path).strip() != text.strip():
            raise ValueError('Existing setup file is protected; refusing replacement')
        return
    atomic_store(path, text.strip())


def configure(action, value=''):
    from nexus_force_shadow import derive_escrow_pubkey
    from nexus_wallet_authorization import (DEFAULT_KEY, DEFAULT_LAUNCHER, DEFAULT_CERT,
                                            DEFAULT_PROBE, selected_certificate, read_certificate, verify_certificate)
    if STATE.is_symlink() or STATE.resolve().is_relative_to(Path('/tmp')) or STATE.resolve().is_relative_to(Path('/var/tmp')):
        raise ValueError('Setup storage must be a persistent private directory')
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    if STATE.stat().st_uid != os.getuid() or STATE.stat().st_mode & 0o077:
        raise ValueError('Setup directory must belong to you and be private (0700)')
    fd = os.open(STATE / 'operation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        obj = draft()
        if action == 'payout':
            if not re.fullmatch(r'keryx:[a-z0-9]{40,120}', value):
                raise ValueError('Enter a mainnet Keryx payout address; final checksum/signature verification uses the official probe')
            protect_existing(payout=value)
            obj['payout'] = value
        elif action in ('key', 'new_key'):
            if action == 'new_key':
                if DEFAULT_KEY.exists() or obj.get('key_path') or worker_identity():
                    raise ValueError('Existing identity found; import/reuse it instead of generating a key')
                if any((ROOT / 'state/ai-service').glob('*journal*')):
                    raise ValueError('Existing service history found; restore the original escrow key')
                order = 0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141
                secret = format(secrets.randbelow(order - 1) + 1, '064x')
            else:
                secret = private_read(Path(value)).strip().lower()
                if not re.fullmatch('[0-9a-f]{64}', secret): raise ValueError('Escrow key file must contain 64 hex characters')
                pubkey = derive_escrow_pubkey(value)
                protect_existing(pubkey=pubkey)
            key = STATE / 'escrow.key'
            store_text(key, secret)
            pubkey = derive_escrow_pubkey(str(key))
            protect_existing(pubkey=pubkey)
            obj.update(key_path=str(key), pubkey=pubkey)
        elif action == 'certificate':
            if not obj.get('payout') or not obj.get('key_path'):
                raise ValueError('Configure payout and escrow key first')
            private_read(Path(value))
            cert = read_certificate(Path(value))
            if not verify_certificate(obj['payout'], obj['pubkey'], cert, Path(obj['key_path']), DEFAULT_PROBE):
                raise ValueError('Wallet certificate rejected: payout/key/signature mismatch')
            dest = STATE / 'escrow.cert';store_text(dest, cert)
            obj.update(cert_path=str(dest), legacy_certificate=False)
        elif action == 'verify':
            if obj.get('legacy_certificate') and not obj.get('cert_path'):
                payout, pubkey, cert, _ = selected_certificate(DEFAULT_LAUNCHER, DEFAULT_KEY, DEFAULT_CERT, DEFAULT_PROBE)
                if payout != obj.get('payout') or pubkey != obj.get('pubkey'):
                    raise ValueError('Existing wallet authorization does not match setup')
                dest = STATE / 'escrow.cert';store_text(dest, cert);obj['cert_path'] = str(dest)
            if not all(obj.get(k) for k in ('payout', 'key_path', 'cert_path', 'pubkey')):
                raise ValueError('Complete payout, escrow key and wallet authorization first')
            protect_existing(obj['payout'], obj['pubkey'])
            private_read(Path(obj['key_path']));private_read(Path(obj['cert_path']))
            if derive_escrow_pubkey(obj['key_path']) != obj['pubkey']:
                raise ValueError('Escrow key/public identity mismatch')
            if not verify_certificate(obj['payout'], obj['pubkey'], read_certificate(Path(obj['cert_path'])), Path(obj['key_path']), DEFAULT_PROBE):
                raise ValueError('Official wallet authorization verification failed')
            if worker_identity():
                atomic_json(DRAFT, obj)
                return 'AUTHORIZED · verified draft saved; activate after mining stops'
            atomic_json(PROFILE, {'schema_version': 1, 'verified': True,
                                 **{k: obj[k] for k in ('payout', 'key_path', 'cert_path', 'pubkey')}})
            atomic_json(DRAFT, obj)
            return 'AUTHORIZED · saved for next launch; no worker started'
        else: raise ValueError('Unknown setup operation')
        atomic_json(DRAFT, obj)
        return 'Setup draft saved · press V to verify; active worker unchanged'
    finally:
        os.close(fd)
