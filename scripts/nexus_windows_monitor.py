"""Windows read-only desktop adapter using the shared telemetry model and panels.

No Linux service/controller imports or wallet reads; no mining is started by UI.
"""
import ctypes
import os
from pathlib import Path
import time


def memory_status():
    class Memory(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [
            (name, ctypes.c_uint64) for name in ('total', 'available', 'page_total',
                'page_available', 'virtual_total', 'virtual_available', 'extended')]
    value = Memory()
    value.length = ctypes.sizeof(value)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(value)):
        return {}
    return {'MemTotal': value.total / 2**30, 'MemAvailable': value.available / 2**30}


class WindowsMonitor:
    def __init__(self, root):
        from nexus_dashboard import TelemetrySessionReader
        self.root = Path(root)
        self.reader = TelemetrySessionReader(root, wallet_status=False)
        self.inventory = {'devices': [], 'amd_devices': [], 'selection': {}}
        self.observed = 0

    def snapshot(self, tab, log_filter, height, before=None):
        from nexus_web_mirror import snapshot_for_model
        from nexus_ui_preferences import load, resolved_view, large_gpu_count, blend, contrast
        from nexus_gpu_registry import records, workload_options
        model = self.reader.poll()
        if time.monotonic() - self.observed >= 2:
            from nexus_hardware_metrics import inventory
            self.inventory = inventory()
            self.observed = time.monotonic()
        gpu = 'UNKNOWN'
        backends = (model.native or {}).get('gpu_backends', {})
        matched = []
        for device in self.inventory['devices']:
            device['runtime_lane'] = None
            if device['vendor'] == 'nvidia' and backends.get(device['id']) == 'cuda-driver':
                matched.append(device)
        if len(matched) == 1:
            device = matched[0]
            device['runtime_lane'] = 'rtx'
            metrics = device['metrics']
            values = [metrics.get(key) for key in ('gpu_busy_percent', 'vram_used_bytes', 'vram_total_bytes',
                                                   'temperature_c', 'power_w', 'graphics_clock_mhz')]
            if all(value is not None for value in values):
                values[1] /= 2**20
                values[2] /= 2**20
                gpu = ','.join(str(value) for value in values)
        model.gpu_devices = self.inventory
        preferences = load(self.root / 'state/desktop/presentation.json')
        # Shared native snapshots retain counters, proof/AI status and stale handling.
        frame = snapshot_for_model(model, gpu, tab, log_filter,
                                   height=height, preferences=preferences, log_before=before)
        bg, fg = preferences['background'], preferences['text']
        def readable(color):
            return color if contrast(color, bg) >= 4.5 else fg
        frame['desktop'] = {
            'ready': True, 'read_only': True,
            'control_reason': 'Windows monitoring preview; worker controls are not qualified yet',
            'preferences': preferences, 'effective_view': resolved_view(preferences, model, gpu),
            'large_gpu_count': large_gpu_count(model, gpu),
            'api_active': False, 'mcp_active': False, 'notifications_enabled': False,
            'is_paused': (model.native or {}).get('state', {}).get('mining') == 'PAUSED',
            'mining_payout': model.mining_payout or '', 'mining_mode': {'mode': model.stratum.get('mode', 'solo'), 'pools': []},
            'key_exists': False,
            'devices': [dict(r, options=workload_options(r)) for r in records(model.gpu_devices)],
            'colors': {'hero': fg, 'value': fg, 'title': fg,
                       'label': readable(blend(fg, bg, .78)), 'muted': readable(blend(fg, '#b5c6bb', .35)),
                       'text': readable(blend(fg, '#ffffff', .25)), 'warning': '#ffd166', 'error': '#ff6868'},
        }
        frame.pop('escrow_public_key', None)
        frame.pop('setup_key_exists', None)
        return frame

    def action(self, name, value, confirmed):
        if name != 'preferences':
            raise ValueError('Windows worker controls are not qualified; monitoring only')
        from nexus_ui_preferences import load, save
        if not isinstance(value, dict) or not value or not set(value) <= {'gpu_view', 'background', 'text', 'font_size', 'glow'}:
            raise ValueError('Invalid presentation settings')
        path = self.root / 'state/desktop/presentation.json'
        save(dict(load(path), **value), path)
        return {'message': 'Presentation preferences saved'}
