#!/usr/bin/env python3
"""CPU-only rejection checks for the experimental typed CUDA frontend."""
import argparse
import json
from pathlib import Path

from translate import compile_kernel, Unsupported


def run(source, cache, evidence):
    evidence.mkdir(parents=True, exist_ok=False)
    original = source.read_text()
    cases = {
        'conditional_barrier': original.replace('__syncthreads();', 'if (tid == 0) __syncthreads();', 1),
        'nonuniform_barrier_loop': original.replace('step <= steps_cuda;', 'step <= steps_cuda - tid;', 1),
        'early_return_with_barriers': original.replace('const unsigned tid = threadIdx.x;', 'const unsigned tid = threadIdx.x; if (tid == 0) return;', 1),
        'loop_counter_modified': original.replace('const unsigned row = cell / 32;', 'step += tid; const unsigned row = cell / 32;', 1),
        'pointer_arithmetic': original.replace('*src = state_a;', '*src = state_a + 1;', 1),
        'unsupported_float': original.replace('std::int32_t acc = 0;', 'float acc = 0;', 1),
        'external_include': '#include "untracked_device_code.cuh"\n' + original,
        'preprocessing': '#define steps_cuda 1\n' + original,
    }
    results = []
    for name, text in cases.items():
        if text == original:
            raise RuntimeError('Negative fixture was not changed: ' + name)
        fixture = evidence / (name + '.cu')
        fixture.write_text(text)
        try:
            compile_kernel(fixture, 'pom_v4_walk_kernel', cache, 128, True)
        except Unsupported as error:
            results.append({'case': name, 'status': 'REJECTED', 'reason': str(error)})
        else:
            raise RuntimeError('Unsupported source accepted: ' + name)
    # Valid CUDA dimensions may still be incompatible with this host fixture.
    # Reflection must prevent dispatch, rather than letting the shader write
    # beyond buffers allocated for a different number of steps.
    from walk_probe import validate_manifest
    shape = evidence / 'changed-dimensions.cu'
    shape.write_text(original.replace('steps_cuda = 256;', 'steps_cuda = 512;', 1))
    compiled = compile_kernel(shape, 'pom_v4_walk_kernel', cache, 128, True)
    try:
        validate_manifest(compiled)
    except ValueError as error:
        results.append({'case': 'fixture_dimensions_mismatch', 'status': 'REJECTED_BEFORE_DISPATCH', 'reason': str(error)})
    else:
        raise RuntimeError('Changed dimensions admitted by fixed host fixture')
    (evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps({'frontend_rejections': 'PASS', 'cases': len(results), 'gpu_used': False}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'consensus/cuda_walk_kernels.cu')
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    for path in (args.cache, args.evidence):
        resolved = path.resolve()
        if any(resolved == p or p in resolved.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
            parser.error('Use persistent storage outside temporary directories')
    run(args.source.resolve(), args.cache.resolve(), args.evidence.resolve())
