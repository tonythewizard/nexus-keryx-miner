import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('notices', ROOT / 'tools/collect_notices.py')
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class NoticeBoundary(unittest.TestCase):
    def test_only_regular_files_inside_the_package_are_copied(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp') as temporary:
            root = Path(temporary)
            package = root / 'package'
            package.mkdir()
            license_file = package / 'LICENSE'
            license_file.write_text('Synthetic license fixture')
            destination = root / 'output/LICENSE'
            self.assertTrue(notices.copy_notice(license_file, package, destination))
            self.assertEqual(destination.read_text(), license_file.read_text())
            credentials = root / 'credentials.toml'
            credentials.write_text('Synthetic private fixture; never copy')
            with self.assertRaises(ValueError):
                notices.copy_notice(credentials, package, root / 'output/escaped')
            linked = package / 'LICENSE-LINK'
            linked.symlink_to(credentials)
            self.assertFalse(notices.copy_notice(linked, package, root / 'output/linked'))
            self.assertFalse((root / 'output/escaped').exists())
            self.assertFalse((root / 'output/linked').exists())


if __name__ == '__main__':
    unittest.main()
