#!/usr/bin/env python3
"""Bounded read-only desktop notifications from NEXUS worker event logs."""
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
STATE = ROOT / 'state/observability/notifications.json'
SETTINGS = ROOT / 'state/observability/notification-settings.json'


ICON = Path(__file__).resolve().parent / 'assets/nexus-tray.svg'
MESSAGES = {
    'SUBMIT_ACK': ('Block submitted!', 'Block submission acknowledged by the node!'),
    'AI_REQUEST_ASSIGNED': ('AI request received!', 'An assigned AI request is being handled with priority!'),
    'AI_RESPONSE_ACK': ('AI response submitted!', 'AI response submission acknowledged by the node!'),
    'UPDATE_AVAILABLE': ('Update available!', 'A new NEXUS version is available. Open SETUP to review the update!'),
}


def notifications_enabled():
    try:
        value = json.loads(SETTINGS.read_text())
        return value.get('enabled') is True
    except FileNotFoundError:
        return True
    except (OSError, ValueError, AttributeError):
        return False


def set_notifications_enabled(enabled):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = SETTINGS.with_suffix('.new')
    fd = os.open(temporary, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as file:
        json.dump({'schema': 1, 'enabled': bool(enabled)}, file)
        file.flush(); os.fsync(file.fileno())
    os.replace(temporary, SETTINGS)


def transitions(worker, state):
    event = worker.get('event')
    stage = None
    identity = None
    if event == 'LIVE_SUBMIT_RESULT' and worker.get('state') == 'RPC_ACKNOWLEDGED':
        stage, identity = 'SUBMIT_ACK', worker.get('hash')
    elif event == 'AI_REQUEST_ASSIGNED':
        stage, identity = event, worker.get('request_id')
    elif event == 'AI_RESPONSE_LIFECYCLE' and worker.get('state') in ('RPC_ACKNOWLEDGED', 'CHAIN_OBSERVED', 'FINALIZED'):
        stage, identity = 'AI_RESPONSE_ACK', worker.get('request_id')
    if stage is None or not isinstance(identity, str) or not re.fullmatch('[0-9a-fA-F]{64}', identity):
        return []
    token = stage + ':' + identity.lower()
    seen = state.setdefault('seen', [])
    if token in seen:
        return []
    seen.append(token)
    del seen[:-4096]
    return [(stage, identity.lower())]


def desktop_notification(stage):
    """Deliver only the allowed events; never include prompts, keys or addresses."""
    if stage not in MESSAGES:
        return False
    if not notifications_enabled():
        return None  # Muted events are consumed, never queued for replay.
    title, body = MESSAGES[stage]
    try:
        result = subprocess.run(['/usr/bin/notify-send', '--app-name=NEXUS',
                                 '--icon=' + str(ICON), '--urgency=normal',
                                 '--expire-time=8000', 'NEXUS · ' + title, body],
                                timeout=3, capture_output=True)
        return result.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def notify(stage, identity):
    delivered = desktop_notification(stage)
    print(json.dumps({'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      'stage': stage, 'event_id': identity, 'evidence': 'worker_reported',
                      'desktop_delivered': delivered is True, 'muted': delivered is None}), flush=True)
    return delivered is not False


def persist(state):
    STATE.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path = STATE.with_suffix('.new')
    fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(state, f)
    os.replace(path, STATE)


def main():
    try:
        state = json.loads(STATE.read_text())
        if not isinstance(state, dict):
            state = {}
    except (OSError, ValueError):
        state = {}
    attached = False
    print(json.dumps({'event': 'NOTIFICATIONS_READY', 'mode': 'read-only'}), flush=True)
    while True:
        try:
            paths = list((ROOT / 'logs').glob('nexus-ui-inference-*.jsonl'))
            if not paths:
                time.sleep(2)
                continue
            path = max(paths, key=lambda p: p.stat().st_mtime_ns)
            st = path.stat()
            identity = [st.st_dev, st.st_ino]
            if state.get('file') != str(path) or state.get('identity') != identity or st.st_size < state.get('offset', 0):
                # Initial attachment never replays old desktop notifications.
                state.update(file=str(path), identity=identity, offset=0 if attached else st.st_size,
                             targets=None, submission=None)
            attached = True
            with path.open('rb') as f:
                f.seek(state.get('offset', 0))
                for _ in range(2048):
                    pos = f.tell()
                    line = f.readline(1024 * 1024 + 1)
                    if not line or not line.endswith(b'\n'):
                        f.seek(pos)
                        break
                    if len(line) > 1024 * 1024:
                        raise ValueError('oversized event')
                    if not any(marker in line for marker in (b'LIVE_SUBMIT_RESULT', b'AI_REQUEST_ASSIGNED', b'AI_RESPONSE_LIFECYCLE')):
                        continue
                    try:
                        obj = json.loads(line)
                        worker = obj.get('worker', obj)
                        if not isinstance(worker, dict):
                            continue
                        for stage, block in transitions(worker, state):
                            if not notify(stage, block):
                                # Allow a later real event to retry after notification transport failure.
                                state['seen'].remove(stage + ':' + block)
                    except (ValueError, TypeError, AttributeError):
                        continue
                state['offset'] = f.tell()
            persist(state)
        except (OSError, ValueError) as e:
            print(json.dumps({'event': 'NOTIFICATION_READ_ERROR', 'type': type(e).__name__}), flush=True)
        time.sleep(2)


if __name__ == '__main__':
    main()
