"""Windows build regression: ICO is generated from the verified source PNG."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('desktop_assets', ROOT / 'tools/prepare_assets.py')
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)
ICON = 'nexus-ui-desktop/src-tauri/icons/icon.png'


class WindowsIconTests(unittest.TestCase):
    def setUp(self):
        cache = ROOT / '.local/tmp'
        cache.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=cache)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.png = self.root / ICON
        self.png.parent.mkdir(parents=True)
        self.ico = self.png.with_suffix('.ico')
        source = (ROOT / ICON).with_name('icon.png.hex').read_text()
        self.png.with_name('icon.png.hex').write_text(source)
        self.expected = json.loads((ROOT / 'tools/source-assets.json').read_text())[ICON]
        (self.root / 'tools').mkdir()
        (self.root / 'tools/source-assets.json').write_text(json.dumps({ICON: self.expected}))

    def prepare(self):
        with patch.object(assets, 'ROOT', self.root):
            assets.prepare()

    def test_clean_checkout_generates_valid_ico_with_exact_png_payload(self):
        self.prepare()
        data = self.ico.read_bytes()
        self.assertEqual(struct.unpack('<HHH', data[:6]), (0, 1, 1))
        width, height = struct.unpack('>II', self.png.read_bytes()[16:24])
        entry = struct.unpack('<BBBBHHII', data[6:22])
        self.assertEqual(entry, (width % 256, height % 256, 0, 0, 1, 32,
                                 self.expected['bytes'], 22))
        self.assertEqual(data[22:], self.png.read_bytes())
        self.assertEqual(hashlib.sha256(data[22:]).hexdigest(), self.expected['sha256'])
        before = self.ico.stat().st_mtime_ns
        self.prepare()
        self.assertEqual(self.ico.stat().st_mtime_ns, before)

    def test_corrupt_source_is_rejected_before_ico_generation(self):
        self.png.with_name('icon.png.hex').write_text('00')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            self.prepare()
        self.assertFalse(self.ico.exists())

    @unittest.skipIf(os.name == 'nt', 'Creating symlinks may require Windows privileges')
    def test_ico_symlink_cannot_overwrite_another_file(self):
        other = self.root / 'other.txt'
        other.write_text('preserve')
        self.ico.symlink_to(other)
        with self.assertRaisesRegex(ValueError, 'Unsafe Windows icon'):
            self.prepare()
        self.assertEqual(other.read_text(), 'preserve')

    def test_windows_config_uses_generated_ico_and_linux_keeps_png(self):
        directory = ROOT / 'nexus-ui-desktop/src-tauri'
        windows = json.loads((directory / 'tauri.windows.conf.json').read_text())
        base = json.loads((directory / 'tauri.conf.json').read_text())
        self.assertEqual(windows['bundle']['icon'], ['icons/icon.ico'])
        self.assertEqual(base['bundle']['icon'], ['icons/icon.png'])


if __name__ == '__main__':
    unittest.main()
