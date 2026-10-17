from paths import ROOT
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == 'nt', 'Windows crash reporter')
class NativeCrashReporterTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Path(os.environ.get('BB_CRASH_FIXTURE', ROOT / 'out/diagnostics-test.exe'))
        if not self.fixture.is_file():
            self.skipTest('Compile diagnostics-test.exe first (bash build.sh --test).')
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.session = Path(self.temporary.name) / 'Thử lỗi native'
        self.session.mkdir()

    def invoke(self, *args):
        env = dict(os.environ, BB_SESSION_DIR=str(self.session))
        return subprocess.run([str(self.fixture), *args], env=env, capture_output=True,
                              timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)

    def test_fault_produces_remote_minidump_with_original_exception(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 139, result.stderr)
        fault = json.loads((self.session / 'fault.json').read_text())
        self.assertEqual(fault['exception_code'], '0xc0000005')
        self.assertEqual(fault['origin'], 'host')
        status = json.loads((self.session / 'minidump.json').read_text())
        self.assertTrue(status['success'], status)
        dump = (self.session / 'crash.dmp').read_bytes()
        self.assertEqual(dump[:4], b'MDMP')
        streams, directory = struct.unpack_from('<II', dump, 8)
        exceptions = []
        for index in range(streams):
            kind, size, offset = struct.unpack_from('<III', dump, directory + index * 12)
            if kind == 6:  # MINIDUMP_EXCEPTION_STREAM
                exceptions.append(struct.unpack_from('<I', dump, offset + 8)[0])
        self.assertEqual(exceptions, [0xc0000005])

    def test_normal_exit_does_not_create_dump(self):
        result = self.invoke('--normal')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.session / 'crash.dmp').exists())
        self.assertFalse((self.session / 'minidump.json').exists())

    def test_synthetic_fatal_escapes_reason_and_writes_dump(self):
        result = self.invoke('--fatal')
        self.assertEqual(result.returncode, 23, result.stderr)
        fault = json.loads((self.session / 'fault.json').read_text())
        self.assertEqual(fault['exception_code'], '0xe0424223')
        self.assertEqual(fault['reason'], 'Synthetic "assertion"\\path\nsecond line')
        self.assertTrue(json.loads((self.session / 'minidump.json').read_text())['success'])


@unittest.skipUnless(os.name == 'nt', 'Windows renderer assertion reporter')
class RendererAssertionTests(NativeCrashReporterTests):
    def setUp(self):
        super().setUp()
        self.fixture = ROOT / 'out/renderer-assert-test.exe'
        if not self.fixture.is_file():
            self.skipTest('Compile renderer-assert-test.exe first.')

    # The renderer fixture always asserts; the inherited normal/fault cases are in the C fixture.
    test_fault_produces_remote_minidump_with_original_exception = None
    test_normal_exit_does_not_create_dump = None
    test_synthetic_fatal_escapes_reason_and_writes_dump = None

    def test_main_worker_and_concurrent_assertions_have_one_complete_dump(self):
        for mode in ('main', 'worker', 'concurrent', 'concurrent'):
            with self.subTest(mode=mode):
                for name in ('fault.json', 'minidump.json', 'crash.dmp'):
                    (self.session / name).unlink(missing_ok=True)
                result = self.invoke(mode)
                self.assertEqual(result.returncode, 23, result.stderr)
                self.assertIn(b'Intentional renderer assertion fixture', result.stderr)
                fault = json.loads((self.session / 'fault.json').read_text())
                self.assertEqual(fault['exception_code'], '0xe0424223')
                self.assertEqual(fault['reason'], 'GPU library assertion failed')
                self.assertTrue(json.loads((self.session / 'minidump.json').read_text())['success'])
                dump = (self.session / 'crash.dmp').read_bytes()
                self.assertEqual(dump[:4], b'MDMP')
                streams, directory = struct.unpack_from('<II', dump, 8)
                exceptions = []
                for index in range(streams):
                    kind, _, offset = struct.unpack_from('<III', dump, directory + index * 12)
                    if kind == 6:
                        exceptions.append(struct.unpack_from('<I', dump, offset + 8)[0])
                self.assertEqual(exceptions, [0xe0424223])


if __name__ == '__main__':
    unittest.main()
