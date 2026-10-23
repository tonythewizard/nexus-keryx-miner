#!/usr/bin/env python3
"""Restore only the verified Tauri window on KDE Wayland; never controls workers."""
import os
import fcntl
from pathlib import Path
import subprocess
import sys


def main():
    pid = int(sys.argv[1])
    proc = Path('/proc', str(pid))
    if proc.stat().st_uid != os.getuid() or proc.joinpath('exe').resolve(strict=True).name != 'nexus-desktop':
        raise ValueError('Not this user\'s Tauri desktop')
    start = proc.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19]
    root = Path(os.environ['NEXUS_DATA_DIR']).resolve(strict=True)
    target = root / 'state/desktop'
    if target.is_symlink():
        raise ValueError('Invalid desktop state path')
    target.mkdir(mode=0o700, parents=True, exist_ok=True)
    lock = os.open(target / 'tauri-window.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(lock)
        return  # A previous tray activation is already restoring this window.
    script = target / ('tauri-restore-' + str(pid) + '.js')
    fd = os.open(script, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as out:
        out.write('for (const w of workspace.windowList()) { if (w.pid === ' + str(pid) +
                  ') { w.minimized=false; w.skipTaskbar=false; workspace.activeWindow=w; }}\n')
    name = 'nexus-tauri-restore-' + str(pid)
    def call(*args):
        return subprocess.run(['qdbus6', 'org.kde.KWin', *args], capture_output=True,
                              text=True, timeout=3, check=True).stdout.strip()
    try:
        call('/Scripting', 'org.kde.kwin.Scripting.unloadScript', name)
        if proc.joinpath('stat').read_text().rsplit(')', 1)[1].split()[19] != start:
            raise ValueError('Desktop identity changed')
        identity = call('/Scripting', 'org.kde.kwin.Scripting.loadScript', str(script), name)
        if not identity.isdecimal():
            raise ValueError('Window restoration unavailable')
        call('/Scripting/Script' + identity, 'org.kde.kwin.Script.run')
    finally:
        try: call('/Scripting', 'org.kde.kwin.Scripting.unloadScript', name)
        finally: script.unlink(missing_ok=True)


if __name__ == '__main__':
    try: main()
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        raise SystemExit(1)
