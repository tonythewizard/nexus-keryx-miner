"""Deterministic process-identity fixtures; no processes or GPUs are started."""
import os
from pathlib import Path
import tempfile
import unittest
from cuda_test_isolation import assert_no_gpu_miner, assert_only_display_contexts


class Isolation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ['TMPDIR'])
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def process(self, pid, executable, arguments):
        p = self.root / str(pid)
        p.mkdir()
        (p / 'exe').symlink_to(executable)
        (p / 'comm').write_text(Path(executable).name + '\n')
        (p / 'cmdline').write_bytes(b'\0'.join(os.fsencode(a) for a in arguments) + b'\0')

    def test_shell_text_is_not_a_running_worker(self):
        self.process(1, '/usr/bin/bash', ['bash', '-c', 'echo nexus-live-cuda-worker; fish nexusmine.fish live'])
        assert_no_gpu_miner(self.root)

    def test_actual_worker_is_rejected(self):
        self.process(2, '/artifacts/nexus-live-cuda-worker', ['nexus-live-cuda-worker'])
        with self.assertRaisesRegex(RuntimeError, 'production NEXUS worker'):
            assert_no_gpu_miner(self.root)

    def test_deleted_running_worker_is_rejected(self):
        self.process(3, '/artifacts/nexus-live-cuda-worker (deleted)', ['renamed'])
        with self.assertRaises(RuntimeError):
            assert_no_gpu_miner(self.root)

    def test_stock_worker_is_rejected(self):
        self.process(4, '/stock/keryx-miner', ['keryx-miner'])
        with self.assertRaisesRegex(RuntimeError, 'stock miner'):
            assert_no_gpu_miner(self.root)

    def test_live_launcher_is_rejected_before_its_worker_exists(self):
        for name in ['nexusmine.fish', 'nexusmine-next.fish', 'launcher.fish']:
            with self.subTest(name=name):
                p = self.root / '5'
                if p.exists():
                    (p / 'exe').unlink()
                    (p / 'cmdline').unlink()
                    (p / 'comm').unlink()
                    p.rmdir()
                self.process(5, '/usr/bin/fish', ['fish', '/persistent/' + name, 'live'])
                with self.assertRaisesRegex(RuntimeError, 'live miner launcher'):
                    assert_no_gpu_miner(self.root)

    def test_cpu_audit_launcher_and_harness_do_not_claim_gpu(self):
        self.process(6, '/usr/bin/fish', ['fish', '/persistent/nexusmine-next.fish', 'audit-ai'])
        self.process(7, '/tests/nexus-ai-response-service-test', ['nexus-ai-response-service-test'])
        assert_no_gpu_miner(self.root)

    def test_exited_process_is_ignored(self):
        (self.root / '8').mkdir()
        assert_no_gpu_miner(self.root)

    def test_verified_kde_display_context_is_allowed(self):
        self.process(10, '/usr/bin/kwin_wayland', ['kwin_wayland'])
        assert_only_display_contexts('10, /usr/bin/kwin_wayland\n', self.root)
        assert_only_display_contexts('', self.root)

    def test_nondumpable_display_requires_correlated_nvml_name_and_owner(self):
        from unittest.mock import patch
        self.process(10, '/usr/bin/kwin_wayland', ['kwin_wayland'])
        with patch('cuda_test_isolation.os.readlink', side_effect=PermissionError):
            assert_only_display_contexts('10, /usr/bin/kwin_wayland', self.root)
            (self.root / '10/comm').write_text('llama-cli\n')
            with self.assertRaises(RuntimeError):
                assert_only_display_contexts('10, /usr/bin/kwin_wayland', self.root)

    def test_same_name_outside_system_path_is_rejected(self):
        self.process(11, '/untrusted/kwin_wayland', ['kwin_wayland'])
        with self.assertRaises(RuntimeError):
            assert_only_display_contexts('11, /usr/bin/kwin_wayland', self.root)

    def test_mixed_display_and_inference_contexts_are_rejected(self):
        self.process(10, '/usr/bin/kwin_wayland', ['kwin_wayland'])
        self.process(12, '/tools/llama-cli', ['llama-cli'])
        with self.assertRaises(RuntimeError):
            assert_only_display_contexts('10, /usr/bin/kwin_wayland\n12, /tools/llama-cli\n', self.root)

    def test_unverifiable_compute_identity_is_rejected(self):
        for value in ['N/A', '-1, /usr/bin/kwin_wayland', '13, /usr/bin/kwin_wayland']:
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                assert_only_display_contexts(value, self.root)


if __name__ == '__main__':
    unittest.main(verbosity=2)
