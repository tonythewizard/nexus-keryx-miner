#!/usr/bin/env python3
"""Persistent GPU inventory and offline-only AMD admission; never starts mining.

Discovery reads AMD sysfs entries only. NVIDIA's existing authorized UUID is
configuration, not newly probed hardware. Capability refresh explicitly runs
the AMD-only read-only native tool; ordinary UI polling never initializes it.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess
import time

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
STATE = ROOT / 'state/gpu-devices'
CUDA_UUID = os.environ.get("NEXUS_PRIMARY_GPU_UUID", 'GPU-00000000-0000-0000-0000-000000000000')
PROBE = ROOT / 'vulkan-jit/build/bin/nexus-vulkan-device-info'
MODES = ('disabled', 'diagnostics')


def lease_path(device_id):
    if not re.fullmatch(r'pci:[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', device_id):
        raise ValueError('Invalid explicit PCI device identity')
    return STATE / ('lease-' + device_id.replace(':', '_').replace('.', '_') + '.lock')


def lease_busy(device_id):
    try:
        fd = os.open(lease_path(device_id), os.O_RDWR | os.O_NOFOLLOW)
    except FileNotFoundError:
        return False
    try:
        try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return True
        return False
    finally:
        os.close(fd)


def private_json(path, default=None):
    resolved = STATE.resolve()
    if any(resolved == p or p in resolved.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
        raise ValueError('GPU state must use persistent storage')
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError:
        return default
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_size > 16384:
            raise ValueError('Invalid private GPU registry file')
        return json.load(stream)


def prepare_state():
    if STATE.is_symlink() or not STATE.resolve().is_relative_to((ROOT / 'state').resolve()):
        raise ValueError('GPU state must remain in persistent local storage')
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)


def atomic(path, value):
    prepare_state()
    pending = STATE / ('pending-' + secrets.token_hex(12))
    try:
        fd = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(pending, path)
    finally:
        pending.unlink(missing_ok=True)


def configuration():
    value = private_json(STATE / 'config.json', {'schema_version': 1, 'devices': {}})
    if not isinstance(value, dict) or value.get('schema_version') != 1 or set(value) != {'schema_version', 'devices'} or not isinstance(value['devices'], dict):
        raise ValueError('Invalid GPU configuration; no device admitted')
    for ident, item in value['devices'].items():
        if not re.fullmatch(r'pci:[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', ident) or not isinstance(item, dict) or set(item) != {'mode', 'backend'} or item['mode'] not in MODES or item['backend'] != 'vulkan-jit':
            raise ValueError('Unsupported GPU assignment; production AMD admission is unavailable')
    return value


def amd_devices(sysfs=Path('/sys/bus/pci/devices')):
    result = []
    for device in sorted(sysfs.iterdir()):
        # Never open NVIDIA metric files or load its Vulkan/CUDA driver here.
        if (device / 'vendor').read_text().strip() != '0x1002':
            continue
        # A GPU's HDMI audio function shares its vendor. Never offer it a workload.
        try:
            if int((device / 'class').read_text().strip(), 16) >> 16 != 0x03:
                continue
        except (OSError, ValueError):
            continue
        # Read-only AMD inventory is model-independent. Compute admission remains separate.
        if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', device.name):
            continue
        total = device / 'mem_info_vram_total'
        metrics = {}
        for key, path, scale in [('utilization_pct', device / 'gpu_busy_percent', 1),
                                 ('vram_used_bytes', device / 'mem_info_vram_used', 1)]:
            try: metrics[key] = int(path.read_text()) / scale
            except (OSError, ValueError): metrics[key] = None
        for key, filename, scale in [('temperature_c', 'temp1_input', 1000), ('power_w', 'power1_average', 1000000)]:
            paths = sorted((device / 'hwmon').glob('hwmon*/' + filename))
            try: metrics[key] = int(paths[0].read_text()) / scale if paths else None
            except (OSError, ValueError): metrics[key] = None
        if metrics.get('power_w') is None:
            # Polaris can expose instantaneous PPT rather than averaged power.
            paths = sorted((device / 'hwmon').glob('hwmon*/power1_input'))
            try:
                metrics['power_w'] = int(paths[0].read_text()) / 1000000 if paths else None
                metrics['power_source'] = 'power1_input / ' + (paths[0].with_name('power1_label').read_text().strip() if paths and paths[0].with_name('power1_label').exists() else 'reported sensor')
            except (OSError, ValueError): metrics['power_w'] = None
        try:
            active = next(line for line in (device / 'pp_dpm_sclk').read_text().splitlines() if line.rstrip().endswith('*'))
            metrics['graphics_clock_mhz'] = int(re.search(r'(\d+)Mhz', active, re.I).group(1))
        except (OSError, ValueError, StopIteration, AttributeError): metrics['graphics_clock_mhz'] = None
        from nexus_gpu_identity import amd_name
        result.append({'id': 'pci:' + device.name, 'name': amd_name(device), 'vendor': 'AMD', 'metrics': metrics,
                       'backend': 'vulkan-jit', 'driver': (device / 'driver').resolve().name if (device / 'driver').exists() else 'UNBOUND',
                       'vram_bytes': int(total.read_text()) if total.exists() else None,
                       'presence': 'PRESENT', 'production_ready': False,
                       'production_blockers': ['RESIDENT_SEARCH_NOT_TRANSLATED', 'INFERENCE_NOT_VALIDATED', 'PRODUCTION_SCHEDULING_NOT_INTEGRATED']})
    return result


def status():
    config = configuration()
    devices = amd_devices()
    observed = private_json(STATE / 'capabilities.json', {})
    if not isinstance(observed, dict) or not isinstance(observed.get('devices', []), list) or any(not isinstance(d, dict) or not isinstance(d.get('id'), str) for d in observed.get('devices', [])):
        raise ValueError('Invalid cached AMD capabilities')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    timestamp = observed.get('observed_unix', 0)
    if not isinstance(timestamp, (int, float)):
        raise ValueError('Invalid cached AMD observation time')
    age = time.time() - timestamp
    fresh = isinstance(observed, dict) and observed.get('boot_id') == boot and 0 <= age <= 300
    capabilities = {d['id']: d for d in observed.get('devices', [])} if fresh else {}
    for device in devices:
        device['mode'] = config['devices'].get(device['id'], {}).get('mode', 'disabled')
        device['capabilities_status'] = 'LIVE_READ_ONLY' if device['id'] in capabilities else 'REFRESH_REQUIRED'
        if device['id'] in capabilities:
            value = capabilities[device['id']]
            device['capabilities'] = {key: value.get(key) for key in ('shaderInt64', 'shaderInt8', 'storageBuffer8BitAccess', 'max_workgroup_invocations', 'shared_memory_bytes')}
        device['workload'] = 'NOT_SCHEDULED'
        device['offline_lease'] = 'BUSY' if lease_busy(device['id']) else 'IDLE'
    configured_ids = {d['id'] for d in devices}
    missing = [ident for ident in config['devices'] if ident not in configured_ids]
    from nexus_gpu_selection import configuration as selection
    from nexus_gpu_identity import nvidia_name
    selected=selection()
    return {'schema_version': 1, 'selection': selected, 'cuda_primary': {'id': CUDA_UUID, 'name': nvidia_name(CUDA_UUID, False), 'backend': 'cuda-driver', 'policy': 'EXISTING_AUTHORIZED_UUID', 'presence': 'NOT_PROBED'},
            'amd_devices': devices, 'missing_configured_devices': missing,
            'production_amd_admission': 'BLOCKED', 'economic_configuration_changed': False}


def configure_mode(mode, device_id=None):
    if mode not in MODES:
        raise ValueError('secondary AMD GPU accepts only disabled or diagnostics; mining/inference remain blocked')
    devices = amd_devices()
    matches = [d for d in devices if device_id is None or d['id'] == device_id]
    if len(matches) != 1:
        raise ValueError('Select exactly one present secondary AMD GPU by PCI id')
    prepare_state()
    fd = os.open(STATE / 'config.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        config = configuration()
        if lease_busy(matches[0]['id']):
            raise ValueError('AMD offline job is active; configuration unchanged')
        config['devices'][matches[0]['id']] = {'mode': mode, 'backend': 'vulkan-jit'}
        atomic(STATE / 'config.json', config)
    return 'secondary AMD GPU: ' + mode.upper() + ' · no GPU workload started; NVIDIA mining unchanged'


@contextmanager
def offline_lease(device_id, workload):
    """Per-PCI exclusive lease. No production workload can enter this path."""
    if workload not in ('offline-copy', 'offline-walk'):
        raise ValueError('Only offline copy/walk diagnostics are admitted; mining and AI remain blocked')
    prepare_state()
    guard = os.open(STATE / 'config.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    lease = None
    try:
        fcntl.flock(guard, fcntl.LOCK_EX)
        matches = [d for d in status()['amd_devices'] if d['id'] == device_id]
        if len(matches) != 1 or matches[0]['mode'] != 'diagnostics' or matches[0]['driver'] != 'amdgpu':
            raise ValueError('Explicit present AMD device must be configured for diagnostics')
        if matches[0]['capabilities_status'] != 'LIVE_READ_ONLY' or not all(matches[0].get('capabilities', {}).get(k) is True for k in ('shaderInt64', 'shaderInt8', 'storageBuffer8BitAccess')):
            raise ValueError('Refresh AMD capabilities before an offline session')
        lease = os.open(lease_path(device_id), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try: fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise ValueError('AMD device already has an offline session') from None
        os.close(guard); guard = None
        yield matches[0]
    finally:
        if lease is not None: os.close(lease)
        if guard is not None: os.close(guard)


def refresh_capabilities():
    prepare_state()
    if not PROBE.is_file() or PROBE.is_symlink():
        raise ValueError('Build the isolated nexus-vulkan-device-info target first')
    scratch = ROOT / 'work/tmp'
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch), TMP=str(scratch), TEMP=str(scratch))
    result = subprocess.run([str(PROBE)], capture_output=True, text=True, timeout=10, env=env)
    if result.returncode:
        raise ValueError('AMD capability probe failed; no production GPU assignment changed')
    obj = json.loads(result.stdout)
    if obj.get('schema_version') != 1 or obj.get('probe') != 'AMD_READ_ONLY' or not isinstance(obj.get('devices'), list):
        raise ValueError('Invalid AMD capability probe response')
    present = {d['id'] for d in amd_devices()}
    for d in obj['devices']:
        if d.get('id') not in present or d.get('vendor_id') != 0x1002 or d.get('device_id') != 0x67df or d.get('production_ready') is not False:
            raise ValueError('Vulkan/sysfs identity mismatch; capabilities rejected')
    obj.update(observed_unix=time.time(), boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(), probe_sha256=hashlib.sha256(PROBE.read_bytes()).hexdigest())
    atomic(STATE / 'capabilities.json', obj)
    return 'AMD capabilities refreshed · read-only; no queue, shader, inference or miner started'


def rows(value):
    devices = value.get('amd_devices', [])
    if not devices:
        return [('Secondary GPU', 'NOT DETECTED', 'muted')]
    result = []
    for device in devices:
        memory = device.get('vram_bytes')
        result.extend([(device.get('name') or 'AMD GPU', device['id']), ('AMD VRAM / driver', (f'{memory / 2**30:.1f} GiB' if memory else 'N/A') + ' / ' + device['driver']),
                       ('AMD mode', device['mode'].upper()), ('AMD capabilities', device['capabilities_status']),
                       ('AMD offline lease', device.get('offline_lease', 'UNKNOWN')),
                       ('AMD production', 'BLOCKED · resident search / inference / scheduling', 'warning')])
    if value.get('missing_configured_devices'):
        result.append(('AMD missing device', ', '.join(value['missing_configured_devices']), 'warning'))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set-rx570-mode', choices=MODES)
    parser.add_argument('--device-id')
    parser.add_argument('--refresh', action='store_true')
    args = parser.parse_args()
    try:
        if args.set_rx570_mode: print(configure_mode(args.set_rx570_mode, args.device_id))
        if args.refresh: print(refresh_capabilities())
        print(json.dumps(status(), indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, 'BLOCKED: ' + str(error) + '\n')
