"""Typed desktop commands. Identity, AI and process gates remain in existing controllers."""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
from nexus_agent_api import ROOT, SOURCE

ACTIONS = frozenset({'preferences', 'workload', 'gpu_select', 'payout', 'key',
    'new_key', 'certificate', 'verify', 'update_check', 'updates_toggle',
    'api_toggle', 'mcp_toggle', 'api_generate', 'copy_api_key', 'copy_public_key',
    'copy_endpoint', 'notifications_toggle', 'notification_status',
    'gpu_capabilities_refresh', 'gpu_mode', 'stop', 'restart', 'shutdown'})
CONFIRM = frozenset({'gpu_select', 'workload', 'payout', 'key', 'new_key', 'certificate',
    'verify', 'api_generate', 'stop', 'restart', 'shutdown'})


def production_source():
    """Dispatch through the active desktop's controller tree, not a candidate launcher."""
    try:
        result = subprocess.run(['systemctl', '--user', 'show', 'nexus-v002-production.service',
            '-p', 'ExecStart', '--value'], capture_output=True, text=True, timeout=2, check=True)
        match = re.search(r'argv\[\]=(.*?) ; ignore_errors=', result.stdout)
        if match:
            for argument in shlex.split(match.group(1)):
                path = Path(argument)
                if path.name == 'nexusmine-next.fish' and path.is_absolute():
                    path = path.resolve(strict=True)
                    info = path.stat()
                    if path.is_relative_to(ROOT.resolve()) and info.st_uid == os.getuid() and not info.st_mode & 0o022:
                        return path.parents[1]
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return SOURCE


def action(name, value, confirmed=False):
    if not isinstance(name, str) or name not in ACTIONS:
        raise ValueError('Unsupported desktop action')
    if name in CONFIRM and confirmed is not True:
        raise ValueError('Explicit confirmation is required')
    if name == 'preferences':
        from nexus_ui_preferences import load, save
        if not isinstance(value, dict) or not value or not set(value) <= {'gpu_view', 'background', 'text', 'font_size', 'glow'}:
            raise ValueError('Invalid presentation settings')
        save(dict(load(), **value))
        return {'message': 'Presentation preference saved; mining workload is unchanged'}
    if name in ('gpu_select', 'workload'):
        fields = {'device', 'enabled'} if name == 'gpu_select' else {'device', 'workload'}
        if (not isinstance(value, dict) or set(value) != fields or
                not isinstance(value.get('device'), str) or
                (name == 'gpu_select' and type(value['enabled']) is not bool) or
                (name == 'workload' and not isinstance(value['workload'], str))):
            raise ValueError('Invalid GPU configuration')
        source = production_source()
        environment = dict(os.environ, PYTHONPATH=str(source / 'scripts'))
        code = ('import json,sys\nfrom nexus_gpu_switch import request_change\n'
                'v=json.load(sys.stdin)\n'
                'try: print(request_change(v["action"],v["value"]))\n'
                'except ValueError as e: sys.exit(str(e))')
        result = subprocess.run(['/usr/bin/python3', '-B', '-c', code],
            input=json.dumps({'action': name, 'value': value}), cwd=source,
            env=environment, capture_output=True, text=True, timeout=45)
        if result.returncode:
            raise ValueError(result.stderr.strip()[:400] or 'GPU handoff refused; configuration unchanged')
        return {'message': result.stdout.strip()}
    if name in ('payout', 'key', 'new_key', 'certificate', 'verify'):
        from nexus_setup import configure
        if not isinstance(value, str) or len(value) > 1024:
            raise ValueError('Invalid setup value')
        return {'message': configure(name, value)}
    if name in ('stop', 'restart', 'shutdown'):
        from nexus_desktop_commands import controller_command
        subprocess.run(controller_command(name, ROOT, production_source()), check=True,
                       capture_output=True, timeout=5)
        return {'message': 'Miner ' + name + ' requested through the existing controller'}
    if name == 'update_check':
        from nexus_updates import check, status
        check(activate=False, inspect_only=True)
        return {'message': 'Version: ' + status().get('status', 'UNKNOWN')}
    if name == 'updates_toggle':
        from nexus_updates import toggle
        return {'message': 'Automatic updates: ' + ('ON' if toggle()['enabled'] else 'OFF')}
    if name in ('api_toggle', 'mcp_toggle'):
        import nexus_dashboard as dashboard
        model = dashboard.DashboardModel()
        dashboard.development_keypress(model, ROOT, ord('T' if name == 'api_toggle' else 'M'))
        return {'message': model.development_notice}
    if name == 'api_generate':
        from nexus_agent_api import ensure_agent_key
        return {'message': ensure_agent_key(rotate=True)}
    if name == 'copy_api_key':
        from nexus_mcp_control import client_token
        return {'message': 'API/MSP key copied to clipboard', 'clipboard': client_token()}
    if name == 'copy_endpoint':
        return {'message': 'MCP endpoint copied to clipboard', 'clipboard': 'http://127.0.0.1:3341/mcp'}
    if name == 'copy_public_key':
        from nexus_setup import draft
        key = draft().get('pubkey')
        if not isinstance(key, str) or not re.fullmatch('[0-9a-f]{64}', key):
            raise ValueError('Escrow public key unavailable')
        return {'message': 'Escrow public key copied to clipboard', 'clipboard': key}
    if name in ('notifications_toggle', 'notification_status'):
        from nexus_notifications import notifications_enabled, set_notifications_enabled
        enabled = notifications_enabled()
        if name == 'notifications_toggle':
            enabled = not enabled
            set_notifications_enabled(enabled)
        return {'message': 'Notifications: ' + ('ON' if enabled else 'OFF'), 'notifications_enabled': enabled}
    if name == 'gpu_capabilities_refresh':
        from nexus_gpu_devices import refresh_capabilities
        return {'message': refresh_capabilities()}
    if name == 'gpu_mode':
        from nexus_gpu_devices import configure_mode
        if value not in ('disabled', 'diagnostics'):
            raise ValueError('Invalid offline GPU mode')
        return {'message': configure_mode(value)}
    raise ValueError('Unsupported desktop action')
