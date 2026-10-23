#!/usr/bin/env python3
"""Build the pinned official ABI6 wrapper from copied CUDA12 b10015 archives.

No GPU devices are passed to the build container. Original archives, the stock
wrapper and production configuration are never modified. Inputs must already
be copied into WORK/target-h6-container/{llama-src, llama-build}-b10015.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

PIN = '12127defda4f41b7679cb2477a4b0d65ee6a0c8f'
IMAGE = 'docker.io/nvidia/cuda:12.2.2-devel-ubuntu22.04'


def digest(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path)
    args = parser.parse_args()
    work = args.work.resolve()
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        path = Path(os.environ.get(key, ''))
        if not path.is_absolute() or not path.is_dir() or '/tmp' == str(path.resolve()) or str(path.resolve()).startswith('/tmp/'):
            raise ValueError('Persistent explicit temporary paths required')
    root = Path(__file__).resolve().parents[1]
    image_id = subprocess.check_output(['podman', 'image', 'inspect', '--format', '{{.Id}}', IMAGE], text=True).strip()
    src = work / 'target-h6-container/llama-src-b10015'
    build = work / 'target-h6-container/llama-build-b10015'
    if subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise ValueError('llama.cpp b10015 pin mismatch')
    patch = root / 'vendor/keryx-trace-abi6/tools/keryx-llama/patches/0001-canonical-trace.patch'
    if not (src / 'ggml/src/ggml-cuda/q8canon.cu').exists():
        subprocess.run(['git', '-C', str(src), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(src), 'apply', str(patch)], check=True)
    else:
        subprocess.run(['git', '-C', str(src), 'apply', '--reverse', '--check', str(patch)], check=True)
    output = work / 'abi6'
    output.mkdir(exist_ok=True)
    (work / 'tmp').mkdir(exist_ok=True)
    definitions = {}
    for line in (build / 'ggml/src/ggml-cuda/CMakeFiles/ggml-cuda.dir/flags.make').read_text().splitlines():
        if line.startswith('CUDA_'):
            key, value = line.split(' = ', 1)
            definitions[key] = shlex.split(value)
    flags = definitions['CUDA_DEFINES'] + definitions['CUDA_INCLUDES'] + definitions['CUDA_FLAGS']
    if 'compute_86' not in ' '.join(flags):
        raise ValueError('Expected SM86 archives')
    container = ['podman', 'run', '--rm', '--network=none', '--memory=2g', '--memory-swap=2g', '--cpus=1',
                 '--env', 'TMPDIR=/src/tmp', '--env', 'TMP=/src/tmp', '--env', 'TEMP=/src/tmp',
                 '--volume', f'{work}:/src', '--volume', f'{root}:/nexus:ro', image_id]
    commands = []

    def run(command):
        commands.append(command)
        print('RUN', shlex.join(command), flush=True)
        subprocess.run(container + command, check=True)

    changed = ['ggml-cuda', 'mmq', 'mmvq', 'q8canon']
    for name in changed:
        source = src / f'ggml/src/ggml-cuda/{name}.cu'
        obj = output / f'{name}.cu.o'
        stamp = output / f'{name}.json'
        key = {'source': digest(source), 'patch': digest(patch), 'flags': flags, 'image_id': image_id}
        if obj.exists() and stamp.exists() and json.loads(stamp.read_text()) == key:
            continue
        run(['/usr/local/cuda/bin/nvcc', *flags, '-c', f'/src/target-h6-container/llama-src-b10015/ggml/src/ggml-cuda/{name}.cu',
             '-o', f'/src/abi6/{name}.cu.o'])
        stamp.write_text(json.dumps(key, indent=2) + '\n')
    original = build / 'ggml/src/ggml-cuda/libggml-cuda.a'
    before = digest(original)
    import shutil
    shutil.copyfile(original, output / 'libggml-cuda.a')
    run(['ar', 'r', '/src/abi6/libggml-cuda.a', *[f'/src/abi6/{n}.cu.o' for n in changed]])
    prefix = '/src/target-h6-container/llama-src-b10015'
    base = '/src/target-h6-container/llama-build-b10015'
    run(['g++', '-O2', '-g0', '-std=c++17', '-march=nehalem', '-shared', '-fPIC', '-fopenmp',
         '/nexus/vendor/keryx-trace-abi6/tools/keryx-llama/keryx_llama.cpp',
         *[f'-I{prefix}/{p}' for p in ('include', 'ggml/include', 'src', 'common')], '-I/usr/local/cuda/include',
         '-Wl,--no-keep-memory', '-Wl,--start-group', f'{base}/src/libllama.a', '/src/abi6/libggml-cuda.a',
         f'{base}/ggml/src/libggml-cpu.a', f'{base}/ggml/src/libggml.a', f'{base}/ggml/src/libggml-base.a',
         '-Wl,--end-group', '-L/usr/local/cuda/lib64', '-L/usr/local/cuda/lib64/stubs',
         '-lcudart', '-lcublas', '-lcublasLt', '-lcuda', '-lpthread', '-ldl', '-Wl,-rpath,$ORIGIN',
         '-o', '/src/abi6/libkeryx-llama.so'])
    assert digest(original) == before
    manifest = {'llama_commit': PIN, 'miner_commit': '686e8691936d48f2931ffc3a23402335202b546c',
                'image': IMAGE, 'image_id': image_id, 'patch_sha256': digest(patch), 'base_cuda_archive_sha256': before,
                'wrapper_source_sha256': digest(root / 'vendor/keryx-trace-abi6/tools/keryx-llama/keryx_llama.cpp'),
                'sha256': digest(output / 'libkeryx-llama.so'), 'commands': commands,
                'gpu_executed': False, 'production_replaced': False}
    (output / 'BUILD.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('ABI6_WRAPPER_BUILD_COMPLETE', manifest['sha256'], flush=True)


if __name__ == '__main__':
    main()
