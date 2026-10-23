#!/usr/bin/env python3
"""Serial, isolated diagnostic build reusing a pinned native Vulkan build's archives.

No GPU initialization. Run inside a monitored build cgroup with persistent TMPDIR.
The dispatcher uses -O0 to keep this correctness fixture's compiler footprint low;
the existing SPIR-V is reused, except the six explicitly corrected K-quant variants.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from prepare_q8_probe import instrument
from build_canonical_q8_k import build as build_k
from clang_dispatcher import embed_shader


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build(llama, existing, output, canonical):
    for name in ('TMPDIR', 'TMP', 'TEMP'):
        path = Path(os.environ.get(name, ''))
        if not path.is_absolute() or not path.is_dir() or path == Path('/tmp') or Path('/tmp') in path.resolve().parents:
            raise ValueError('Explicit persistent temporary directories are required')
    if output.exists(): raise ValueError('A new separate build output is required')
    dispatcher = llama / 'ggml/src/ggml-vulkan/ggml-vulkan.cpp'
    helpers = Path(__file__).parent
    # Verify the source before creating any artifacts.
    prepared = instrument(dispatcher.read_text(), (helpers / 'q8_probe_backend.inc').read_text())
    gpu = existing / 'llama/ggml/src/ggml-vulkan'
    flags = {}
    for line in (gpu / 'CMakeFiles/ggml-vulkan.dir/flags.make').read_text().splitlines():
        if line.startswith('CXX_'):
            key, value = line.split(' = ', 1); flags[key] = shlex.split(value)
    output.mkdir(parents=True)
    source = output / 'ggml-vulkan-probe.cpp'; source.write_text(prepared)
    commands = []

    def run(command):
        commands.append([str(arg) for arg in command])
        with (output / f'build-{len(commands)}.log').open('w') as log:
            subprocess.run(commands[-1], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=180)

    run(['clang++', *flags['CXX_DEFINES'], *flags['CXX_INCLUDES'], '-I' + str(dispatcher.parent),
         *[flag for flag in flags['CXX_FLAGS'] if not flag.startswith('-O')], '-O0',
         '-c', source, '-o', output / 'ggml-vulkan.cpp.o'])
    run(['clang++', '-std=c++17', '-O1', '-g0', '-I' + str(llama / 'ggml/include'),
         '-c', helpers / 'q8_numeric_probe.cpp', '-o', output / 'probe.o'])
    archive = gpu / 'libggml-vulkan.a'
    original_archive = digest(archive)
    if canonical:
        corrected = output / 'canonical-k'
        build_k(dispatcher.parent / 'vulkan-shaders', gpu / 'mul_mat_vecq.comp.cpp', corrected)
        obj = corrected / 'mul_mat_vecq.comp.cpp.o'
        embed_shader(corrected / 'mul_mat_vecq.comp.cpp', ['-o', str(obj)])
        archive = corrected / 'libggml-vulkan.a'
        shutil.copyfile(gpu / 'libggml-vulkan.a', archive)
        run(['ar', 'r', archive, obj])
    base = existing / 'llama/ggml/src'
    executable = output / 'nexus-vulkan-q8-numeric-probe'
    run(['clang++', '-Wl,--no-keep-memory', output / 'probe.o', output / 'ggml-vulkan.cpp.o', archive,
         base / 'libggml.a', base / 'libggml-cpu.a', base / 'libggml-base.a',
         '-lvulkan', '-ldl', '-lm', '-lgomp', '-lpthread', '-o', executable])
    if digest(gpu / 'libggml-vulkan.a') != original_archive:
        raise RuntimeError('Original Vulkan archive changed during the diagnostic build')
    manifest = {'executable_sha256': digest(executable), 'pinned_dispatcher_sha256': digest(dispatcher),
                'original_vulkan_archive_sha256': original_archive, 'canonical_k_correction': canonical,
                'commands': commands, 'production_runtime_modified': False, 'gpu_initialized': False}
    (output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(manifest['executable_sha256'] + '  ' + executable.name + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('llama_source', type=Path)
    parser.add_argument('existing_build', type=Path)
    parser.add_argument('new_output', type=Path)
    parser.add_argument('--canonical-k', action='store_true')
    args = parser.parse_args()
    build(args.llama_source.resolve(), args.existing_build.resolve(), args.new_output.resolve(), args.canonical_k)
