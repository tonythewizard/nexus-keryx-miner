#!/usr/bin/env python3
"""CPU-only admission checks; no real process or GPU is stopped or opened."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('capture_runner', Path(__file__).with_name('run_abi6_capture_test.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Admission(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.audit = Path(self.temp.name) / 'audit.jsonl'
        self.summary = dict(event='AUDIT_SUMMARY', complete=True, live_watermark_complete=True,
                            assigned_open=0, journal_ambiguous=0, pending_class='AI_PENDING_NONE_VERIFIED')

    def write(self):
        self.audit.write_text(json.dumps(self.summary) + '\n')

    def test_incomplete_coverage_refused_before_any_gpu_admission(self):
        self.summary['complete'] = False; self.write()
        with self.assertRaisesRegex(ValueError, 'coverage'):
            runner.admit(self.audit)

    def test_stale_audit_refused(self):
        self.write(); os.utime(self.audit, (time.time()-120, time.time()-120))
        with self.assertRaisesRegex(ValueError, '60 seconds'):
            runner.admit(self.audit)

    def test_open_obligations_and_unknown_state_refused(self):
        self.summary['assigned_open'] = 1; self.write()
        with self.assertRaisesRegex(ValueError, 'obligations'):
            runner.admit(self.audit)
        self.summary['assigned_open'] = 0; self.summary['pending_class'] = 'AI_PENDING_INDETERMINATE'; self.write()
        with self.assertRaisesRegex(ValueError, 'indeterminate'):
            runner.admit(self.audit)

    def test_exclusive_device_selection_and_valid_admission(self):
        self.write()
        # Synthetic empty process snapshot: no hardware/process inspection in this test.
        with patch.object(Path, 'iterdir', return_value=iter([])), patch.dict(os.environ, CUDA_VISIBLE_DEVICES='wrong'):
            with self.assertRaisesRegex(ValueError, 'UUID'):
                runner.admit(self.audit)
        with patch.object(Path, 'iterdir', return_value=iter([])), patch.dict(os.environ, CUDA_VISIBLE_DEVICES=runner.UUID):
            runner.admit(self.audit)

    def test_existing_miner_prevents_gpu_admission(self):
        self.write()
        process = Path(self.temp.name) / '123'; process.mkdir()
        binary = Path(self.temp.name) / 'nexus-live-cuda-worker'; binary.touch()
        (process / 'exe').symlink_to(binary)
        with patch.object(Path, 'iterdir', return_value=iter([process])):
            with self.assertRaisesRegex(ValueError, 'existing miner'):
                runner.admit(self.audit)

    def test_offline_scope_requires_exclusive_gpu(self):
        with patch.object(Path, 'iterdir', return_value=iter([])), patch.dict(os.environ, CUDA_VISIBLE_DEVICES='wrong'):
            with self.assertRaisesRegex(ValueError, 'UUID'):
                runner.admit_exclusive()
        with patch.object(Path, 'iterdir', return_value=iter([])), patch.dict(os.environ, CUDA_VISIBLE_DEVICES=runner.UUID):
            runner.admit_exclusive()

    def test_offline_scope_still_rejects_existing_miner(self):
        process = Path(self.temp.name) / '123'; process.mkdir()
        binary = Path(self.temp.name) / 'nexus-live-cuda-worker'; binary.touch()
        (process / 'exe').symlink_to(binary)
        with patch.object(Path, 'iterdir', return_value=iter([process])):
            with self.assertRaisesRegex(ValueError, 'existing miner'):
                runner.admit_exclusive()


if __name__ == '__main__':
    unittest.main()
