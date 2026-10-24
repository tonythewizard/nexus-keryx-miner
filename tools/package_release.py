#!/usr/bin/env python3
"""Package reviewed build outputs only; installation data is never traversed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', required=True, choices=('windows', 'linux'))
    parser.add_argument('--worker-dir', required=True, type=Path)
    parser.add_argument('--oracle', required=True, type=Path)
    parser.add_argument('--runtime-dir', required=True, type=Path)
    parser.add_argument('--cuda-dir', required=True, type=Path)
    parser.add_argument('--crt-dir', type=Path)
    parser.add_argument('--cuda-license', type=Path, help='Installed CUDA license when not at toolkit/EULA.txt')
    parser.add_argument('--inspector', type=Path, help='dumpbin.exe on Windows; readelf on Linux')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    windows = args.platform == 'windows'
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        parser.error('invalid source version')
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    name = f'nexus-keryx-miner-{version}-{args.platform}-x86_64-cuda-sm86-preview'
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='package-', dir=args.output) as temporary:
        package = Path(temporary) / name
        package.mkdir()
        def copy(source, destination=None):
            if source.is_symlink() or not source.is_file():
                raise ValueError(f'Required build output missing or symlink: {source.name}')
            target = package / (destination or source.name)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        ext = '.exe' if windows else ''
        copy(args.worker_dir / ('nexus-live-cuda-worker' + ext))
        copy(args.oracle, 'nexus-m21-oracle' + ext)
        copy(args.runtime_dir / ('keryx-llama.dll' if windows else 'libkeryx-llama.so'))
        # Explicit redistributable runtime names; never copy driver stubs,
        # models, wallet files or a user's installation directory wholesale.
        patterns = ('cudart64_*.dll', 'cublas64_*.dll', 'cublasLt64_*.dll') if windows else (
            'libcudart.so.*', 'libcublas.so.*', 'libcublasLt.so.*')
        for pattern in patterns:
            libraries = sorted(p for p in args.cuda_dir.glob(pattern) if p.is_file() and not p.is_symlink())
            if len(libraries) != 1:
                raise ValueError(f'Expected one CUDA runtime for {pattern} in {args.cuda_dir}; found {len(libraries)}')
            destination = None
            if not windows:
                destination = re.match(r'(lib.+\.so\.\d+)', libraries[0].name).group(1)
            copy(libraries[0], destination)
        if windows and args.crt_dir:
            for pattern in ('msvcp140*.dll', 'vcruntime140*.dll', 'concrt140.dll'):
                for library in args.crt_dir.glob(pattern):
                    copy(library)
        for path in ('LICENSE', 'THIRD_PARTY.md', 'VERSION', 'docs/windows.md', 'tools/nexus_run.py'):
            copy(ROOT / path, Path(path).name)
        copy(ROOT / 'tools/windows/profile.example.json', 'profile.example.json')
        copy(args.cuda_license or args.cuda_dir.parent / 'EULA.txt', 'licenses/NVIDIA-CUDA-EULA.txt')
        # Dependency notices are installed alongside the runtime by CMake.
        notices = args.runtime_dir / 'licenses'
        if not notices.is_dir():
            raise ValueError('Dependency license directory missing')
        for notice in notices.rglob('*'):
            if notice.is_file():
                copy(notice, Path('licenses') / notice.relative_to(notices))
        if args.inspector:
            from distribution_dependencies import verify
            verify(package, args.platform, args.inspector)
        (package / 'BUILD.json').write_text(json.dumps({
            'version': version, 'commit': commit,
            'source': f'https://github.com/tonythewizard/nexus-keryx-miner/tree/{commit}',
            'platform': args.platform, 'architecture': 'x86_64', 'cuda_architecture': 'sm_86',
            'inference_source': 'https://github.com/ggml-org/llama.cpp/tree/12127defda4f41b7679cb2477a4b0d65ee6a0c8f',
            'inference_patch_sha256': hashlib.sha256((ROOT / 'vendor/keryx-trace-abi6/tools/keryx-llama/patches/0001-canonical-trace.patch').read_bytes()).hexdigest(),
            'dependency_imports_checked': bool(args.inspector),
            'status': 'experimental-native-cli', 'gpu_execution_validated': False,
            'h15_complete': False, 'desktop_included': False,
        }, indent=2) + '\n')
        sums = []
        for file in sorted(package.rglob('*')):
            if file.is_file():
                sums.append(f'{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.relative_to(package).as_posix()}')
        (package / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
        if windows:
            archive = args.output / (name + '.zip')
            with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
                for file in sorted(package.rglob('*')):
                    if file.is_file():
                        output.write(file, file.relative_to(package.parent).as_posix())
        else:
            archive = args.output / (name + '.tar.gz')
            with tarfile.open(archive, 'w:gz') as output:
                output.add(package, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'Created {archive.name}; source commit {commit}')


if __name__ == '__main__':
    main()
