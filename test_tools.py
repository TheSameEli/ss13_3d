# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Regressions for portable asset tooling and source-only publication boundaries."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image
from asset_utils import fingerprint
from export_source import export_source


class ToolTests(unittest.TestCase):
    def test_transparent_rgb_is_not_identity(self):
        a = Image.new('RGBA', (2, 2), (255, 0, 100, 0))
        b = Image.new('RGBA', (2, 2), (0, 0, 0, 0))
        self.assertEqual(fingerprint(a), fingerprint(b))
        a.putpixel((0, 0), (255, 0, 100, 255))
        self.assertNotEqual(fingerprint(a), fingerprint(b))

    def test_empty_asset_root_fails_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name('compile_geometry.py')),
                 '--assets-root', temp], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Missing icons/', result.stderr)
            self.assertEqual(list(Path(temp).iterdir()), [])

    def test_source_export_excludes_private_and_runtime_data(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'source'
            manifest = export_source(output)
            self.assertIn('README.md', manifest)
            self.assertNotIn('server/native_3d_controls.dm', manifest)
            self.assertIn('AUTHORS.md', manifest)
            for name, digest in manifest.items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
                self.assertNotIn(Path(name).parts[0], ('bin', 'package', 'research', '.git'))
                self.assertFalse(name.endswith(('.generated.h', '.mp3', '.dll', '.exe')))
            marker = output / 'keep-my-file.txt'
            marker.write_text('unchanged', encoding='utf-8')
            with self.assertRaises(FileExistsError):
                export_source(output)
            self.assertEqual(marker.read_text(encoding='utf-8'), 'unchanged')


if __name__ == '__main__':
    unittest.main()
