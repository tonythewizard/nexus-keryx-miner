import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('dependencies', ROOT / 'tools/distribution_dependencies.py')
dependencies = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dependencies)


class Dependencies(unittest.TestCase):
    def test_windows_imports_require_the_matching_bundled_dll(self):
        dump = '  KERNEL32.dll\n  api-ms-win-core-file-l1-1-0.dll\n  CUBLAS64_13.DLL\n'
        with tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp') as temporary:
            directory = Path(temporary)
            (directory / 'nexus-live-cuda-worker.exe').touch()
            with patch.object(dependencies.subprocess, 'check_output', return_value=dump):
                with self.assertRaises(ValueError):
                    dependencies.verify(directory, 'windows', 'dumpbin.exe')
                (directory / 'cublas64_13.dll').touch()
                self.assertEqual(dependencies.verify(directory, 'windows', 'dumpbin.exe'), 2)

    def test_elf_imports_are_read_from_needed_records(self):
        self.assertEqual(dependencies.imports(
            '0x0001 (NEEDED) Shared library: [libcudart.so.13]\n'
            '0x0001 (NEEDED) Shared library: [libc.so.6]\n', 'linux'),
            {'libcudart.so.13', 'libc.so.6'})


if __name__ == '__main__':
    unittest.main()
