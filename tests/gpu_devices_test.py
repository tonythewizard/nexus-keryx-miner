"""CPU-only GPU configuration checks. All writable state uses explicit fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / 'scripts'))
import nexus_gpu_devices as gpu
import nexus_vulkan_runtime as runtime


class Devices(unittest.TestCase):
    def setUp(self):
        directory = Path(os.environ['TMPDIR']) / 'gpu-device-fixtures'
        directory.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=directory)
        self.root = Path(self.tmp.name)
        self.id = 'pci:0000:00:00.0'
        self.devices = [{'id': self.id, 'name': 'fixture AMD', 'driver': 'amdgpu', 'presence': 'PRESENT',
                         'vram_bytes': 8 * 2**30, 'production_ready': False}]
        self.patches = [patch.object(gpu, 'ROOT', self.root), patch.object(gpu, 'STATE', self.root / 'state/gpu-devices'),
                        patch.object(gpu, 'amd_devices', lambda: [dict(x) for x in self.devices])]
        for p in self.patches: p.start()
        gpu.prepare_state()
        gpu.atomic(gpu.STATE / 'capabilities.json', {'schema_version': 1, 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                   'observed_unix': time.time(), 'devices': [{'id': self.id, 'shaderInt64': True, 'shaderInt8': True,
                   'storageBuffer8BitAccess': True, 'max_workgroup_invocations': 1024, 'shared_memory_bytes': 65536}]})

    def tearDown(self):
        for p in reversed(self.patches): p.stop()
        self.tmp.cleanup()

    def test_default_disabled(self):
        value = gpu.status()
        self.assertEqual(value['amd_devices'][0]['mode'], 'disabled')
        self.assertEqual(value['production_amd_admission'], 'BLOCKED')

    def test_mode_persisted_private(self):
        gpu.configure_mode('diagnostics', self.id)
        self.assertEqual(gpu.status()['amd_devices'][0]['mode'], 'diagnostics')
        self.assertEqual((gpu.STATE / 'config.json').stat().st_mode & 0o777, 0o600)
        with self.assertRaises(ValueError): gpu.configure_mode('mining', self.id)

    def test_unknown_device_no_fallback(self):
        with self.assertRaises(ValueError): gpu.configure_mode('diagnostics', 'pci:0000:00:03.0')

    def test_exclusive_lease_and_release(self):
        gpu.configure_mode('diagnostics', self.id)
        with gpu.offline_lease(self.id, 'offline-walk'):
            self.assertTrue(gpu.lease_busy(self.id))
            with self.assertRaises(ValueError):
                with gpu.offline_lease(self.id, 'offline-copy'): pass
            with self.assertRaises(ValueError): gpu.configure_mode('disabled', self.id)
        self.assertFalse(gpu.lease_busy(self.id))
        gpu.configure_mode('disabled', self.id)

    def test_disabled_and_production_roles_blocked(self):
        with self.assertRaises(ValueError):
            with gpu.offline_lease(self.id, 'offline-walk'): pass
        gpu.configure_mode('diagnostics', self.id)
        for name in ('mining', 'inference', 'live', 'submission'):
            with self.assertRaises(ValueError):
                with gpu.offline_lease(self.id, name): pass

    def test_missing_configured_device_reported(self):
        gpu.configure_mode('diagnostics', self.id)
        self.devices.clear()
        self.assertEqual(gpu.status()['missing_configured_devices'], [self.id])

    def test_bad_config_not_accepted(self):
        gpu.atomic(gpu.STATE / 'config.json', {'schema_version': 1, 'devices': {self.id: {'mode': 'mining', 'backend': 'vulkan-jit'}}})
        with self.assertRaises(ValueError): gpu.status()

    def test_session_contract(self):
        selected = gpu.status()['amd_devices'][0]
        item = runtime.OfflineVulkanSession(selected)
        with self.assertRaises(ValueError): item.prepare_module(SOURCE / 'consensus/cuda_walk_kernels.cu', 'pom_v4_walk_kernel')
        with self.assertRaises(ValueError): item.prepare_module(SOURCE / 'consensus/cuda_walk_kernels.cu', 'pom_v4_walk_kernel', 2048, bounded_indices_proven=True)
        item.active = False
        with self.assertRaises(ValueError): item.environment()

    def test_all_matrix_pages_with_amd_rows(self):
        from nexus_dashboard import DashboardModel
        from nexus_presenter import build_presenter_frame
        model = DashboardModel(); model.fixture_mode = True; model.gpu_devices = gpu.status()
        for tab in range(8):
            frame = build_presenter_frame(46, 160, model, 'UNKNOWN', tab)
            self.assertTrue(frame.panels)
            if tab == 1:
                labels = [row[0] for panel in frame.panels for row in panel['rows']]
                self.assertEqual(labels.count('Device enabled'), 2)
                self.assertEqual(labels.count('Sent / ACK / DAG / chain'), 2)
                self.assertIn('PoM execution backend', labels)
                self.assertIn('Winning commits', labels)


if __name__ == '__main__': unittest.main(verbosity=2)
