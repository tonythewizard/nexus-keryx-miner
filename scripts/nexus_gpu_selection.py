"""Persistent GPU selection for PoM and inference, with guarded worker handoff."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import nexus_gpu_devices as devices
CUDA_UUID = devices.CUDA_UUID

AMD_ID = os.environ.get('NEXUS_SECONDARY_GPU_ID', 'pci:0000:00:00.0')


def configuration():
    value = devices.private_json(devices.STATE / 'selection.json', {
        'schema_version': 1, 'nvidia_pom': True, 'amd_pom': False,
    })
    if (not isinstance(value, dict) or set(value) != {'schema_version', 'nvidia_pom', 'amd_pom'}
            or type(value['schema_version']) is not int or value['schema_version'] != 1
            or any(type(value[k]) is not bool for k in ('nvidia_pom', 'amd_pom'))):
        raise ValueError('Invalid GPU selection; startup refused')
    return value


def set_enabled(device_id, enabled):
    if type(enabled) is not bool or device_id not in (CUDA_UUID, AMD_ID):
        raise ValueError('Select an explicitly authorized GPU')
    if enabled and device_id == AMD_ID and not any(d['id'] == AMD_ID for d in devices.amd_devices()):
        raise ValueError('Authorized secondary AMD GPU is not present')
    devices.prepare_state()
    fd = os.open(devices.STATE / 'selection.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value = configuration()
        value['nvidia_pom' if device_id == CUDA_UUID else 'amd_pom'] = enabled
        from nexus_gpu_switch import prepare_switch, request_switch
        active = prepare_switch(value)
        previous = configuration()
        devices.atomic(devices.STATE / 'selection.json', value)
        if active:
            try:
                request_switch()
            except Exception:
                devices.atomic(devices.STATE / 'selection.json', previous)
                raise
    return ('GPU selection saved · controlled worker handoff requested; AI must be idle'
            if active else 'GPU selection saved · applies to PoM and inference at next start')


def ensure_runtime():
    """Publish private defaults once so the worker can watch the same file."""
    devices.prepare_state()
    fd = os.open(devices.STATE / 'selection.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value = configuration()
        if not os.path.lexists(devices.STATE / 'selection.json'):
            devices.atomic(devices.STATE / 'selection.json', value)


def application_status(value, key, runtime, fresh, ai_backend=None):
    requested = value.get(key, key == 'nvidia_pom')
    text = 'ON' if requested else 'OFF'
    if not fresh or not runtime:
        return text + ' · SAVED / NO LIVE ACK'
    backend = (ai_backend or {}).get('backend')
    ai_device = (ai_backend or {}).get('device_id')
    if ai_device is None:
        ai_device = {'cuda':CUDA_UUID,'vulkan':AMD_ID}.get(backend)
    if not requested and ai_device == (CUDA_UUID if key == 'nvidia_pom' else AMD_ID):
        return text + ' · AI HANDOFF / NOT YET EXCLUDED'
    if runtime.get('error'):
        return text + ' · ERROR / PoM PAUSED'
    lane = 'nvidia' if key == 'nvidia_pom' else 'amd'
    if requested and not runtime.get(lane + '_available'):
        return text + ' · RESTART REQUIRED'
    if not runtime.get('live_control'):
        return text + ' · RESTART REQUIRED'
    if runtime.get(lane + '_enabled') != requested or not runtime.get(lane + '_applied'):
        return text + ' · APPLYING'
    return text + ' · APPLIED'


def runtime_lines():
    value = configuration()
    if value['amd_pom'] and not any(d['id'] == AMD_ID and d['driver'] == 'amdgpu' for d in devices.amd_devices()):
        raise ValueError('Configured secondary AMD GPU is missing or unbound; no silent device fallback')
    return [str(int(value['nvidia_pom'])), str(int(value['amd_pom']))]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-lines', action='store_true')
    parser.add_argument('--ensure-runtime', action='store_true')
    parser.add_argument('--amd-available', action='store_true')
    args = parser.parse_args()
    try:
        if args.ensure_runtime: ensure_runtime()
        if args.amd_available:
            print(int(any(d['id'] == AMD_ID and d['driver'] == 'amdgpu' for d in devices.amd_devices())))
        else:
            print('\n'.join(runtime_lines()) if args.runtime_lines else json.dumps(configuration()))
    except (OSError, ValueError) as e:
        parser.exit(2, f'GPU_SELECTION=BLOCKED {e}\n')
