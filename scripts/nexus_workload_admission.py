"""Admission for the currently attested worker, never a backend fallback.

The saved preference requires a matching executable capability and hash.
Presentation preferences alone cannot attest an executable backend.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import re
import subprocess


def primary_backend(device, preferences, vulkan_worker=False):
    from nexus_gpu_registry import resolve_workload
    requested = preferences.get('workloads', {}).get(device, 'auto')
    resolved = resolve_workload({'vendor': 'nvidia'}, requested)
    if resolved == 'nexus_bda':
        if vulkan_worker:
            return 'vulkan-bda'
        raise ValueError('NVIDIA_VULKAN_NOT_ADMITTED: installed worker lacks this route; refusing CUDA fallback')
    if resolved != 'nvidia':
        if vulkan_worker:
            return 'vulkan-jit'
        raise ValueError('NVIDIA_VULKAN_NOT_ADMITTED: installed worker lacks this route; refusing CUDA fallback')
    return 'cuda-driver'


def device_backend(device, preferences):
    """Pure vendor policy. Identity, driver features and executable admission follow separately."""
    from nexus_gpu_registry import resolve_workload
    resolved = resolve_workload(device, preferences.get('workloads', {}).get(device['id'], 'auto'))
    return {'nvidia':'cuda-driver', 'nexus':'vulkan-jit', 'nexus_bda':'vulkan-bda'}[resolved]


def worker_capabilities(worker, expected_sha):
    if not re.fullmatch('[0-9a-f]{64}', expected_sha):
        raise ValueError('Invalid worker SHA-256 pin')
    with Path(worker).open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != expected_sha:
            raise ValueError('Worker SHA-256 mismatch')
    p = subprocess.run([str(worker), '--inference-capabilities'], capture_output=True, timeout=5, check=True)
    if len(p.stdout) > 4096:
        raise ValueError('Oversized worker capabilities')
    caps = json.loads(p.stdout)
    if type(caps.get('schema_version')) is not int or caps['schema_version'] != 1:
        raise ValueError('Unsupported worker capability schema')
    return caps


def admit_routes(selection, workloads, worker, sha):
    from nexus_gpu_selection import CUDA_UUID, AMD_ID
    devices = [(CUDA_UUID,'nvidia','nvidia_pom'), (AMD_ID,'amd','amd_pom')]
    routes = {ident:device_backend({'id':ident,'vendor':vendor}, {'workloads':workloads})
              for ident,vendor,key in devices}
    caps = worker_capabilities(worker, sha)
    mixed = selection['nvidia_pom'] and selection['amd_pom'] and routes[CUDA_UUID] != 'cuda-driver'
    if mixed and caps.get('independent_vulkan_lanes') is not True:
        raise ValueError('Installed worker does not support independent Vulkan lanes; no settings changed')
    for ident,vendor,key in devices:
        if not selection[key]:
            continue
        backend = routes[ident]
        if backend.startswith('vulkan') and caps.get('vulkan_helper') is not True:
            raise ValueError('Installed worker lacks Vulkan support')
        if ident == CUDA_UUID and backend.startswith('vulkan') and caps.get('primary_vulkan') is not True:
            raise ValueError('Installed worker lacks primary Vulkan support')
        if backend == 'vulkan-bda':
            flag = 'primary_vulkan_bda' if ident == CUDA_UUID else 'secondary_vulkan_bda'
            if caps.get(flag) is not True:
                raise ValueError('Installed worker lacks the selected BDA route')
            env = dict(os.environ)
            icd = '/usr/share/vulkan/icd.d/' + ('nvidia_icd.json' if vendor == 'nvidia' else 'radeon_icd.json')
            env.update(VK_DRIVER_FILES=icd, VK_ICD_FILENAMES=icd, VK_LOADER_LAYERS_DISABLE='~implicit~')
            p = subprocess.run([str(worker), '--probe-bda-route', ident], env=env,
                               capture_output=True, timeout=15)
            if p.returncode or p.stdout.strip() != b'BDA_ROUTE_CAPABILITY=PASS':
                raise ValueError('BDA admission failed for '+ident+': '+p.stderr.decode(errors='replace')[:1024])
    if not selection['nvidia_pom']:
        routes[CUDA_UUID] = 'cuda-driver'  # Inactive primary must not select its saved Vulkan/AI route.
    return routes


def vulkan_worker_available(worker, expected_sha, *, require_bda=False):
    capabilities = worker_capabilities(worker, expected_sha)
    return (type(capabilities.get('schema_version')) is int and capabilities['schema_version'] == 1
            and capabilities.get('primary_vulkan') is True and capabilities.get('vulkan_helper') is True
            and (not require_bda or capabilities.get('primary_vulkan_bda') is True))


def installed_worker():
    # Resolve the actual service source, not whichever UI/worktree invokes us.
    from nexus_tauri_actions import production_source
    from nexus_agent_api import ROOT
    source = production_source()
    text = (source / 'scripts/nexusmine-next.fish').read_text()
    path = re.search(r'set -l worker "\$recovery/([^"\n]+)"', text)
    sha = re.search(r'set -l expected_sha256 ([0-9a-f]{64})', text)
    if not path or not sha:
        raise ValueError('Installed worker pin unavailable')
    worker = (ROOT / path.group(1)).resolve(strict=True)
    if not worker.is_relative_to(ROOT.resolve()):
        raise ValueError('Installed worker is outside persistent recovery storage')
    return worker, sha.group(1)


def read_preferences(path):
    from nexus_ui_preferences import DEFAULTS, validate
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return dict(DEFAULTS)
    with os.fdopen(fd) as f:
        info = os.fstat(f.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError('Invalid private workload preferences')
        data = f.read(16385)
        if len(data) > 16384:
            raise ValueError('Oversized workload preferences')
        return validate(json.loads(data))


def main():
    from nexus_gpu_devices import CUDA_UUID
    from nexus_ui_preferences import preferences_path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', choices=[CUDA_UUID])
    parser.add_argument('--runtime-routes', action='store_true')
    parser.add_argument('--preferences', type=Path, default=preferences_path())
    parser.add_argument('--worker', type=Path)
    parser.add_argument('--worker-sha256')
    a = parser.parse_args()
    try:
        preferences = read_preferences(a.preferences)
        if bool(a.worker) != bool(a.worker_sha256):
            raise ValueError('Worker path and SHA-256 must be supplied together')
        if a.runtime_routes:
            from nexus_gpu_selection import configuration, AMD_ID
            worker, sha = (a.worker, a.worker_sha256) if a.worker else installed_worker()
            routes = admit_routes(configuration(), preferences['workloads'], worker, sha)
            print(routes[CUDA_UUID]);print(routes[AMD_ID]);return
        if not a.device:
            raise ValueError('Specify --device or --runtime-routes')
        available = False
        if preferences.get('workloads', {}).get(a.device) in ('nexus', 'nexus_bda'):
            worker, sha = (a.worker, a.worker_sha256) if a.worker else installed_worker()
            available = vulkan_worker_available(worker, sha,
                require_bda=preferences['workloads'][a.device] == 'nexus_bda')
        print(primary_backend(a.device, preferences, available))
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(13, 'NEXUS_WORKLOAD=BLOCKED ' + str(exc) + '\n')


if __name__ == '__main__':
    main()
