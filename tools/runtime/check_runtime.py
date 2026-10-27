#!/usr/bin/env python3
"""Check a built wrapper ABI and null-handle boundaries without loading a model."""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import subprocess
import sys

WRAPPER_EXPORTS = (
    'abi', 'last_error', 'load', 'tensor_count', 'context_acquire', 'context_release',
    'context_info', 'tensor_info', 'tensor_device', 'generate', 'generate_traced',
    'trace_tokens', 'trace_configure', 'trace_leaf_count', 'trace_read', 'replay',
    'replay_multi', 'probe_device', 'free',
)
CANONICAL_EXPORTS = (
    'configure', 'release', 'leaf_count', 'begin_response', 'arm_capture', 'read',
    'read_capture', 'arm_captures', 'read_captures',
)
SEQUENCE_EXPORTS = ('configure', 'release', 'begin_response', 'roots', 'arm', 'read_captures')


def check(path, expected):
    if path.is_symlink() or not path.is_file():
        raise ValueError('runtime must be a regular build output')
    runtime = c.CDLL(str(path.resolve(strict=True)))
    abi = runtime.keryx_llama_abi
    abi.argtypes = []
    abi.restype = c.c_int
    actual = abi()
    if actual != expected:
        raise ValueError(f'expected ABI {expected}, runtime reports {actual}')
    names = ['keryx_llama_' + name for name in WRAPPER_EXPORTS]
    canonical = ['ggml_cuda_q8c_' + name for name in CANONICAL_EXPORTS]
    sequence = ['ggml_cuda_q8s_' + name for name in SEQUENCE_EXPORTS] if expected == 9 else []
    if expected == 9:
        names += ['keryx_llama_seq_configure', 'keryx_llama_seq_roots', 'keryx_llama_seq_replay']
    for name in names + canonical + sequence:
        getattr(runtime, name)
    # These calls return before touching a CUDA context. Device probes and model
    # load/generation/replay with a real handle are deliberately separate tests.
    null_checks = 0
    if expected == 9:
        configure = runtime.keryx_llama_seq_configure
        configure.argtypes = [c.c_void_p, c.POINTER(c.c_uint32)] + [c.c_uint32] * 5
        configure.restype = c.c_int
        if configure(None, None, 1, 0, 0, 0, 0) != -1:
            raise ValueError('sequence configure accepted a null engine')
        null_checks += 1
        roots = runtime.keryx_llama_seq_roots
        roots.argtypes = [c.c_void_p, c.POINTER(c.c_uint8), c.c_size_t, c.POINTER(c.c_uint32)]
        roots.restype = c.c_int
        out = (c.c_uint8 * 32)(*([0xCC] * 32))
        count = c.c_uint32(0x12345678)
        if roots(None, out, len(out), c.byref(count)) != -1:
            raise ValueError('sequence roots accepted a null engine')
        if bytes(out) != b'\xcc' * 32 or count.value != 0x12345678:
            raise ValueError('sequence roots changed a rejected call output')
        null_checks += 1
        replay = runtime.keryx_llama_seq_replay
        replay.argtypes = [c.c_void_p, c.POINTER(c.c_int32), c.c_int, c.c_int,
                           c.POINTER(c.c_uint32), c.POINTER(c.c_uint64), c.POINTER(c.c_uint64),
                           c.c_uint32, c.c_uint32, c.POINTER(c.c_uint8), c.c_size_t,
                           c.POINTER(c.c_uint64), c.POINTER(c.c_uint8), c.c_size_t, c.POINTER(c.c_uint32)]
        replay.restype = c.c_int
        if replay(None, None, 0, 0, None, None, None, 0, 0, out, len(out), None,
                  out, len(out), c.byref(count)) != -1:
            raise ValueError('sequence replay accepted a null engine')
        if bytes(out) != b'\xcc' * 32 or count.value != 0x12345678:
            raise ValueError('sequence replay changed a rejected call output')
        null_checks += 1
    # Dynamic visibility alone could resolve a transitive import. On Linux verify
    # the exact C symbols are defined in this built library, without conflating
    # them with extra legitimate C++-mangled names containing the same substring.
    if sys.platform.startswith('linux'):
        symbols = subprocess.run(['nm', '-D', '--defined-only', str(path)],
                                 check=True, text=True, capture_output=True)
        defined = {line.split()[-1] for line in symbols.stdout.splitlines() if line.split()}
        missing = set(names + canonical + sequence) - defined
        if missing:
            raise ValueError('runtime lacks defined C symbols: ' + ', '.join(sorted(missing)))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'application_abi': actual, 'wrapper_exports': len(names),
            'canonical_c_exports': len(canonical), 'sequence_c_exports': len(sequence),
            'null_handle_checks': null_checks, 'sha256': digest,
            'model_loaded': False, 'gpu_execution_validated': False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--expected-abi', type=int, choices=(8, 9), default=9)
    args = parser.parse_args()
    print(json.dumps(check(args.runtime, args.expected_abi), indent=2))


if __name__ == '__main__':
    main()
