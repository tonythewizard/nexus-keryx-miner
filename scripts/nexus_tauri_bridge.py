#!/usr/bin/env python3
"""Private typed desktop stdio adapter. Never exposes a network listener."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def presentation(mirror, tab, log_filter, height=46):
    from nexus_web_mirror import snapshot_for_model
    from nexus_presenter import build_presenter_frame
    from nexus_ui_preferences import load, resolved_view, large_gpu_count, blend, contrast
    from nexus_gpu_registry import records, workload_options
    from nexus_notifications import notifications_enabled
    with mirror.lock:
        model, gpu = mirror.model.presentation_copy(), mirror.gpu
    preferences = load()
    frame = snapshot_for_model(model, gpu, tab, log_filter)
    frame['panels'] = build_presenter_frame(height, 160, model, gpu, tab, log_filter,
                                            preferences=preferences).panels
    bg, fg = preferences['background'], preferences['text']
    def readable(color):
        return color if contrast(color, bg) >= 4.5 else fg
    frame['desktop'] = {
        'ready': mirror.ready.is_set() if hasattr(mirror, 'ready') else True,
        'preferences': preferences,
        'effective_view': resolved_view(preferences, model, gpu),
        'large_gpu_count': large_gpu_count(model, gpu),
        'api_active': model.development.get('server_unit') == 'active',
        'mcp_active': model.development.get('mcp', {}).get('server_unit') == 'active',
        'notifications_enabled': notifications_enabled(),
        'mining_payout': model.mining_payout or model.setup.get('payout') or '',
        'key_exists': bool(model.setup.get('key_path') or model.wallet.get('escrow_public_key')),
        'devices': [dict(id=r['id'], name=r['name'], vendor=r['vendor'],
                         runtime_lane=r.get('runtime_lane'), options=workload_options(r)) for r in records(model.gpu_devices)],
        'colors': {'hero': fg, 'value': fg, 'title': fg,
                   'label': readable(blend(fg, bg, .78)),
                   'muted': readable(blend(fg, '#b5c6bb', .35)),
                   'text': readable(blend(fg, '#ffffff', .25)),
                   'warning': '#ffd166' if contrast('#ffd166', bg) >= 4.5 else '#805500',
                   'error': '#ff6868' if contrast('#ff6868', bg) >= 4.5 else '#b00020'},
    }
    from nexus_gpu_devices import STATE, private_json
    try:
        handoff = private_json(STATE / 'handoff.json', {})
        if handoff.get('request_id'):
            frame['desktop']['gpu_handoff'] = {
                'request_id': handoff['request_id'], 'status': handoff.get('status'),
                'reason': str(handoff.get('reason', ''))[:400]}
    except (OSError, ValueError, TypeError, AttributeError):
        pass  # Missing status never prevents access to controls.
    frame.pop('escrow_public_key', None)
    frame.pop('setup_key_exists', None)
    return frame


def main():
    protocol, sys.stdout = sys.stdout, sys.stderr
    from nexus_agent_api import ROOT
    if Path(os.environ['NEXUS_DATA_DIR']).resolve(strict=True) != ROOT.resolve(strict=True):
        raise ValueError('The transitional adapter requires the existing data root')
    from nexus_web_mirror import WebMirror
    mirror = WebMirror(poll_interval=.5)
    sequence, session = 0, uuid.uuid4().hex
    while True:
        line = sys.stdin.buffer.readline(8193)
        if not line:
            return
        if len(line) > 8192 or not line.endswith(b'\n'):
            raise ValueError('Oversized presentation request')
        request = json.loads(line)
        if not isinstance(request, dict) or type(request.get('schema_version')) is not int or request['schema_version'] != 1:
            raise ValueError('Unsupported presentation request')
        try:
            if request.get('op') == 'snapshot':
                if set(request) != {'schema_version', 'op', 'tab', 'filter', 'height'}:
                    raise ValueError('Invalid snapshot fields')
                if type(request['tab']) is not int or not isinstance(request['filter'], str) or type(request['height']) is not int or not 16 <= request['height'] <= 46:
                    raise ValueError('Invalid snapshot fields')
                frame = presentation(mirror, request['tab'], request['filter'], request['height'])
            elif request.get('op') == 'action':
                if set(request) != {'schema_version', 'op', 'action', 'value', 'confirmed'} or type(request['confirmed']) is not bool:
                    raise ValueError('Invalid action fields')
                from nexus_tauri_actions import action
                result = action(request['action'], request['value'], request['confirmed'])
                frame = {'schema_version': 1, 'result': result}
                mirror.refresh.set()
            else:
                raise ValueError('Unsupported desktop operation')
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            frame = {'schema_version': 1, 'error': str(error) if isinstance(error, ValueError) else 'Operation unavailable; existing configuration preserved'}
        sequence += 1
        frame['transport'] = {'schema_version': 1, 'session': session, 'sequence': sequence,
                              'monotonic_ns': time.monotonic_ns()}
        encoded = json.dumps(frame, separators=(',', ':'), allow_nan=False)
        if len(encoded.encode()) > 2 * 1024 * 1024:
            raise ValueError('Oversized presentation frame')
        protocol.write(encoded + '\n')
        protocol.flush()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError):
        print('NEXUS desktop adapter unavailable; mining is unaffected', file=sys.stderr)
        raise SystemExit(1)
