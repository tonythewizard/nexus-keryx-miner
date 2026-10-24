#!/usr/bin/env python3
"""Launch the packaged CLI with an explicit private profile and authorization."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def launch_command(profile, directory, live=False, claims=False):
    fields = {'schema_version', 'data_dir', 'gpu_uuid', 'endpoint', 'payout',
              'delegation', 'model', 'tree', 'metadata', 'escrow_key', 'output'}
    if not isinstance(profile, dict) or set(profile) != fields or type(profile['schema_version']) is not int or profile['schema_version'] != 1:
        raise ValueError('Unsupported private profile')
    if any(not isinstance(profile[key], str) or not profile[key] for key in fields - {'schema_version'}):
        raise ValueError('Fill every profile field locally')
    uuid = profile['gpu_uuid']
    if not re.fullmatch(r'GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', uuid) or uuid == 'GPU-00000000-0000-0000-0000-000000000000':
        raise ValueError('Select an explicit local GPU UUID')
    for key in ('data_dir', 'model', 'tree', 'metadata', 'escrow_key', 'output'):
        if not Path(profile[key]).is_absolute():
            raise ValueError(f'Absolute private path required: {key}')
    if claims and not live:
        raise ValueError('Escrow claims require explicit live authorization')
    ext = '.exe' if os.name == 'nt' else ''
    engine = directory / ('keryx-llama.dll' if os.name == 'nt' else 'libkeryx-llama.so')
    worker = directory / ('nexus-live-cuda-worker' + ext)
    oracle = directory / ('nexus-m21-oracle' + ext)
    if not all(path.is_file() for path in (engine, worker, oracle)):
        raise ValueError('Incomplete release package')
    environment = os.environ.copy()
    environment.update(NEXUS_DATA_DIR=profile['data_dir'],
                       NEXUS_PRIMARY_GPU_UUID=uuid, CUDA_VISIBLE_DEVICES=uuid,
                       NEXUS_POM_BACKEND='cuda-driver', NEXUS_POM_DEVICE_ID=uuid,
                       NEXUS_ESCROW_KEY_FILE=profile['escrow_key'], NEXUS_OPOI_LIBRARY=str(engine),
                       NEXUS_BOUND_FILE_CACHE='0', NEXUS_PRIMARY_POM_ENABLED='1')
    # This release contains one CUDA lane. Inherited experimental settings may
    # not silently add devices, select another backend or alter inference.
    for name in list(environment):
        if name.startswith(('NEXUS_SECONDARY_', 'NEXUS_VULKAN_', 'NEXUS_PRIMARY_VULKAN_', 'NEXUS_AI_BACKEND')):
            del environment[name]
    if os.name != 'nt':
        environment['LD_LIBRARY_PATH'] = str(directory) + os.pathsep + environment.get('LD_LIBRARY_PATH', '')
    args = [str(worker), profile['endpoint'], profile['payout'], profile['delegation'],
            profile['model'], profile['tree'], profile['metadata'], str(oracle), profile['output'],
            '0' if live else '1', '1', 'live' if live else 'shadow',
            '--enable-submit' if live else '--no-submit',
            '--enable-ai-response' if live else '--no-ai-response',
            '--enable-escrow-claims' if claims else '--no-escrow-claims']
    return args, environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--live', action='store_true', help='authorize PoM and AI response submissions')
    parser.add_argument('--enable-escrow-claims', action='store_true')
    args = parser.parse_args()
    if args.profile.stat().st_size > 16384:
        parser.error('Profile exceeds size limit')
    try:
        profile = json.loads(args.profile.read_text(encoding='utf-8'))
        command, environment = launch_command(profile, Path(__file__).resolve().parent,
                                               args.live, args.enable_escrow_claims)
        result = subprocess.run(command, env=environment, check=False)
        raise SystemExit(result.returncode)
    except (ValueError, OSError):
        parser.exit(2, 'Launch refused: check the private profile and complete release package.\n')


if __name__ == '__main__':
    main()
