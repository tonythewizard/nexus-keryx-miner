#!/usr/bin/env python3
"""Explicit small AMD-only experiment. No RPC, model, wallet or mining.

The modified fixture changes only the copy kernel's tile constant in a
persistent copy of the CUDA source; the repository original is never changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from translate import compile_kernel, Unsupported


def run(source, runner, cache, evidence):
    source = source.resolve()
    original = source.read_bytes()
    if b'tile_bytes_cuda = 1024;' not in original:
        raise ValueError('Probe expects the current scatter kernel and 1024-byte tiles')
    evidence.mkdir(parents=True, exist_ok=False)
    variant = evidence / 'changed-scatter.cu'
    variant.write_bytes(original.replace(b'tile_bytes_cuda = 1024;', b'tile_bytes_cuda = 512;', 1))
    results = []
    for name, path, tile in [('original', source, 1024), ('modified_at_runtime', variant, 512), ('original_again', source, 1024)]:
        # Host bound proof for this fixture: every slot < 32, tile <= 1024,
        # every byte offset < 32768, comfortably below the 32-bit index ABI.
        compiled = compile_kernel(path, 'cache_scatter_kernel', cache, 128, True)
        manifest = json.loads(Path(compiled['manifest']).read_text())
        if manifest['spirv_sha256'] != hashlib.sha256(Path(compiled['spirv']).read_bytes()).hexdigest():
            raise ValueError('Generated SPIR-V hash mismatch')
        expected = [('cache', 'buffer'), ('packed_tiles', 'buffer'), ('destination_slots', 'buffer'), ('tile_count', 'scalar')]
        if [(arg['name'], arg['kind']) for arg in manifest['arguments']] != expected:
            raise ValueError('Unexpected scatter ABI')
        dispatch = subprocess.run([str(runner.resolve()), compiled['spirv'], str(tile)], capture_output=True, text=True, timeout=30)
        (evidence / (name + '.log')).write_text(dispatch.stdout + dispatch.stderr)
        results.append({'case': name, **compiled, 'dispatch_returncode': dispatch.returncode, 'output': dispatch.stdout})
        (evidence / 'runtime-results.json').write_text(json.dumps(results, indent=2) + '\n')
        if dispatch.returncode or 'SCATTER_BYTE_PARITY=PASS' not in dispatch.stdout or 'GUARD_BYTES=PASS' not in dispatch.stdout:
            raise RuntimeError('Scatter execution failed; no automatic retry')
    if results[0]['cache_key'] == results[1]['cache_key'] or results[0]['cache_key'] != results[2]['cache_key'] or not results[2]['cache_hit']:
        raise RuntimeError('Source change invalidation/cache reuse failed')
    try:
        compile_kernel(source.with_name('cuda_resident_kernels.cu'), 'nexus_resident_search_preseeded', cache, 128, True)
    except Unsupported as error:
        (evidence / 'unsupported-resident.txt').write_text(str(error) + '\n')
    else:
        raise RuntimeError('Unexpected resident search admission')
    if source.read_bytes() != original:
        raise RuntimeError('Repository source changed during experiment')
    print(json.dumps({'scatter': 'PASS', 'runtime_source_change': 'PASS', 'cache_reuse': 'PASS',
                      'unsupported_resident_search': 'REJECTED', 'full_cuda_workflow': 'NOT_IMPLEMENTED',
                      'results': results}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'consensus/cuda_walk_kernels.cu')
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    for path in (args.cache, args.evidence):
        resolved = path.resolve()
        if any(resolved == p or p in resolved.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
            parser.error('Use persistent storage outside temporary directories')
    run(args.source, args.runner, args.cache, args.evidence)
