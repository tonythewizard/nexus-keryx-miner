#!/usr/bin/env python3
"""Bounded AMD replay of a recorded mainnet candidate against the official GGUF.

No RPC/submission transport. Does not start or stop mining or other services.
The caller should run this process in a memory-limited scope.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / 'scripts'))
from nexus_gpu_devices import refresh_capabilities
from nexus_vulkan_runtime import session
from walk_probe import validate_manifest, ORACLE_SHA


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run(args):
    for name in ('context', 'trace', 'model', 'tree', 'metadata', 'runner', 'bridge', 'oracle', 'evidence'):
        path = getattr(args, name).resolve()
        if any(path == bad or bad in path.parents for bad in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
            raise ValueError('All artifacts must use persistent storage')
        setattr(args, name, path)
    if sha(args.oracle) != ORACLE_SHA:
        raise ValueError('Official Rust oracle hash mismatch')
    def model_identity():
        st = args.model.stat()
        return [st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns]
    model_before = model_identity()
    if args.context.stat().st_size > 8 * 1024 * 1024 or args.trace.stat().st_size > 1024 * 1024:
        raise ValueError('Recorded candidate exceeds bounded replay size')
    args.evidence.mkdir(parents=True, exist_ok=False)
    context = args.context.read_bytes()
    if len(context) < 225 or context[:4] != b'NXRF' or len(context) != 225 + struct.unpack_from('<I', context, 221)[0]:
        raise ValueError('Invalid recorded proof context')
    local_context = args.evidence / 'recorded.context'
    local_trace = args.evidence / 'recorded.trace'
    local_proof = args.evidence / 'recorded.proof'
    local_context.write_bytes(context)
    local_trace.write_bytes(args.trace.read_bytes())
    local_proof.write_bytes(context[225:])
    result = {'mode': 'OFFLINE_REPLAY', 'submission': 'NOT_LINKED', 'production_admission': False,
              'device_id': args.device_id, 'source_context_sha256': sha(local_context),
              'source_trace_sha256': sha(local_trace), 'oracle_sha256': sha(args.oracle),
              'runner_sha256': sha(args.runner), 'bridge_sha256': sha(args.bridge)}

    def command(name, argv, env=None, accepted=True, rejection=None):
        proc = subprocess.run([str(x) for x in argv], env=env, capture_output=True, text=True, timeout=60)
        (args.evidence / (name + '.log')).write_text(proc.stdout + proc.stderr)
        if (proc.returncode == 0) != accepted:
            raise RuntimeError(name + ': unexpected validation result; inspect persistent log')
        if not accepted and (not rejection or proc.returncode not in (1, 101) or
                             rejection not in proc.stdout + proc.stderr):
            raise RuntimeError(name + ': failure does not prove the intended rejection')
        return proc.stdout

    command('baseline-rust', [args.oracle, 'verify-real', local_context, local_trace, local_proof])
    refresh_capabilities()
    with session(args.device_id, 'offline-walk') as gpu:
        module = gpu.prepare_module(SOURCE / 'consensus/cuda_walk_kernels.cu', 'pom_v4_walk_kernel',
                                    128, bounded_indices_proven=True)
        # Contract for this adapter: one nonce, 256 x 1024-byte gathered tiles,
        # 256 in-range slots, 256 offsets and 257 states, all < 2**32 bytes.
        # Full model tile count is used only for 64-bit offset modulo, not buffer indexing.
        validate_manifest(module)
        (args.evidence / 'module.json').write_text(json.dumps(module, indent=2) + '\n')
        env = gpu.environment()
        if args.coordinator_replay:
            env.update(NEXUS_VULKAN_REPLAY_CONTEXT=str(local_context), NEXUS_VULKAN_REPLAY_EVENT='none')
        command('prepare', [args.bridge, 'prepare', args.model, args.tree, args.metadata, local_context, args.evidence], env)
        actual = args.evidence / 'model.output'
        command('dispatch', [args.runner, module['spirv'], '--gathered-walk', args.evidence / 'model.input', actual], env)
        command('assemble', [args.bridge, 'complete', args.model, args.tree, args.metadata, local_context, actual, args.evidence], env)
    verified = command('amd-rust', [args.oracle, 'verify-real', args.evidence / 'amd.context',
                                   args.evidence / 'amd.trace', args.evidence / 'amd.proof'])
    if 'OFFICIAL_RUST_REAL_MODEL=PASS' not in verified:
        raise RuntimeError('Missing explicit official Rust verdict')
    if (args.evidence / 'amd.proof').read_bytes() != local_proof.read_bytes():
        raise RuntimeError('AMD proof differs from the recorded canonical proof')
    # Keep intermediate trace corruption distinct from a proof-assembly error:
    # Borsh stores tile/path evidence, not every intermediate GPU state. Rust
    # must reject the corrupted trace even when final state/proof bytes agree.
    original_gpu = actual.read_bytes()
    negative_cases = []
    for name, position in [('guard', 8 + 2048), ('offset', 8),
                           ('state', 8 + 2048 + 16 + 1024 + 16 + 1024)]:
        corrupt = bytearray(original_gpu)
        corrupt[position] ^= 1
        fixture = args.evidence / ('bad-' + name + '.output')
        fixture.write_bytes(corrupt)
        directory = args.evidence / ('bad-' + name)
        command('reject-' + name + '-assembly',
                [args.bridge, 'complete', args.model, args.tree, args.metadata, local_context, fixture, directory],
                accepted=name == 'state', rejection={'guard': 'GPU output guard damaged',
                    'offset': 'Vulkan candidate differs from recorded verified proof'}.get(name))
        if name == 'state':
            command('reject-state-rust', [args.oracle, 'verify-real', directory / 'amd.context',
                                         directory / 'amd.trace', directory / 'amd.proof'],
                    accepted=False, rejection='state mismatch')
        negative_cases.append(name)
    bad_context = bytearray(context)
    bad_context[85] ^= 1  # pre-PoW hash must bind to the recorded seed
    fixture = args.evidence / 'bad-seed.context'
    fixture.write_bytes(bad_context)
    command('reject-seed-binding', [args.bridge, 'prepare', args.model, args.tree, args.metadata,
                                    fixture, args.evidence / 'bad-seed'], accepted=False,
            rejection='Recorded current-era seed/nonce binding invalid')
    negative_cases.append('seed_binding')
    # Malformed pool bounds must be rejected by the native adapter before a
    # dispatch, even though the logical model tile count is much larger.
    with session(args.device_id, 'offline-walk') as gpu:
        env = gpu.environment()
        payload = (args.evidence / 'model.input').read_bytes()
        for name, position, value in [('zero_pool', 24, 0), ('oversized_pool', 24, 1025),
                                       ('slot_out_of_pool', 28 + 8 + 256 * 1024, 256)]:
            corrupt = bytearray(payload)
            struct.pack_into('<I', corrupt, position, value)
            fixture = args.evidence / ('bad-' + name + '.input')
            fixture.write_bytes(corrupt)
            command('reject-' + name, [args.runner, module['spirv'], '--gathered-walk', fixture,
                                      args.evidence / 'must-not-exist.output'], env, accepted=False,
                    rejection='Tile slot outside bounded buffer' if name == 'slot_out_of_pool' else 'Invalid bounded walk fixture')
            negative_cases.append(name)
        if args.coordinator_replay:
            for event in ('reorg', 'ai'):
                env.update(NEXUS_VULKAN_REPLAY_CONTEXT=str(local_context), NEXUS_VULKAN_REPLAY_EVENT=event)
                output = args.evidence / ('discarded-' + event + '.output')
                reply = command('coordinator-' + event, [args.runner, module['spirv'], '--gathered-walk',
                                                        args.evidence / 'model.input', output], env)
                if 'WORK_REPLAY=PASS' not in reply or 'candidate=DISCARDED' not in reply or output.exists():
                    raise RuntimeError('Cancelled GPU work escaped the coordinator gate')
    if (args.evidence / 'must-not-exist.output').exists():
        raise RuntimeError('Malformed pool fixture reached output generation')
    if model_before != model_identity():
        raise RuntimeError('GGUF identity or modification time changed during replay')
    result.update(status='PASS', official_rust=verified.strip(),
                  proof_bytes=(args.evidence / 'amd.proof').stat().st_size,
                  proof_sha256=sha(args.evidence / 'amd.proof'),
                  uploaded_tile_bytes=256 * 1024, negative_cases=negative_cases,
                  coordinator_replay=args.coordinator_replay,
                  coordinator_job_envelope='SYNTHETIC' if args.coordinator_replay else 'NOT_USED',
                  model_identity=model_before, model_modified=False, production_modified=False)
    (args.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('context', 'trace', 'model', 'tree', 'metadata', 'runner', 'bridge', 'oracle', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--device-id', default='pci:0000:00:00.0')
    parser.add_argument('--coordinator-replay', action='store_true',
                        help='Inject synthetic reorg/AI events after actual Vulkan queue submission')
    args = parser.parse_args()
    try:
        run(args)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print('AMD_REAL_MODEL_REPLAY=BLOCKED ' + str(exc), file=sys.stderr)
        raise SystemExit(1)
