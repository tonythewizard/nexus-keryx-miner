"""Pinning, idempotence and port scoping of the gRPC MSVC build workaround."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('grpc_msvc_patch', ROOT / 'tools/cmake/grpc_msvc.py')
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class GrpcMsvcTests(unittest.TestCase):
    def setUp(self):
        (ROOT / '.local/tmp').mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.header = self.root / 'call_filters.h'
        self.original = b'namespace fixture {\n' + helper.MARKER + b'  return unchanged_expression;\n}\n}\n'
        self.patched = self.original.replace(helper.MARKER, helper.MARKER + helper.INSERT)
        self.header.write_bytes(self.original)
        pins = patch.multiple(helper, ORIGINAL_SHA256=digest(self.original), PATCHED_SHA256=digest(self.patched))
        pins.start()
        self.addCleanup(pins.stop)

    def test_only_local_constexpr_deductions_are_added_and_reapply_is_noop(self):
        self.assertTrue(helper.apply(self.header))
        data = self.header.read_bytes()
        self.assertEqual(data, self.patched)
        self.assertEqual(data.replace(helper.INSERT, b''), self.original)
        self.assertEqual(data.count(b'[[maybe_unused]] constexpr auto'), 6)
        before = self.header.stat().st_mtime_ns
        self.assertFalse(helper.apply(self.header))
        self.assertEqual(self.header.stat().st_mtime_ns, before)

    def test_changed_input_is_rejected_without_rewriting(self):
        corrupted = self.original + b'changed\n'
        self.header.write_bytes(corrupted)
        with self.assertRaisesRegex(ValueError, 'Unexpected gRPC source'):
            helper.apply(self.header)
        self.assertEqual(self.header.read_bytes(), corrupted)

    def test_changed_output_pin_cannot_write_a_partial_patch(self):
        with patch.object(helper, 'PATCHED_SHA256', '0' * 64):
            with self.assertRaisesRegex(ValueError, 'output checksum'):
                helper.apply(self.header)
        self.assertEqual(self.header.read_bytes(), self.original)

    def test_project_hook_skips_other_compilers_and_rejects_unknown_grpc(self):
        hook = ROOT / 'tools/cmake/grpc-msvc.cmake'
        skipped = subprocess.run(['cmake', '-DCMAKE_CXX_COMPILER_ID=GNU', '-P', str(hook)], capture_output=True, text=True)
        self.assertEqual(skipped.returncode, 0, skipped.stderr)
        unknown = subprocess.run(['cmake', '-DCMAKE_CXX_COMPILER_ID=MSVC',
                                  '-DCMAKE_CXX_COMPILER_VERSION=19.51', '-DPACKAGE_VERSION=2.0',
                                  '-P', str(hook)], capture_output=True, text=True)
        self.assertNotEqual(unknown.returncode, 0)
        self.assertIn('before updating the dependency', unknown.stderr)

    def test_vcpkg_hashes_helpers_and_scopes_hook_to_grpc_only(self):
        triplet = ROOT / 'tools/cmake/triplets/x64-windows-static-md.cmake'
        for port in ('grpc', 'protobuf'):
            script = self.root / (port + '.cmake')
            script.write_text('set(PORT ' + port + ')\ninclude("' + triplet.as_posix() + '")\n'
                              'file(WRITE "' + (self.root / (port + '.txt')).as_posix() +
                              '" "${VCPKG_CMAKE_CONFIGURE_OPTIONS}\\n${VCPKG_HASH_ADDITIONAL_FILES}")\n')
            subprocess.run(['cmake', '-P', str(script)], check=True, capture_output=True)
        grpc = (self.root / 'grpc.txt').read_text()
        other = (self.root / 'protobuf.txt').read_text()
        self.assertIn('CMAKE_PROJECT_grpc_INCLUDE=', grpc)
        self.assertIn('grpc_msvc.py', grpc)
        self.assertNotIn('grpc-msvc', other)
        self.assertIn('CMAKE_CXX_STANDARD=20', grpc)
        self.assertIn('CMAKE_CXX_STANDARD=20', other)


if __name__ == '__main__':
    unittest.main()
