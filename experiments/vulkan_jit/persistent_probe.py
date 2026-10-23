#!/usr/bin/env python3
"""Same-process Vulkan engine replay; historical inputs, no mainnet writes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from real_model_probe import SOURCE, sha
from nexus_gpu_devices import refresh_capabilities
from nexus_vulkan_runtime import session
from walk_probe import validate_manifest, ORACLE_SHA


def run(args):
    for name in ('first', 'second', 'runner', 'bridge', 'oracle', 'model', 'tree', 'metadata', 'evidence'):
        path = getattr(args, name).resolve()
        if any(p == path or p in path.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
            raise ValueError('Persistent paths required')
        setattr(args, name, path)
    if sha(args.oracle) != ORACLE_SHA:
        raise ValueError('Official oracle hash mismatch')
    args.evidence.mkdir(parents=True, exist_ok=False)
    def command(name, cmd, env=None, rejection=None):
        p = subprocess.run([str(x) for x in cmd], env=env, capture_output=True, text=True, timeout=60)
        (args.evidence / (name + '.log')).write_text(p.stdout + p.stderr)
        if rejection:
            if p.returncode != 1 or rejection not in p.stdout + p.stderr:
                raise RuntimeError(name + ': expected specific rejection was not observed')
        elif p.returncode:
            raise RuntimeError(name + ': inspect persistent failure log')
        return p.stdout
    for name, record in [('first', args.first), ('second', args.second)]:
        command(name + '-baseline', [args.oracle, 'verify-real', record / 'recorded.context',
                                    record / 'recorded.trace', record / 'recorded.proof'])
    refresh_capabilities()
    with session('pci:0000:00:00.0', 'offline-walk') as gpu:
        module = gpu.prepare_module(SOURCE / 'consensus/cuda_walk_kernels.cu', 'pom_v4_walk_kernel',
                                    128, bounded_indices_proven=True)
        validate_manifest(module)
        manifest = json.loads(Path(module['manifest']).read_text())
        (args.evidence / 'module.json').write_text(json.dumps(module, indent=2) + '\n')
        env = gpu.environment()
        base = [args.runner, module['spirv'], manifest['spirv_sha256'],
                args.first / 'model.input', args.first / 'recorded.context',
                args.second / 'model.input', args.second / 'recorded.context',
                args.evidence / 'gpu', 'pci:0000:00:00.0']
        command('module-hash-rejection', base[:2] + ['0' * 64] + base[3:], env,
                'JIT module SHA-256 mismatch')
        wrong_device = base[:-1] + ['pci:0000:00:00.0']
        command('device-rejection', wrong_device, env, 'Unsupported device/capacity')
        stdout = command('persistent-dispatch', base, env)
        if 'VULKAN_PERSISTENT_ENGINE=PASS' not in stdout:
            raise RuntimeError('Missing explicit engine verdict')
    proofs = []
    for label, record in [('first', args.first), ('second', args.second), ('resumed', args.first)]:
        destination = args.evidence / label
        command(label + '-assemble', [args.bridge, 'complete', args.model, args.tree, args.metadata,
                                      record / 'recorded.context', args.evidence / 'gpu' / (label + '.output'), destination])
        official = command(label + '-rust', [args.oracle, 'verify-real', destination / 'amd.context',
                                             destination / 'amd.trace', destination / 'amd.proof'])
        if 'OFFICIAL_RUST_REAL_MODEL=PASS' not in official:
            raise RuntimeError('Missing independent Rust verification')
        proofs.append({'case': label, 'sha256': sha(destination / 'amd.proof'), 'official_rust': official.strip()})
    for label in ('reorg', 'ai'):
        if (args.evidence / 'gpu' / ('must-not-exist-' + label + '.output')).exists():
            raise RuntimeError('Cancelled job produced an artifact')
    result = {'status': 'PASS', 'mode': 'OFFLINE_REPLAY', 'submission': 'NOT_LINKED',
              'device_instances': 1, 'buffer_reallocation': 'NONE', 'proofs': proofs,
              'runner_sha256': sha(args.runner), 'engine_output': stdout.strip()}
    (args.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('first', 'second', 'runner', 'bridge', 'oracle', 'model', 'tree', 'metadata', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print('VULKAN_PERSISTENT_ENGINE=BLOCKED ' + str(exc))
        raise SystemExit(1)
