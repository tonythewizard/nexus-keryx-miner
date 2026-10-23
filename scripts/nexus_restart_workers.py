#!/usr/bin/env python3
"""Explicit tray force restart, preserving the running worker's systemd launcher."""
import fcntl
import hashlib
import json
import os
import re
import shlex
import stat
from pathlib import Path
import subprocess
import time
import uuid

from nexus_agent_api import ROOT, worker_identity
from nexus_updates import STATE as UPDATE_STATE, atomic

SOURCE = Path(__file__).resolve().parent.parent
STATE = ROOT / 'state/restart'
HELPERS = ('nexus-matrix-dashboard.service',
           'nexus-first-block-alerts.service', 'nexus-agent-api.service', 'nexus-mcp.service')


def run(args, timeout=10, env=None):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=True, env=env).stdout.strip()


def native_workers():
    return [w for w in worker_identity() if w['kind'] == 'nexus-live-cuda-worker']


def digest(pid):
    with Path('/proc', str(pid), 'exe').open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def status(phase, **fields):
    """Keep the restart identity and last completed phase on failure."""
    path = STATE / 'status.json'
    try:
        previous = json.loads(path.read_text())
    except (OSError, ValueError):
        previous = {}
    atomic(path, {**previous, **fields, 'status': phase, 'updated_unix': time.time()})


def properties(unit):
    text = run(['systemctl', '--user', 'show', unit, '-p', 'LoadState',
                '-p', 'ActiveState', '-p', 'Job'])
    return dict(row.split('=', 1) for row in text.splitlines() if '=' in row)


def reset_if_failed(unit):
    # ResetFailedUnit requires a loaded unit. An optional service can be
    # garbage-collected after stop even though its unit file still exists.
    state = properties(unit)
    if state.get('LoadState') != 'loaded':
        raise ValueError('Restart service definition is unavailable')
    if state.get('ActiveState') == 'failed':
        run(['systemctl', '--user', 'reset-failed', unit])


def stopped_service_identity(unit):
    """Recover only the configured production launcher and its verified pin."""
    state=properties(unit)
    if state.get('LoadState')!='loaded' or state.get('ActiveState') not in ('inactive','failed'):
        raise ValueError('Production service is not safely stopped')
    command=run(['systemctl','--user','show',unit,'-p','ExecStart','--value'])
    match=re.search(r'argv\[\]=(.*?) ; ignore_errors=',command)
    args=shlex.split(match.group(1)) if match else []
    candidates=[Path(value) for value in args if value.endswith('/scripts/nexusmine-next.fish')]
    if len(candidates)!=1 or candidates[0].resolve() not in {
            (SOURCE/'scripts/nexusmine-next.fish').resolve(),
            (ROOT/'source/scripts/nexusmine-next.fish').resolve()}:
        raise ValueError('Stopped service has no recognized pinned NEXUS launcher')
    launcher=candidates[0]
    info=launcher.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_mode&0o022:
        raise ValueError('Unsafe stopped-service launcher')
    environment=shlex.split(run(['systemctl','--user','show',unit,'-p','Environment','--value']))
    if run(['systemctl','--user','show',unit,'-p','EnvironmentFiles','--value']):
        raise ValueError('Stopped-service environment file needs explicit identity validation')
    releases=[entry.split('=',1)[1] for entry in environment if entry.startswith('NEXUS_RELEASE_HOME=')]
    if releases:
        lines=run(['/usr/bin/python3','-B',str(launcher.parent/'nexus_updates.py'),
                   'runtime-paths',releases[-1]]).splitlines()
        if len(lines)!=4:
            raise ValueError('Installed release identity is unavailable')
        binary,expected=Path(lines[0]),lines[1]
    else:
        text=launcher.read_text()
        path=re.search(r'^set -l worker (.+)$',text,re.M)
        pin=re.search(r'^set -l expected_sha256 ([0-9a-f]{64})$',text,re.M)
        if not path or not pin:
            raise ValueError('Stopped-service worker pin is unavailable')
        values=shlex.split(path.group(1))
        if len(values)!=1:
            raise ValueError('Stopped-service worker path is ambiguous')
        value=values[0]
        if value.startswith('$recovery/'):
            root=re.search(r'^set -l recovery (.+)$',text,re.M)
            roots=shlex.split(root.group(1)) if root else []
            if len(roots)!=1 or Path(roots[0]).resolve()!=ROOT.resolve():
                raise ValueError('Launcher recovery root does not match the installation')
            value=str(ROOT/value[len('$recovery/'):])
        binary,expected=Path(value),pin.group(1)
    if not binary.is_absolute() or not binary.resolve().is_relative_to(ROOT.resolve()):
        raise ValueError('Stopped-service worker is outside the installation')
    if not re.fullmatch('[0-9a-f]{64}',expected):
        raise ValueError('Invalid stopped-service worker pin')
    with binary.open('rb') as file:
        if hashlib.file_digest(file,'sha256').hexdigest()!=expected:
            raise ValueError('Stopped-service worker hash mismatch')
    if native_workers():
        raise ValueError('A worker appeared during stopped-service recovery')
    return expected


def marker_bytes(marker):
    try:return marker.read_bytes()
    except FileNotFoundError:return None


