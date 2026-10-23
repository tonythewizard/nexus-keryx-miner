"""SDK-free MCP settings and private shared-key access for the miner UI."""
import hashlib
import hmac
import json
import os
from pathlib import Path
import stat
import subprocess
from nexus_agent_api import ROOT, STATE, atomic_json

MCP_STATE = ROOT / 'mcp-diagnostics'
ENDPOINT = 'http://127.0.0.1:3341/mcp'
UNIT = 'nexus-mcp.service'


def private_json(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_size > 65536:
            raise ValueError('unsafe credential/settings file')
        return json.load(stream)


def authorized(token, scope='diagnostics'):
    if not isinstance(token, str) or not token or len(token) > 256:
        return False
    try:
        digest = hashlib.sha256(token.encode()).hexdigest()
        return any(k.get('active') and scope in k.get('scopes', []) and
                   hmac.compare_digest(k.get('sha256', ''), digest)
                   for k in private_json(STATE / 'keys-hashed.json'))
    except (OSError, ValueError, TypeError, AttributeError):
        return False


def client_token():
    token = private_json(STATE / 'client-keys.json').get('agent')
    if not authorized(token):
        raise ValueError('Generate an active API key with G first')
    return token


def enabled():
    try:
        value = private_json(MCP_STATE / 'config.json')['enabled']
        return value is True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def toggle():
    state = subprocess.run(['systemctl', '--user', 'is-active', UNIT], capture_output=True, text=True, timeout=2).stdout.strip()
    value = state != 'active'
    if value:
        client_token()
    subprocess.run(['systemctl', '--user', 'start' if value else 'stop', UNIT], check=True, capture_output=True, timeout=10)
    atomic_json(MCP_STATE / 'config.json', {'enabled': value})
    return value


def status():
    unit = subprocess.run(['systemctl', '--user', 'is-active', UNIT], capture_output=True, text=True, timeout=1).stdout.strip()
    try:
        result = private_json(MCP_STATE / 'status.json')
    except (OSError, ValueError):
        result = {}
    return {**result, 'server_unit': unit, 'enabled': enabled(), 'endpoint': ENDPOINT}


if __name__ == '__main__':
    if enabled():
        client_token()
        subprocess.run(['systemctl', '--user', 'start', UNIT], check=True)
