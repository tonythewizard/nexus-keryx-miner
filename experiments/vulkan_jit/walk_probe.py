#!/usr/bin/env python3
"""Offline AMD walk parity: JIT source -> Vulkan -> official Rust fixture.

No model, RPC, production process, CUDA device or mining submission is used.
The extra seed uses a small CPU reference, first checked against Rust's vector.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

from translate import compile_kernel, Unsupported

MASK = (1 << 64) - 1
ORACLE_SHA = 'ecf1132e3a19f494255c0c6c2c14d59c7c71c74c331ddb5a4eb78a2c66a95641'


def mix(x):
    x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & MASK
    x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & MASK
    return x ^ (x >> 31)


def blob64():
    blob = bytearray()
    h = 0xdeadbeef
    for _ in range(64 * 1024 + 3 * 32):
        h = mix(h)
        blob.append(h & 255)
    return bytes(blob)


def cpu_walk(seed, blob):
    state = bytearray()
    for row in range(32):
        h = mix(seed ^ (0x03421325594C3C51 + row))
        for _ in range(8):
            h = mix(h)
            state.extend(struct.pack('<I', h & 0xffffffff))
    trace, offsets = bytearray(state), []
    ntiles = len(blob) // 1024
    offset = mix(seed ^ 0x6D1CCF96AC4D76F9) % ntiles
    signed = [[b if b < 128 else b - 256 for b in blob[t * 1024:(t + 1) * 1024]] for t in range(ntiles)]
    for step in range(1, 257):
        offsets.append(offset)
        tile = blob[offset * 1024:(offset + 1) * 1024]
        lhs = [b if b < 128 else b - 256 for b in state]
        rhs = signed[offset]
        out = bytearray(1024)
        for row in range(32):
            a = lhs[row * 32:row * 32 + 32]
            for col in range(32):
                b = rhs[col * 32:col * 32 + 32]
                acc = sum(x * y for x, y in zip(a, b))
                tweak = (step * 0x9E3779B9 + row * 0xC2B2AE35 + col * 0x85EBCA6B) & 0xffffffff
                z = (((acc & 0xffffffff) ^ tweak) * 0x9E3779B9) & 0xffffffff
                z ^= z >> 16
                z = (z * 0x85EBCA6B) & 0xffffffff
                z ^= z >> 13
                out[row * 32 + col] = z & 255
        state = out
        trace.extend(state)
        if step < 256:
            fold = 0
            for word in struct.unpack('<8I', tile[:32]):
                fold = mix(fold ^ word)
            offset = mix(seed ^ (((step + 1) * 0x89050E78D34609EF) & MASK) ^ fold) % ntiles
    return struct.pack('<256Q', *offsets), bytes(trace)


def validate_manifest(compiled, resident=False, banked=False):
    manifest = json.loads(Path(compiled['manifest']).read_text())
    expected = [('seeds', 'buffer'), ('tiles', 'buffer'), ('tile_slots', 'buffer'), ('ntiles', 'scalar'),
                ('offsets', 'buffer'), ('final_states', 'buffer'), ('trace', 'buffer'), ('diagnostics', 'scalar')]
    if resident or banked:
        expected.remove(('tile_slots', 'buffer'))
    if banked:
        expected[1:2]=[('tiles0','buffer'),('tiles1','buffer')]
        expected.insert(expected.index(('ntiles','scalar'))+1,('split_tiles','scalar'))
        if manifest['identity']['entry']!='pom_v4_banked_resident_walk_kernel':
            raise ValueError('Banked module entry mismatch')
    if [(a['name'], a['kind']) for a in manifest['arguments']] != expected:
        raise ValueError('Unexpected walk ABI')
    types = {'seeds': 'std::uint64_t', 'tiles': 'std::uint8_t', 'tiles0': 'std::uint8_t',
             'tiles1': 'std::uint8_t', 'tile_slots': 'std::uint32_t', 'offsets': 'std::uint64_t',
             'final_states': 'std::uint8_t', 'trace': 'std::uint8_t'}
    outputs = {'offsets', 'final_states', 'trace'}
    for binding, arg in enumerate(a for a in manifest['arguments'] if a['kind'] == 'buffer'):
        if (arg['binding'] != binding or arg['cuda_type'] != types[arg['name']]
                or arg['readonly'] != (arg['name'] not in outputs)):
            raise ValueError('Walk buffer element type, binding or access mismatch')
    scalars = [(a['offset'], a['size']) for a in manifest['arguments'] if a['kind'] == 'scalar']
    if scalars != ([(0,8),(8,8),(16,4)] if banked else [(0, 8), (8, 4)]) or manifest['spirv_sha256'] != hashlib.sha256(Path(compiled['spirv']).read_bytes()).hexdigest():
        raise ValueError('Walk scalar layout or SPIR-V integrity mismatch')
    reflection = manifest.get('reflection', {})
    constants = reflection.get('source_constants', {})
    stride = constants.get('tile_row_stride_cuda', 32)
    offset_bytes = reflection.get('shared_arrays', {}).get('next_offset', 0)
    expected_shared = (2048 + 32 * stride + offset_bytes) * 4
    if reflection.get('shared_storage_layout') == 'packed-s8x4-v1':
        if reflection.get('shared_arrays') != {'state': 512, 'tile': 288, 'next_offset': 8} or stride != 33 or not banked:
            raise ValueError('Invalid packed shared-memory contract')
        expected_shared = 3232
    if (stride not in (32, 33) or offset_bytes not in (0, 8)
            or ((stride != 32 or offset_bytes) and not (resident or banked))
            or constants.get('tile_bytes_cuda') != 1024 or constants.get('steps_cuda') != 256
            or reflection.get('shared_memory_bytes') != expected_shared):
        raise ValueError('Source dimensions differ from the bounded fixture contract; dispatch refused')


def run(source, runner, oracle, cache, evidence, resident=False, proof_runner=None):
    original = source.read_bytes()
    if hashlib.sha256(oracle.read_bytes()).hexdigest() != ORACLE_SHA:
        raise ValueError('Oracle does not match the attested official-reference executable')
    evidence.mkdir(parents=True, exist_ok=False)
    reference = evidence / 'official.nxv4'
    proc = subprocess.run([str(oracle), 'generate', str(reference)], capture_output=True, text=True, timeout=20)
    (evidence / 'rust-generate.log').write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError('Official Rust fixture generation failed')
    raw = reference.read_bytes()
    if raw[:4] != b'NXV4':
        raise ValueError('Unexpected official fixture')
    seed = struct.unpack_from('<Q', raw, 4)[0]
    position = 4 + 8 + 8 + 1 + 32 + 8
    rust_offsets = raw[position:position + 256 * 8]
    rust_trace = raw[position + 256 * 8:position + 256 * 8 + 257 * 1024]
    blob = blob64()
    started = time.monotonic()
    ref0 = cpu_walk(seed, blob)
    if ref0 != (rust_offsets, rust_trace):
        raise RuntimeError('CPU auxiliary reference differs from official Rust states/offsets')
    seeds = [seed, seed ^ MASK]
    references = [ref0, cpu_walk(seeds[1], blob)]
    cpu_s = time.monotonic() - started
    slots = b''.join(struct.pack('<256I', *struct.unpack('<256Q', r[0])) for r in references)
    # Every index is proven below 2**32: 2 seeds, 64 full tiles, 512 slots,
    # 512 offsets, 2048 final bytes and 526336 trace bytes. Slots < 64.
    header = (b'NXWR1\0\0\0' if resident else b'NXWK1\0\0\0') + struct.pack('<IIQ', len(seeds), 1, 64)
    payload = struct.pack('<2Q', *seeds) + blob[:64 * 1024] + (b'' if resident else slots)
    equivalent = evidence / 'equivalent-source.cu'
    old = b'acc += lhs * rhs;'
    if original.count(old) != 1:
        raise ValueError('Unexpected walk expression for source-change probe')
    equivalent.write_bytes(original.replace(old, b'acc += lhs * rhs + 0;'))
    mutation = evidence / 'corrupted-source.cu'
    mutation.write_bytes(original.replace(old, b'acc += lhs * rhs + 1;'))
    cases = [('original', source, 128, True, False), ('no_diagnostics', source, 128, False, False),
             ('workgroup64', source, 64, True, False), ('workgroup256', source, 256, True, False),
             ('runtime_equivalent_change', equivalent, 128, True, False),
             ('runtime_corruption', mutation, 128, True, True), ('original_cached', source, 128, True, False)]
    results = []
    for name, path, local, diagnostics, expect_mismatch in cases:
        compiled = compile_kernel(path, 'pom_v4_resident_walk_kernel' if resident else 'pom_v4_walk_kernel', cache, local, True)
        validate_manifest(compiled, resident)
        fixture, output = evidence / (name + '.input'), evidence / (name + '.output')
        fixture.write_bytes(header[:12] + struct.pack('<I', int(diagnostics)) + header[16:] + payload)
        dispatch = subprocess.run([str(runner), compiled['spirv'], '--resident-walk' if resident else '--walk', str(fixture), str(output)], capture_output=True, text=True, timeout=30)
        (evidence / (name + '.log')).write_text(dispatch.stdout + dispatch.stderr)
        if dispatch.returncode:
            raise RuntimeError(name + ': Vulkan dispatch failed; no automatic retry')
        actual = output.read_bytes()
        if actual[:8] != b'NXWO1\0\0\0':
            raise ValueError('Unexpected output format')
        cursor, parts = 8, []
        for length in (2 * 256 * 8, 2 * 1024, 2 * 257 * 1024 if diagnostics else 0):
            parts.append(actual[cursor:cursor + length])
            if actual[cursor + length:cursor + length + 16] != b'\xcd' * 16:
                raise RuntimeError(name + ': GPU guard overwritten')
            cursor += length + 16
        if cursor != len(actual):
            raise ValueError('Truncated/extra GPU output')
        expected = [b''.join(r[0] for r in references), b''.join(r[1][-1024:] for r in references),
                    b''.join(r[1] for r in references) if diagnostics else b'']
        match = parts == expected
        if match == expect_mismatch:
            raise RuntimeError(name + ': parity outcome differs from expectation')
        results.append({'case': name, 'parity': 'PASS' if match else 'CORRUPTION_DETECTED', 'guards': 'PASS',
                        'nonces': 2, 'states_per_nonce': 257 if diagnostics else 'final_only', 'offsets_per_nonce': 256,
                        **compiled, 'dispatch_log': dispatch.stdout})
        (evidence / 'results.json').write_text(json.dumps({'cpu_reference_s': cpu_s, 'results': results}, indent=2) + '\n')
    if results[0]['cache_key'] == results[4]['cache_key'] or results[0]['cache_key'] != results[-1]['cache_key'] or not results[-1]['cache_hit']:
        raise RuntimeError('Runtime source invalidation/cache reuse failed')
    if source.read_bytes() != original:
        raise RuntimeError('Repository source changed during the experiment')
    if proof_runner:
        if not resident:
            raise ValueError('GPU proof bridge requires the resident-walk fixture')
        wire = evidence / 'gpu-proof.borsh'
        def proof_command(name, command, accepted=True):
            proc = subprocess.run([str(arg) for arg in command], capture_output=True, text=True, timeout=20)
            (evidence / (name + '.log')).write_text(proc.stdout + proc.stderr)
            if (proc.returncode == 0) != accepted:
                raise RuntimeError(name + ': unexpected proof validation outcome')
        proof_command('gpu-proof-build', [proof_runner, reference, evidence / 'original.output', wire])
        proof_command('gpu-proof-rust', [oracle, 'verify', wire])
        proof_command('gpu-proof-rust-zero-target', [oracle, 'reject-target', wire])
        proof_command('gpu-proof-rust-tier', [oracle, 'reject-tier', wire])
        malformed = evidence / 'truncated.borsh'
        malformed.write_bytes(wire.read_bytes()[:-1])
        proof_command('gpu-proof-rust-borsh', [oracle, 'reject-borsh', malformed])
        original_gpu = (evidence / 'original.output').read_bytes()
        for name, position in [('offset', 8), ('guard', 8 + 2 * 256 * 8),
                               ('state', 8 + 2 * 256 * 8 + 16 + 2 * 1024 + 16)]:
            corrupt = bytearray(original_gpu)
            corrupt[position] ^= 1
            fixture = evidence / ('corrupted-' + name + '.output')
            fixture.write_bytes(corrupt)
            proof_command('gpu-proof-reject-' + name,
                          [proof_runner, reference, fixture, evidence / 'must-not-exist.borsh'], False)
        proof_command('gpu-proof-reject-kernel-corruption',
                      [proof_runner, reference, evidence / 'runtime_corruption.output', evidence / 'must-not-exist.borsh'], False)
        if (evidence / 'must-not-exist.borsh').exists():
            raise RuntimeError('Rejected witness produced a proof file')
        (evidence / 'proof-result.json').write_text(json.dumps({
            'gpu_borsh': 'BYTE_IDENTICAL', 'official_rust_verify': 'PASS',
            'proof_bytes': wire.stat().st_size, 'negative_cases': 7,
            'target': 'OFFLINE_FIXTURE_ONLY', 'submission': 'NOT_LINKED'}, indent=2) + '\n')
    print(json.dumps({'walk_parity': 'PASS', 'official_rust_vector': 'MATCH', 'second_seed_cpu_reference': 'MATCH',
                      'runtime_source_change': 'PASS', 'source_corruption_detection': 'PASS', 'cases': len(results),
                      'cpu_reference_s': cpu_s, 'production_backend': 'UNCHANGED', 'gpu_tile_selection': resident, 'cpu_tile_slots_supplied': not resident}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--resident', action='store_true', help='Test GPU-addressed resident tiles, without CPU tile slots')
    parser.add_argument('--proof-runner', type=Path, help='Optional offline Borsh/Rust bridge for resident tests')
    for name in ('runner', 'oracle', 'cache', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for path in (args.cache, args.evidence):
        resolved = path.resolve()
        if any(resolved == p or p in resolved.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
            parser.error('Use persistent storage outside temporary directories')
    source = args.source or (Path(__file__).resolve().parents[2] / 'consensus' /
                             ('cuda_portable_resident.cu' if args.resident else 'cuda_walk_kernels.cu'))
    if args.proof_runner and not args.resident:
        parser.error('--proof-runner requires --resident')
    run(source.resolve(), args.runner.resolve(), args.oracle.resolve(), args.cache.resolve(), args.evidence.resolve(),
        args.resident, args.proof_runner.resolve() if args.proof_runner else None)
