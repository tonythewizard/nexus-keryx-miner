#!/usr/bin/env python3
"""Explicitly stop this user's NEXUS miners, with bounded forced termination."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

from nexus_agent_api import ROOT

WORKERS = {'nexus-live-cuda-worker'}


def workers():
    result = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        fd = None
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            # Pin identity before inspecting it: signals cannot hit a reused PID.
            fd = os.pidfd_open(int(entry.name))
            executable = os.readlink(entry / 'exe').removesuffix(' (deleted)')
            command = (entry / 'cmdline').read_bytes().rstrip(b'\0').split(b'\0')
            owned_script = any(arg.decode(errors='replace') in {str(Path(__file__).resolve().parent / name) for name in ('nexus_tray.py', 'nexus_dashboard.py', 'nexus_desktop.py', 'nexus_agent_api.py', 'nexus_mcp.py', 'nexus_mcp_proxy.py', 'nexus_block_alerts.py', 'nexus_notifications.py')} for arg in command)
            owned_desktop = Path(executable).name == 'nexus-desktop' and Path(executable).is_relative_to(ROOT.resolve())
            if Path(executable).name not in WORKERS and not owned_script and not owned_desktop:
                os.close(fd)
                continue
            units = set()
            for row in (entry / 'cgroup').read_text().splitlines():
                for part in row.split(':', 2)[-1].split('/'):
                    if part.startswith('nexus-') and part.endswith('.service'):
                        units.add(part)
            result.append((int(entry.name), fd, units))
        except (OSError, ValueError):
            if fd is not None:
                os.close(fd)
    return result


def exited(fd):
    return bool(select.select([fd], [], [], 0)[0])


def main():
    force = sys.argv[1:] == ['--force']
    if len(sys.argv) != 1 and not force:
        print('Usage: stopnexus [--force]', file=sys.stderr)
        return 2
    # A manual stop wins over any pending automatic activation/rollback.
    from nexus_updates import STATE, atomic
    stop_record = {'requested_unix': time.time()}
    request_id = os.environ.get('NEXUS_STOP_REQUEST_ID', '')
    if len(request_id) == 32 and all(c in '0123456789abcdef' for c in request_id):
        stop_record['request_id'] = request_id
    atomic(STATE / 'user-stop.json', stop_record)
    targets = workers()
    units = {'nexus-tauri-preview.service', 'nexus-v002-production.service', 'nexus-update-activation.service', 'nexus-tray.service', 'nexus-matrix-dashboard.service', 'nexus-matrix-explorer-readonly.service', 'nexus-agent-api.service', 'nexus-mcp.service', 'nexus-first-block-alerts.service'}
    units.update(unit for _, _, owned in targets for unit in owned)
    # Ignore absent optional monitors instead of making a successful stop look
    # failed. Keep identity-pinned process termination as the independent fallback.
    try:
        reply = subprocess.run(['systemctl','--user','show',*sorted(units),'-p','Id','-p','LoadState'],
                               capture_output=True,text=True,timeout=3,check=True)
        loaded=set()
        for section in reply.stdout.strip().split('\n\n'):
            props=dict(line.split('=',1) for line in section.splitlines() if '=' in line)
            if props.get('LoadState')=='loaded':loaded.add(props['Id'])
        units=loaded
    except (OSError, subprocess.SubprocessError): pass
    # A manual service stop cancels Restart= before terminating the worker.
    try:
        r = subprocess.run(['systemctl', '--user', 'stop', '--no-block', *sorted(units)],
                           capture_output=True, text=True, timeout=3 if force else 10)
        error = r.stderr.strip() if r.returncode else ''
    except (OSError, subprocess.TimeoutExpired) as e:
        error = str(e)
    if error:
        print(f'Service stop failed: {error}', file=sys.stderr)
        if not force:
            for _, fd, _ in targets:
                os.close(fd)
            return 1
    try:
        for pid, fd, _ in targets:
            if not exited(fd):
                try:
                    signal.pidfd_send_signal(fd, signal.SIGKILL if force else signal.SIGTERM)
                    print(f'NEXUS PID {pid}: {"forced termination" if force else "cooperative stop requested"}')
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + (0 if force else 10)
        while time.monotonic() < deadline and any(not exited(fd) for _, fd, _ in targets):
            time.sleep(0.2)
        if not force:
            for pid, fd, _ in targets:
                if not exited(fd):
                    try:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                        print(f'NEXUS PID {pid}: forced termination')
                    except ProcessLookupError:
                        pass
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and any(not exited(fd) for _, fd, _ in targets):
            time.sleep(0.2)
    finally:
        for _, fd, _ in targets:
            os.close(fd)
    remaining = workers()
    for pid, fd, _ in remaining:
        print(f'NEXUS PID {pid}: still active', file=sys.stderr)
        os.close(fd)
    if remaining:
        return 1
    print('All NEXUS mining workers stopped.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
