import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nexus_run', ROOT / 'tools/nexus_run.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class Release(unittest.TestCase):
    def setUp(self):
        (ROOT / '.local/tmp').mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def package_inputs(self):
        for name in ('worker', 'engine/licenses', 'cuda/bin', 'out'):
            (self.root / name).mkdir(parents=True)
        for name in ('worker/nexus-live-cuda-worker.exe', 'oracle.exe',
                     'engine/keryx-llama.dll', 'engine/licenses/NOTICE',
                     'cuda/EULA.txt', 'cuda/bin/cudart64_13.dll',
                     'cuda/bin/cublas64_13.dll', 'cuda/bin/cublasLt64_13.dll',
                     'worker/private-wallet.key', 'engine/DO-NOT-PACK.key'):
            (self.root / name).write_text('synthetic packaging fixture')
        (self.root / 'engine/NEXUS_RUNTIME.json').write_text(json.dumps({'application_abi': 8, 'candidate_only': False, 'cuda_architectures': ['sm_86', 'sm_89', 'sm_120']}))
        return [sys.executable, str(ROOT / 'tools/package_release.py'), '--platform', 'windows',
                '--worker-dir', str(self.root / 'worker'), '--oracle', str(self.root / 'oracle.exe'),
                '--runtime-dir', str(self.root / 'engine'), '--cuda-dir', str(self.root / 'cuda/bin'),
                '--output', str(self.root / 'out')]

    def test_old_or_partial_architecture_runtime_is_refused(self):
        args = self.package_inputs()
        for architectures in (None, ['sm_86'], ['sm_86', 'sm_89']):
            info = {'application_abi': 8, 'candidate_only': False}
            if architectures is not None:
                info['cuda_architectures'] = architectures
            (self.root / 'engine/NEXUS_RUNTIME.json').write_text(json.dumps(info))
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('matching RTX 30/40/50', result.stderr)
            self.assertFalse(list((self.root / 'out').glob('*')))

    def test_missing_runtime_sidecar_is_refused_before_packaging(self):
        args = self.package_inputs()
        (self.root / 'engine/NEXUS_RUNTIME.json').unlink()
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('runtime ABI sidecar is required', result.stderr)
        self.assertFalse(list((self.root / 'out').glob('*')))

    def test_abi9_candidate_is_refused_before_packaging(self):
        args = self.package_inputs()
        (self.root / 'engine/NEXUS_RUNTIME.json').write_text(json.dumps({'application_abi': 9, 'candidate_only': True}))
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('explicitly selected worker runtime ABI', result.stderr)
        self.assertFalse(list((self.root / 'out').glob('*')))

    def test_abi8_runtime_sidecar_is_retained_and_identified(self):
        args = self.package_inputs()
        info = {'application_abi': 8, 'candidate_only': False, 'cuda_architectures': ['sm_86', 'sm_89', 'sm_120']}
        (self.root / 'engine/NEXUS_RUNTIME.json').write_text(json.dumps(info))
        subprocess.run(args, check=True, capture_output=True)
        with zipfile.ZipFile(next((self.root / 'out').glob('*.zip'))) as packed:
            files = {Path(name).name: packed.read(name) for name in packed.namelist()}
            self.assertEqual(json.loads(files['INFERENCE_RUNTIME.json']), info)
            self.assertEqual(json.loads(files['BUILD.json'])['inference_application_abi'], 8)

    def test_explicit_abi9_preserves_experimental_metadata(self):
        args = self.package_inputs() + ['--runtime-abi', '9']
        info = {'application_abi': 9, 'candidate_only': True,
                'cuda_architectures': ['sm_86', 'sm_89', 'sm_120'],
                'patch_set_sha256': 'a' * 64, 'wrapper_sha256': 'b' * 64}
        (self.root / 'engine/NEXUS_RUNTIME.json').write_text(json.dumps(info))
        subprocess.run(args, check=True, capture_output=True)
        with zipfile.ZipFile(next((self.root / 'out').glob('*.zip'))) as packed:
            files = {Path(name).name: packed.read(name) for name in packed.namelist()}
            self.assertEqual(json.loads(files['INFERENCE_RUNTIME.json']), info)
            build = json.loads(files['BUILD.json'])
            self.assertEqual(build['inference_application_abi'], 9)
            self.assertTrue(build['inference_runtime_candidate_only'])
            self.assertEqual(build['inference_patch_set_sha256'], 'a' * 64)

    def test_explicit_abi9_rejects_abi8_cache(self):
        args = self.package_inputs() + ['--runtime-abi', '9']
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.root / 'out').glob('*')))

    def test_archive_allowlist_and_checksums(self):
        subprocess.run(self.package_inputs(), check=True, capture_output=True)
        archive = next((self.root / 'out').glob('*.zip'))
        with zipfile.ZipFile(archive) as packed:
            entries = {Path(name).name: packed.read(name) for name in packed.namelist()}
            self.assertNotIn('private-wallet.key', entries)
            self.assertNotIn('DO-NOT-PACK.key', entries)
            manifest = json.loads(entries['BUILD.json'])
            self.assertEqual(manifest['cuda_architectures'], ['sm_86', 'sm_89', 'sm_120'])
            self.assertFalse(manifest['gpu_execution_validated'])
            self.assertFalse(manifest['h15_complete'])
            self.assertEqual(len(manifest['commit']), 40)
            for line in entries['SHA256SUMS'].decode().splitlines():
                digest, name = line.split('  ', 1)
                prefix = packed.namelist()[0].split('/')[0]
                self.assertEqual(hashlib.sha256(packed.read(prefix + '/' + name)).hexdigest(), digest)
        self.assertEqual(archive.with_name(archive.name + '.sha256').read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())

    def test_explicit_installed_cuda_license(self):
        args = self.package_inputs()
        installed = self.root / 'installed-cuda-copyright'
        (self.root / 'cuda/EULA.txt').rename(installed)
        subprocess.run(args + ['--cuda-license', str(installed)], check=True, capture_output=True)
        self.assertEqual(len(list((self.root / 'out').glob('*.zip'))), 1)

    def test_cuda13_windows_x64_layout_and_toolkit_root_license(self):
        args = self.package_inputs()
        binaries = self.root / 'cuda/bin'
        (binaries / 'x64').mkdir()
        for library in binaries.glob('*.dll'):
            library.rename(binaries / 'x64' / library.name)
        # Driver DLLs and unrequested toolkit components remain excluded.
        for name in ('nvcuda.dll', 'nvrtc64_130_0.dll'):
            (binaries / 'x64' / name).write_bytes(b'excluded fixture')
        args[args.index('--cuda-dir') + 1] = str(binaries / 'x64')
        subprocess.run(args + ['--cuda-license', str(self.root / 'cuda/EULA.txt')],
                       check=True, capture_output=True)
        archive = next((self.root / 'out').glob('*.zip'))
        with zipfile.ZipFile(archive) as packed:
            dlls = {Path(name).name for name in packed.namelist() if name.endswith('.dll')}
            self.assertEqual(dlls, {'keryx-llama.dll', 'cudart64_13.dll',
                                   'cublas64_13.dll', 'cublasLt64_13.dll'})
            license_name = next(name for name in packed.namelist()
                                if name.endswith('/licenses/NVIDIA-CUDA-EULA.txt'))
            self.assertEqual(packed.read(license_name), (self.root / 'cuda/EULA.txt').read_bytes())

    def test_missing_runtime_blocks_archive(self):
        args = self.package_inputs()
        (self.root / 'cuda/bin/cublasLt64_13.dll').unlink()
        result = subprocess.run(args, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.root / 'out').glob('*.zip')))

    def test_cuda_pdf_license_is_preserved_without_installed_eula(self):
        args = self.package_inputs()
        (self.root / 'cuda/EULA.txt').unlink()
        license_file = self.root / 'EULA.pdf'
        pdf = b'%PDF-1.5\nsynthetic license fixture\n%%EOF\n'
        license_file.write_bytes(pdf)
        subprocess.run(args + ['--cuda-license', str(license_file)], check=True, capture_output=True)
        archive = next((self.root / 'out').glob('*.zip'))
        with zipfile.ZipFile(archive) as packed:
            license_name = next(name for name in packed.namelist()
                                if name.endswith('/licenses/NVIDIA-CUDA-EULA.pdf'))
            self.assertEqual(packed.read(license_name), pdf)
            self.assertFalse(any(name.endswith('/licenses/NVIDIA-CUDA-EULA.txt')
                                 for name in packed.namelist()))
            sums = packed.read(next(name for name in packed.namelist()
                                    if name.endswith('/SHA256SUMS'))).decode()
            self.assertIn(hashlib.sha256(pdf).hexdigest() + '  licenses/NVIDIA-CUDA-EULA.pdf', sums)

    def test_missing_cuda_license_blocks_archive(self):
        args = self.package_inputs()
        (self.root / 'cuda/EULA.txt').unlink()
        result = subprocess.run(args, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.root / 'out').glob('*.zip')))

    def test_pre_1980_dependency_notice_keeps_contents_and_checksums(self):
        args = self.package_inputs()
        # Real timestamp in the locked chacha20poly1305 0.10.1 crate archive.
        relative = 'licenses/rust/chacha20poly1305-0.10.1/LICENSE-APACHE'
        notice = self.root / 'engine' / relative
        notice.parent.mkdir(parents=True)
        contents = b'synthetic dependency notice\n'
        notice.write_bytes(contents)
        os.utime(notice, (123456789, 123456789))
        result = subprocess.run(args, check=True, capture_output=True, text=True)
        self.assertIn(relative, result.stdout)
        archive = next((self.root / 'out').glob('*.zip'))
        with zipfile.ZipFile(archive) as packed:
            name = next(name for name in packed.namelist() if name.endswith('/' + relative))
            self.assertEqual(packed.getinfo(name).date_time, (1980, 1, 1, 0, 0, 0))
            self.assertEqual(packed.read(name), contents)
            sums = packed.read(next(name for name in packed.namelist()
                                    if name.endswith('/SHA256SUMS'))).decode()
            self.assertIn(hashlib.sha256(contents).hexdigest() + '  ' + relative, sums)
        self.assertEqual(archive.with_name(archive.name + '.sha256').read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())

    def test_launcher_requires_profile_and_explicit_live_authorization(self):
        ext = '.exe' if os.name == 'nt' else ''
        for name in ('nexus-live-cuda-worker' + ext, 'nexus-m21-oracle' + ext,
                     'keryx-llama.dll' if os.name == 'nt' else 'libkeryx-llama.so'):
            (self.root / name).touch()
        profile = json.loads((ROOT / 'tools/windows/profile.example.json').read_text())
        for key in ('data_dir', 'model', 'tree', 'metadata', 'escrow_key', 'output'):
            profile[key] = str(self.root / key)
        profile.update(payout='synthetic-not-a-wallet', delegation='synthetic-not-a-certificate')
        with self.assertRaises(ValueError):
            launcher.launch_command(profile, self.root)
        # Repeated synthetic key material; never a physical machine identity.
        profile['gpu_uuid'] = 'GPU-' + '-'.join('1' * n for n in (8, 4, 4, 4, 12))
        with patch.dict(os.environ, {'NEXUS_SECONDARY_VULKAN_MODE': 'resident-live'}):
            args, env = launcher.launch_command(profile, self.root)
        self.assertIn('--no-submit', args)
        self.assertIn('--no-ai-response', args)
        self.assertNotIn('NEXUS_SECONDARY_VULKAN_MODE', env)
        self.assertEqual(env['NEXUS_POM_BACKEND'], 'cuda-driver')
        self.assertEqual(env['CUDA_VISIBLE_DEVICES'], profile['gpu_uuid'])
        args, _ = launcher.launch_command(profile, self.root, live=True)
        self.assertIn('--enable-submit', args)
        self.assertIn('--enable-ai-response', args)
        self.assertIn('--no-escrow-claims', args)
        with self.assertRaises(ValueError):
            launcher.launch_command(profile, self.root, claims=True)


if __name__ == '__main__':
    unittest.main()
