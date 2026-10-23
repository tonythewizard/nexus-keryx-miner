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
        return [sys.executable, str(ROOT / 'tools/package_release.py'), '--platform', 'windows',
                '--worker-dir', str(self.root / 'worker'), '--oracle', str(self.root / 'oracle.exe'),
                '--runtime-dir', str(self.root / 'engine'), '--cuda-dir', str(self.root / 'cuda/bin'),
                '--output', str(self.root / 'out')]

    def test_archive_allowlist_and_checksums(self):
        subprocess.run(self.package_inputs(), check=True, capture_output=True)
        archive = next((self.root / 'out').glob('*.zip'))
        with zipfile.ZipFile(archive) as packed:
            entries = {Path(name).name: packed.read(name) for name in packed.namelist()}
            self.assertNotIn('private-wallet.key', entries)
            self.assertNotIn('DO-NOT-PACK.key', entries)
            manifest = json.loads(entries['BUILD.json'])
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

    def test_missing_runtime_blocks_archive(self):
        args = self.package_inputs()
        (self.root / 'cuda/bin/cublasLt64_13.dll').unlink()
        result = subprocess.run(args, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.root / 'out').glob('*.zip')))

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
