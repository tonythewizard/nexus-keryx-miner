"""Whitelisted detached desktop controllers; no secrets or arbitrary shell commands."""
from pathlib import Path

_CONTROLLERS = {
    'restart_ui': ('nexus-ui-restart', None, []),
    'stop': ('nexus-controlled-stop', 'nexus_stop_workers.py', []),
    'shutdown': ('nexus-force-shutdown', 'nexus_stop_workers.py', ['--force']),
    'restart': ('nexus-force-restart', 'nexus_restart_workers.py', []),
}


def controller_command(action, root, source):
    try:
        unit, script, arguments = _CONTROLLERS[action]
    except KeyError:
        raise ValueError('Unsupported desktop controller') from None
    temporary = str(Path(root) / 'work/tmp')
    command = (['/usr/bin/systemctl', '--user', 'restart', 'nexus-matrix-dashboard.service']
               if script is None else ['/usr/bin/python3', '-B', str(Path(source) / 'scripts' / script)] + arguments)
    return ['systemd-run', '--user', '--collect', '--unit=' + unit,
            '--service-type=exec', '--property=Restart=no'] + [
                '--setenv=' + name + '=' + temporary for name in ('TMPDIR', 'TMP', 'TEMP')
            ] + command
