from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == 'nt', 'Windows experiment entry')
class VramExperimentEntryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'Test fork'
        script_dir = self.root / 'tools/experiments'
        script_dir.mkdir(parents=True)
        self.entry = script_dir / 'test-vram-writeback.cmd'
        shutil.copy2(ROOT / 'tools/experiments/test-vram-writeback.cmd', self.entry)
        (self.root / 'out').mkdir()
        (self.root / 'out/bb-probe.exe').write_text('existence fixture, never executed')
        (self.root / 'run.bat').write_text(
            '@echo off\n'
            '> captured.txt echo mode=%BB_IMAGE_WRITEBACK_GC%\n'
            '>> captured.txt echo tracking=%BB_WRITE_TRACKING%\n'
            '>> captured.txt echo frames=%BB_FRAME_STATS%\n'
            '>> captured.txt echo prebuilt=%BB_PREBUILT%\n'
            'exit /b 0\n')

    def invoke(self, *args):
        return subprocess.run(['cmd.exe', '/d', '/c', str(self.entry), *args],
                              capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)

    def test_modes_keep_logging_and_tracking_identical(self):
        for args, expected in (((), '1'), (('on',), '1'), (('off',), '0')):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                values = dict(line.split('=', 1) for line in (self.root / 'captured.txt').read_text().splitlines())
                self.assertEqual(values, dict(mode=expected, tracking='1', frames='1', prebuilt='1'))

    def test_unknown_mode_never_reaches_the_runtime(self):
        result = self.invoke('unknown')
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / 'captured.txt').exists())
