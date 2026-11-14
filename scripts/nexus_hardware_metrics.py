"""Read-only Windows GPU inventory. Unsupported sensors remain unavailable.

Identity is the NVIDIA UUID or Windows PNP device ID, never display order.
No Vulkan device is opened and no compute capability is implied by discovery.
"""
import csv
import json
import math
import os
import subprocess
import time


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) and result >= 0 else None
    except (ValueError, TypeError):
        return None


def run(command):
    options = {'creationflags': 0x08000000} if os.name == 'nt' else {}
    return subprocess.run(command, capture_output=True, text=True, check=True, timeout=3, **options).stdout


def inventory():
    observed = time.time()
    devices = []
    try:
        out = run(['nvidia-smi', '--query-gpu=uuid,name,memory.total,memory.used,utilization.gpu,temperature.gpu,power.draw,clocks.gr',
                   '--format=csv,noheader,nounits'])
        for row in csv.reader(out.splitlines()):
            if len(row) != 8:
                continue
            uuid, name, total, used, util, temp, power, clock = [x.strip() for x in row]
            if not uuid.startswith('GPU-'):
                continue
            metrics = dict(zip(('vram_total_bytes', 'vram_used_bytes', 'gpu_busy_percent', 'temperature_c', 'power_w', 'graphics_clock_mhz'),
                               [number(x) for x in (total, used, util, temp, power, clock)]))
            for key in ('vram_total_bytes', 'vram_used_bytes'):
                if metrics[key] is not None:
                    metrics[key] *= 2**20
            devices.append({'id': uuid, 'name': name, 'vendor': 'nvidia', 'runtime_lane': None,
                            'vram_bytes': metrics['vram_total_bytes'], 'metrics': metrics, 'observed_unix': observed})
    except (OSError, subprocess.SubprocessError):
        pass
    if os.name == 'nt':
        try:
            out = run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                       'Get-CimInstance Win32_VideoController | Select-Object Name,PNPDeviceID | ConvertTo-Json -Compress'])
            rows = json.loads(out)
            if isinstance(rows, dict):
                rows = [rows]
            for row in rows or []:
                ident, name = row.get('PNPDeviceID', ''), row.get('Name', '')
                if not isinstance(ident, str) or not isinstance(name, str):
                    continue
                if 'VEN_1002' not in ident.upper():
                    continue
                devices.append({'id': ident, 'name': name, 'vendor': 'amd', 'runtime_lane': None,
                                'vram_bytes': None, 'metrics': {}, 'observed_unix': observed,
                                'driver': 'Windows display driver', 'mode': 'diagnostics', 'capabilities_status': 'NOT QUALIFIED',
                                'metrics_status': 'UNAVAILABLE', 'metrics_reason': 'AMD Windows hardware sensor provider not installed'})
        except (OSError, ValueError, AttributeError, subprocess.SubprocessError):
            pass
    return {'devices': devices, 'amd_devices': [d for d in devices if d['vendor'] == 'amd'],
            'selection': {}, 'observed_unix': observed}