def restart():
    initial_stop=marker_bytes(UPDATE_STATE/'user-stop.json')
    workers = native_workers()
    if len(workers)>1:
        raise ValueError('Multiple NEXUS workers; restart refused')
    worker=workers[0] if workers else None
    if worker:
        units={part for row in Path('/proc',str(worker['pid']),'cgroup').read_text().splitlines()
               for part in row.split(':',2)[-1].split('/')
               if part.startswith('nexus-') and part.endswith('.service')}
        if len(units)!=1:
            raise ValueError('Worker service identity is ambiguous; restart refused')
        unit=units.pop()
    else:
        unit='nexus-v002-production.service'
    fragment = Path(run(['systemctl', '--user', 'show', unit, '-p', 'FragmentPath', '--value']))
    if not fragment.is_absolute() or fragment.stat().st_uid != os.getuid() or fragment.stat().st_size > 65536:
        raise ValueError('Unsafe worker service fragment')
    contents = fragment.read_text()
    expected = digest(worker['pid']) if worker else stopped_service_identity(unit)
    if native_workers() != workers:
        raise ValueError('Worker changed before restart preparation')
    target = Path.home() / '.config/systemd/user' / unit
    transient = '/systemd/transient/' in str(fragment)
    if transient and target.exists():
        raise ValueError('A different persistent service already uses this name')
    active_helpers = [name for name in HELPERS if subprocess.run(
        ['systemctl', '--user', 'is-active', '--quiet', name], timeout=3).returncode == 0]
    snapshot = STATE / (unit + '.snapshot')
    fd = os.open(snapshot, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as file:
        file.write(contents); file.flush(); os.fsync(file.fileno())
    status('STOPPING' if worker else 'RECOVERING', unit=unit, previous_pid=worker['pid'] if worker else None,
           previous_starttime=worker['starttime'] if worker else None, worker_sha256=expected,
           restore_helpers=active_helpers)
    marker = UPDATE_STATE / 'user-stop.json'
    if worker:
        # Detached controller survives the old tray/desktop being stopped.
        stop_id=uuid.uuid4().hex
        run(['/usr/bin/python3','-B',str(SOURCE/'scripts/nexus_stop_workers.py'),'--force'],timeout=30,
            env={**os.environ,'NEXUS_STOP_REQUEST_ID':stop_id})
        owned_stop=marker_bytes(marker)
        if owned_stop is None or json.loads(owned_stop).get('request_id')!=stop_id:
            raise ValueError('A newer manual shutdown takes priority over restart')
    else:
        # Explicit tray restart may recover a stopped unit; do not kill its UI.
        owned_stop=initial_stop
    if native_workers():
        raise ValueError('Old worker still exists; second worker will not be started')
    status('STOPPED')
    # A collected transient unit disappears after stop. Preserve its exact configuration
    # under the same name, without enabling it at boot or changing its launch arguments.
    if transient:
        target.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as file:
            file.write(contents); file.flush(); os.fsync(file.fileno())
        run(['systemctl', '--user', 'daemon-reload'])
    if marker_bytes(marker) != owned_stop:
        raise ValueError('A newer manual shutdown takes priority over restart')
    status('PREPARING_START')
    # Finish the asynchronous worker stop before requesting its next start.
    deadline = time.monotonic() + 15
    while properties(unit).get('ActiveState') == 'deactivating':
        if time.monotonic() >= deadline:
            raise ValueError('Previous service stop did not finish; no second worker started')
        time.sleep(.1)
    reset_if_failed(unit)
    if marker_bytes(marker) != owned_stop:
        raise ValueError('A newer manual shutdown takes priority over restart')
    if owned_stop is not None:
        marker.unlink()
    status('STARTING')
    if marker.exists():
        raise ValueError('A newer manual shutdown takes priority over restart')
    if native_workers():
        raise ValueError('A worker appeared before service start; restart refused')
    run(['systemctl', '--user', 'start', unit])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        current = native_workers()
        if current:
            if len(current) != 1 or digest(current[0]['pid']) != expected:
                run(['/usr/bin/python3', '-B', str(SOURCE / 'scripts/nexus_stop_workers.py'), '--force'], timeout=30)
                raise ValueError('Restarted worker identity mismatch; stopped without a retry loop')
            if marker.exists():
                raise ValueError('Manual shutdown requested during restart')
            helper_errors = []
            for helper in active_helpers:
                try:
                    reset_if_failed(helper)
                    run(['systemctl', '--user', 'start', helper])
                except (OSError, ValueError, subprocess.SubprocessError):
                    helper_errors.append(helper)
            status('START_REQUESTED', pid=current[0]['pid'], starttime=current[0]['starttime'],
                   worker_sha256=expected, readiness='CHECK_LIVE_TELEMETRY',
                   helper_errors=helper_errors)
            print('NEXUS_FORCE_RESTART=START_REQUESTED; inspect live readiness', flush=True)
            return
        if properties(unit).get('ActiveState') not in ('active', 'activating', 'reloading'):
            raise ValueError('Worker startup failed; no automatic retry')
        time.sleep(.5)
    raise ValueError('Worker startup deadline exceeded; inspect the service, no automatic retry')


def main():
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(STATE / 'restart.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        print('NEXUS_FORCE_RESTART=ALREADY_RUNNING', flush=True)
        return 0
    try:
        atomic(STATE / 'status.json', {'status': 'PREPARING', 'updated_unix': time.time()})
        restart()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        # Do not expose launch arguments, credentials or private file contents.
        try:
            phase = json.loads((STATE / 'status.json').read_text()).get('status', 'PREPARING')
        except (OSError, ValueError):
            phase = 'PREPARING'
        operation = None
        if isinstance(error, subprocess.CalledProcessError) and isinstance(error.cmd, (list, tuple)):
            # Only command/verb, never launch arguments, payout or certificates.
            operation = ' '.join(str(x) for x in error.cmd[:3])
        status('FAILED', failed_phase=phase, error_type=type(error).__name__,
               operation=operation, exit_code=getattr(error, 'returncode', None),
               detail=str(error) if isinstance(error, ValueError) else 'Service operation failed; inspect logs')
        print('NEXUS_FORCE_RESTART=FAILED; inspect state/restart/status.json and service logs', flush=True)
        return 1
    finally:
        os.close(fd)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
