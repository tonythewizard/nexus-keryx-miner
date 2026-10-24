from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/runtime/apply_trace_patch.cmake'


class TracePatch(unittest.TestCase):
    def setUp(self):
        (ROOT / '.local/tmp').mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp')
        self.addCleanup(temporary.cleanup)
        self.checkout = Path(temporary.name) / 'checkout'
        self.checkout.mkdir()
        subprocess.run(['git', 'init', '--quiet', str(self.checkout)], check=True)
        self.source = self.checkout / 'build/dependency'
        self.source.mkdir(parents=True)
        self.patch = Path(temporary.name) / 'trace.patch'
        chunks = []
        for suffix in ('cu', 'cuh'):
            name = f'ggml/src/ggml-cuda/q8canon.{suffix}'
            chunks.append(f'diff --git a/{name} b/{name}\n'
                          'new file mode 100644\n--- /dev/null\n'
                          f'+++ b/{name}\n@@ -0,0 +1 @@\n+canonical-{suffix}\n')
        self.patch.write_bytes(''.join(chunks).encode('utf-8'))

    def run_patch(self, verify=False):
        # Reproduce both parent-checkout discovery and inherited Git overrides.
        environment = dict(os.environ, GIT_DIR=str(self.checkout / '.git'),
                           GIT_WORK_TREE=str(self.checkout))
        return subprocess.run(
            ['cmake', f'-DNEXUS_LLAMA_SOURCE={self.source}',
             f'-DNEXUS_TRACE_PATCH={self.patch}',
             f'-DNEXUS_VERIFY_ONLY={"ON" if verify else "OFF"}', '-P', str(SCRIPT)],
            env=environment, text=True, capture_output=True)

    def test_nested_checkout_patch_is_applied_and_reconfiguration_is_idempotent(self):
        first = self.run_patch()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        for suffix in ('cu', 'cuh'):
            self.assertEqual((self.source / f'ggml/src/ggml-cuda/q8canon.{suffix}').read_text(),
                             f'canonical-{suffix}\n')
        again = self.run_patch()
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        checked = self.run_patch(verify=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_missing_or_modified_patch_fails_closed(self):
        self.assertNotEqual(self.run_patch(verify=True).returncode, 0)
        self.assertFalse((self.source / 'ggml/src/ggml-cuda/q8canon.cu').exists())
        applied = self.run_patch()
        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        kernel = self.source / 'ggml/src/ggml-cuda/q8canon.cu'
        kernel.write_text('modified\n')
        self.assertNotEqual(self.run_patch(verify=True).returncode, 0)
        self.assertNotEqual(self.run_patch().returncode, 0)
        self.assertEqual(kernel.read_text(), 'modified\n')

    def test_windows_checkout_preserves_canonical_patch_bytes(self):
        relative = Path('vendor/keryx-trace-abi6/tools/keryx-llama/patches/0001-canonical-trace.patch')
        canonical = subprocess.check_output(['git', '-C', str(ROOT), 'show', f'HEAD:{relative.as_posix()}'])
        checked_patch = self.checkout / relative
        checked_patch.parent.mkdir(parents=True)
        checked_patch.write_bytes(canonical)
        (self.checkout / '.gitattributes').write_bytes((ROOT / '.gitattributes').read_bytes())
        command = ['git', '-C', str(self.checkout), '-c', 'core.autocrlf=true']
        subprocess.run([*command, 'add', '.gitattributes', relative.as_posix()], check=True)
        checked_patch.unlink()
        subprocess.run([*command, 'checkout-index', '--force', '--', relative.as_posix()], check=True)
        self.assertEqual(checked_patch.read_bytes(), canonical)

        # Apply that checkout's bytes to LF archive files, including an existing
        # file hunk: additions alone would not detect the CRLF context mismatch.
        target = self.source / 'ggml/src/ggml-cuda/existing.cu'
        target.parent.mkdir(parents=True)
        target.write_bytes(b'upstream\n')
        synthetic = self.patch.read_bytes() + (
            b'diff --git a/ggml/src/ggml-cuda/existing.cu b/ggml/src/ggml-cuda/existing.cu\n'
            b'--- a/ggml/src/ggml-cuda/existing.cu\n'
            b'+++ b/ggml/src/ggml-cuda/existing.cu\n'
            b'@@ -1 +1 @@\n-upstream\n+patched\n')
        checked_patch.write_bytes(synthetic)
        subprocess.run([*command, 'add', relative.as_posix()], check=True)
        checked_patch.unlink()
        subprocess.run([*command, 'checkout-index', '--force', '--', relative.as_posix()], check=True)
        self.patch = checked_patch
        applied = self.run_patch()
        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        self.assertEqual(target.read_bytes(), b'patched\n')
        checked = self.run_patch(verify=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == '__main__':
    unittest.main()
