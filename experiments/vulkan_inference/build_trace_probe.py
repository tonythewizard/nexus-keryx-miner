#!/usr/bin/env python3
"""Incrementally build the offline full-Qwen capture fixture, never the worker.

Run serially in a monitored build cgroup. The input archive is the separately
validated canonical-Q8 archive. The original llama tree and archives stay intact.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from build_q8_numeric_probe import digest
from prepare_q8_probe import instrument
from prepare_bounded_upload import prepare

ARCHIVE = '7c43ae6449bd9cae4f451993b2e968f1a98a5ebc032d349f1201360d0fedc746'


def flags(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('CXX_'):
            key, value = line.split(' = ', 1); result[key] = shlex.split(value)
    return result['CXX_DEFINES'] + result['CXX_INCLUDES'] + [f for f in result['CXX_FLAGS'] if not f.startswith('-O')]


def build(llama, existing, canonical, output):
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        directory = Path(os.environ.get(key, ''))
        if not directory.is_absolute() or not directory.is_dir() or directory.resolve() == Path('/tmp') or Path('/tmp') in directory.resolve().parents:
            raise ValueError('Explicit persistent temporary directories required')
    if output.exists() or digest(canonical) != ARCHIVE:
        raise ValueError('New output and the attested canonical Vulkan archive required')
    source = llama / 'ggml/src/ggml-vulkan/ggml-vulkan.cpp'
    loader = llama / 'src/llama-model-loader.cpp'
    here = Path(__file__).parent
    prepared = instrument(source.read_text(), (here / 'q8_probe_backend.inc').read_text())
    bounded = prepare(loader.read_text())
    output.mkdir(parents=True)
    (output / 'ggml-vulkan-trace.cpp').write_text(prepared)
    (output / 'llama-model-loader-bounded.cpp').write_text(bounded)
    commands = []

    def run(command):
        commands.append([str(v) for v in command])
        with (output / f'build-{len(commands)}.log').open('w') as log:
            subprocess.run(commands[-1], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)

    base = existing / 'llama/ggml/src'
    original_llama = existing / 'llama/src/libllama.a'
    before = digest(original_llama)
    run(['clang++', *flags(base / 'ggml-vulkan/CMakeFiles/ggml-vulkan.dir/flags.make'), '-O0', '-g0',
         '-I' + str(source.parent), '-c', output / 'ggml-vulkan-trace.cpp', '-o', output / 'ggml-vulkan.cpp.o'])
    # Use the original GNU compiler for the loader's cross-object SFINAE templates.
    # Its symbol mangling differs from Clang for this pinned loader.
    run(['/usr/bin/c++', *flags(existing / 'llama/src/CMakeFiles/llama.dir/flags.make'), '-O1', '-g0',
         '-c', output / 'llama-model-loader-bounded.cpp', '-o', output / 'bounded-loader.o'])
    archive = output / 'libllama-bounded.a'; shutil.copyfile(original_llama, archive)
    run(['ar', 'd', archive, 'llama-model-loader.cpp.o'])
    run(['ar', 'r', archive, output / 'bounded-loader.o'])
    run(['clang++', '-std=c++20', '-O1', '-g0', '-I' + str(llama / 'include'), '-I' + str(llama / 'ggml/include'),
         '-I' + str(here.parents[1] / 'include'), '-c', here / 'trace_probe.cpp', '-o', output / 'trace_probe.o'])
    executable = output / 'nexus-vulkan-trace-probe'
    run(['clang++', '-Wl,--no-keep-memory', output / 'trace_probe.o', archive, output / 'ggml-vulkan.cpp.o', canonical,
         base / 'libggml.a', canonical, base / 'libggml-cpu.a', base / 'libggml-base.a',
         '-lvulkan', '-ldl', '-lm', '-lgomp', '-lpthread', '-lcrypto', '-o', executable])
    if before != digest(original_llama): raise RuntimeError('Original llama archive changed')
    result = {'executable_sha256': digest(executable), 'original_llama_archive_sha256': before,
              'canonical_vulkan_archive_sha256': ARCHIVE, 'commands': commands,
              'model_loaded': False, 'worker_modified': False, 'mainnet_transactions': 0}
    (output / 'build-manifest.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('llama_source', 'existing_build', 'canonical_vulkan_archive', 'new_output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    build(*(getattr(args, name).resolve() for name in ('llama_source', 'existing_build', 'canonical_vulkan_archive', 'new_output')))
