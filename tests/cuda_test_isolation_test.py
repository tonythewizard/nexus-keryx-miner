"""Deterministic process-identity fixtures; no processes or GPUs are started."""
import os
from pathlib import Path
import tempfile
import unittest
from cuda_test_isolation import assert_no_gpu_miner


class Isolation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ['TMPDIR'])
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def process(self, pid, executable, arguments):
        p = self.root / str(pid)
        p.mkdir()
        (p / 'exe').symlink_to(executable)
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
