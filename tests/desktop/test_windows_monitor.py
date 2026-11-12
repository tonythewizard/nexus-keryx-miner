"""Read-only Windows desktop: shared panels without Linux controllers or wallet reads."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
TEST_ROOT = ROOT / '.local/tmp/windows-monitor-tests'
TEST_ROOT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
from nexus_windows_monitor import WindowsMonitor
from nexus_hardware_metrics import inventory


class WindowsMonitorTests(unittest.TestCase):
    def test_all_pages_render_without_wallet_or_service_subprocesses(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as directory:
            monitor = WindowsMonitor(directory)
            amd = {'id': 'PCI\\VEN_1002&DEV_0000', 'name': 'Synthetic AMD', 'vendor': 'amd',
                   'runtime_lane': None, 'vram_bytes': None, 'metrics': {},
                   'driver': 'Windows display driver', 'mode': 'diagnostics', 'capabilities_status': 'NOT QUALIFIED'}
            with patch('nexus_dashboard.load_wallet_status', side_effect=AssertionError('wallet access')), \
                 patch('nexus_hardware_metrics.inventory', return_value={'devices': [amd], 'amd_devices': [amd], 'selection': {}}), \
                 patch('nexus_dashboard.subprocess.run', side_effect=AssertionError('unexpected process')), \
                 patch('nexus_dashboard.host_memory', return_value={}):
                for tab in range(9):
                    frame = monitor.snapshot(tab, 'ALL', 46)
                    self.assertEqual(frame['tab'], tab)
                    self.assertTrue(frame['panels'])
                    self.assertTrue(frame['desktop']['read_only'])
                    self.assertNotIn('escrow_public_key', frame)
                    json.dumps(frame, allow_nan=False)

    def test_existing_telemetry_does_not_start_wallet_helpers(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as directory:
            logs = Path(directory) / 'logs'
            logs.mkdir()
            (logs / 'nexus-ui-inference-synthetic.jsonl').write_text(
                json.dumps({'worker': {'event': 'STARTUP_STATE', 'state': 'BOOT', 'result': 'PASS'}}) + '\n')
            monitor = WindowsMonitor(directory)
            with patch('nexus_dashboard.load_wallet_status', side_effect=AssertionError('wallet access')), \
                 patch('nexus_hardware_metrics.inventory', return_value={'devices': [], 'amd_devices': [], 'selection': {}}), \
                 patch('nexus_dashboard.subprocess.run', side_effect=AssertionError('unexpected process')), \
                 patch('nexus_dashboard.host_memory', return_value={}):
                frame = monitor.snapshot(4, 'ALL', 46)
                self.assertTrue(frame['panels'])
            monitor.reader.close()

    def test_controllers_fail_closed_even_with_confirmation(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as directory:
            monitor = WindowsMonitor(directory)
            with patch('subprocess.run', side_effect=AssertionError('must not launch a controller')):
                for action in ('stop', 'restart', 'shutdown', 'gpu_select', 'key', 'api_generate', 'workload'):
                    with self.assertRaises(ValueError):
                        monitor.action(action, '', True)

    def test_preferences_are_saved_only_in_selected_data_root(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as directory:
            monitor = WindowsMonitor(directory)
            monitor.action('preferences', {'font_size': 14}, False)
            value = json.loads((Path(directory) / 'state/desktop/presentation.json').read_text())
            self.assertEqual(value['font_size'], 14)
            with self.assertRaises(ValueError):
                monitor.action('preferences', {'workloads': {'anything': 'nvidia'}}, True)

    def test_missing_sensors_are_unknown_not_zero_or_adapter_ram(self):
        video = json.dumps({'Name': 'Synthetic AMD', 'PNPDeviceID': 'PCI\\VEN_1002&DEV_0000'})
        with patch('nexus_hardware_metrics.os.name', 'nt'), \
             patch('nexus_hardware_metrics.run', side_effect=[subprocess.CalledProcessError(1, 'nvidia-smi'), video]):
            result = inventory()
        self.assertEqual(len(result['devices']), 1)
        self.assertEqual(result['devices'][0]['metrics'], {})
        self.assertIsNone(result['devices'][0]['vram_bytes'])
        self.assertIsNone(result['devices'][0]['runtime_lane'])

    def test_nvidia_values_are_finite_and_bound_to_uuid(self):
        with patch('nexus_hardware_metrics.os.name', 'posix'), \
             patch('nexus_hardware_metrics.run', return_value='GPU-synthetic, Test GPU, 8192, 1024, 0, nan, N/A, 1700\n'):
            device = inventory()['devices'][0]
        self.assertEqual(device['id'], 'GPU-synthetic')
        self.assertEqual(device['metrics']['gpu_busy_percent'], 0)
        self.assertEqual(device['metrics']['vram_used_bytes'], 1024 * 2**20)
        self.assertIsNone(device['metrics']['temperature_c'])
        self.assertIsNone(device['metrics']['power_w'])


if __name__ == '__main__':
    unittest.main()
