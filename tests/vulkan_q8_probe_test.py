#!/usr/bin/env python3
"""CPU tests for the isolated diagnostic instrumentation, without loading Vulkan."""
import hashlib
import importlib.util
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('q8_probe', ROOT / 'experiments/vulkan_inference/prepare_q8_probe.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
KSPEC = importlib.util.spec_from_file_location('canonical_q8_k', ROOT / 'experiments/vulkan_inference/build_canonical_q8_k.py')
KMODULE = importlib.util.module_from_spec(KSPEC)
KSPEC.loader.exec_module(KMODULE)
BSPEC = importlib.util.spec_from_file_location('bounded_upload', ROOT / 'experiments/vulkan_inference/prepare_bounded_upload.py')
BMODULE = importlib.util.module_from_spec(BSPEC)
BSPEC.loader.exec_module(BMODULE)
LSPEC = importlib.util.spec_from_file_location('lazy_mmap', ROOT / 'experiments/vulkan_inference/prepare_lazy_mmap.py')
LMODULE = importlib.util.module_from_spec(LSPEC)
LSPEC.loader.exec_module(LMODULE)
RSPEC = importlib.util.spec_from_file_location('trace_supervisor', ROOT / 'experiments/vulkan_inference/run_trace_fixture.py')
RMODULE = importlib.util.module_from_spec(RSPEC)
RSPEC.loader.exec_module(RMODULE)


class InstrumentationTest(unittest.TestCase):
    source = ('static void ggml_vk_mul_mat_vec_q_f16(\n'
              '    // For batch_n, the A matrix is the same for each batch, and B/D use the row stride as the batch stride\n'
              '    return GGML_STATUS_SUCCESS;\n\n    UNUSED(backend);\n'
              'device->integer_dot_product = device->integer_dot_product && shader_integer_dot_product_props.integerDotProduct4x8BitPackedSignedAccelerated;\n'
              '        vkGetPhysicalDeviceFeatures2(device->physical_device, &device_features2);')

    def pinned(self, source):
        return patch.object(MODULE, 'PIN', hashlib.sha256(source.encode()).hexdigest())

    def test_unknown_source_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unexpected'):
            MODULE.instrument(self.source, '// hook')

    def test_exact_hooks_only(self):
        with self.pinned(self.source):
            result = MODULE.instrument(self.source, '// hook')
        self.assertEqual(result.count('nexus_vk_q8_probe_capture('), 1)
        self.assertEqual(result.count('nexus_vk_q8_probe_finish('), 1)
        self.assertTrue(result.startswith('// hook\n'))
        self.assertIn('return GGML_STATUS_SUCCESS', result)

    def test_missing_or_duplicate_anchor_rejected(self):
        for source in (self.source.replace('UNUSED(backend)', 'UNUSED(other)'), self.source * 2):
            with self.pinned(source), self.assertRaisesRegex(ValueError, 'Ambiguous'):
                MODULE.instrument(source, '// hook')

    def test_reinstrumentation_rejected(self):
        with self.pinned(self.source):
            result = MODULE.instrument(self.source, '// hook')
            with self.assertRaisesRegex(ValueError, 'Unexpected'):
                MODULE.instrument(result, '// hook')

    def test_canonical_correction_is_local_and_source_pinned(self):
        source = 'before\n' + KMODULE.OLD + '\nafter\n'
        with self.assertRaises(ValueError):
            KMODULE.correct(source)
        with patch.object(KMODULE, 'PIN', hashlib.sha256(source.encode()).hexdigest()):
            result = KMODULE.correct(source)
        self.assertEqual(result, 'before\n' + KMODULE.NEW + '\nafter\n')
        self.assertNotIn('cache_b_ds.y / 2', result)

    def test_upload_patch_rejects_unreviewed_source(self):
        with self.assertRaises(ValueError):
            BMODULE.prepare(BMODULE.OLD)

    def test_lazy_mapping_changes_only_eager_prefetch_on_pinned_source(self):
        text = 'before\n' + LMODULE.OLD + '\nafter\n'
        with self.assertRaises(ValueError):
            LMODULE.prepare(text)
        with patch.object(LMODULE, 'PIN', hashlib.sha256(text.encode()).hexdigest()):
            self.assertEqual(LMODULE.prepare(text), 'before\n' + LMODULE.NEW + '\nafter\n')
        duplicate = text + text
        with patch.object(LMODULE, 'PIN', hashlib.sha256(duplicate.encode()).hexdigest()):
            with self.assertRaises(ValueError):
                LMODULE.prepare(duplicate)

    def test_upload_patch_is_confined_to_the_fallback(self):
        source = 'before\n' + BMODULE.OLD + '\nafter\n'
        with patch.object(BMODULE, 'PIN', hashlib.sha256(source.encode()).hexdigest()):
            result = BMODULE.prepare(source)
        self.assertTrue(result.endswith('before\n' + BMODULE.NEW + '\nafter\n'))
        self.assertIn('ggml_validate_row_data', result)
        self.assertIn('progress_callback', result)

    def test_loader_overlay_is_repeatable_and_preserves_original(self):
        text = 'before\n' + BMODULE.OLD + '\nafter\n'
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory)/'original.cpp', Path(directory)/'overlay.cpp'
            source.write_text(text)
            with patch.object(BMODULE, 'PIN', hashlib.sha256(text.encode()).hexdigest()):
                BMODULE.write_overlay(source, output)
                first = output.read_bytes()
                with self.assertRaises(ValueError):
                    BMODULE.write_overlay(source, output)
                BMODULE.write_overlay(source, output, replace=True)
                self.assertEqual(output.read_bytes(), first)
                self.assertEqual(source.read_text(), text)
                with self.assertRaises(ValueError):
                    BMODULE.write_overlay(source, source, replace=True)
                output.unlink(); output.symlink_to(source)
                with self.assertRaises(ValueError):
                    BMODULE.write_overlay(source, output, replace=True)
                self.assertEqual(source.read_text(), text)

    def test_disappeared_or_zombie_node_is_not_a_valid_sample(self):
        self.assertIsNone(RMODULE.process_memory('State:\tZ (zombie)\n'))
        self.assertIsNone(RMODULE.process_memory('VmRSS:\t1024 kB\n'))
        self.assertEqual(RMODULE.process_memory('VmRSS:\t1024 kB\nVmSwap:\t2048 kB\n'),
                         {'VmRSS': 1048576, 'VmSwap': 2097152})

    def test_host_swap_budget_uses_total_and_free_not_node_rss(self):
        host = RMODULE.host_memory('MemAvailable: 8000000 kB\nSwapTotal: 16000000 kB\nSwapFree: 1000000 kB\n')
        self.assertLess(host['oomd_swap_limit'] - host['swap_used'], 0)
        self.assertGreater(host['available'], 4 * 1024**3)

    def test_supervisor_fails_closed_before_oomd_and_after_node_exit(self):
        host = {'oomd_swap_limit': 1000000000, 'swap_used': 950000000, 'available': 8 * 1024**3}
        self.assertIn('node disappeared', RMODULE.stop_reason(None, {}, host, {}, 0, 0, 0, 0))
        self.assertIn('oomd', RMODULE.stop_reason({}, {}, host, {}, 0, 0, 0, 0))
        host['swap_used'] = 0
        self.assertIsNone(RMODULE.stop_reason({}, {}, host, {}, 0, 0, 0, 0))


if __name__ == '__main__':
    unittest.main()
