from paths import ROOT
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import shutil
from unittest.mock import patch
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

    def isolated_fixture(self, helper=None):
        directory = self.session / 'isolated runtime'
        directory.mkdir()
        original = self.fixture
        self.fixture = directory / original.name
        shutil.copy2(original, self.fixture)
        if helper:
            shutil.copy2(original, directory / 'bb-crash-reporter.exe')
        return directory

    def test_real_write_fault_on_main_and_worker_captures_context(self):
        for mode in ('--write', '--worker', '--guest'):
            with self.subTest(mode=mode):
                result = self.invoke(mode)
                self.assertEqual(result.returncode, 139, result.stderr)
                fault = json.loads((self.session / 'fault.json').read_text())
                self.assertEqual(fault['access_kind'], 'write')
                self.assertNotEqual(int(fault['instruction_address'], 16), 0)
                self.assertNotEqual(int(fault['registers']['rsp'], 16), 0)
                self.assertEqual(len(fault['instruction_bytes']), 32)
                self.assertEqual(fault['origin'], 'guest' if mode == '--guest' else 'host')
                state = json.loads((self.session / 'diagnostics.json').read_text())
                self.assertEqual(state['dump_state'], 'written')
                self.assertEqual(state['fault_write_error'], 0)
                dump = (self.session / 'crash.dmp').read_bytes()
                streams, directory = struct.unpack_from('<II', dump, 8)
                contexts = []
                for index in range(streams):
                    kind, _, offset = struct.unpack_from('<III', dump, directory + index * 12)
                    if kind == 6:
                        size, rva = struct.unpack_from('<II', dump, offset + 160)
                        self.assertGreaterEqual(size, 256)
                        contexts.append((struct.unpack_from('<Q', dump, rva + 248)[0],
                                         struct.unpack_from('<Q', dump, rva + 152)[0]))
                self.assertEqual(contexts, [(int(fault['instruction_address'], 16),
                                             int(fault['registers']['rsp'], 16))])

    def test_missing_reporter_still_writes_fault_and_reason(self):
        self.isolated_fixture()
        result = self.invoke('--write')
        self.assertEqual(result.returncode, 139, result.stderr)
        self.assertEqual(json.loads((self.session / 'fault.json').read_text())['access_kind'], 'write')
        state = json.loads((self.session / 'diagnostics.json').read_text())
        self.assertFalse(state['reporter_ready'])
        self.assertEqual(state['startup_stage'], 'reporter_path')
        self.assertNotEqual(state['startup_error'], 0)
        self.assertEqual(state['dump_state'], 'reporter_unavailable')

    def test_fault_file_open_failure_keeps_remote_dump(self):
        (self.session / 'fault.json').mkdir()
        result = self.invoke('--write')
        self.assertEqual(result.returncode, 139, result.stderr)
        state = json.loads((self.session / 'diagnostics.json').read_text())
        self.assertNotEqual(state['fault_open_error'], 0)
        self.assertNotEqual(state['fault_write_error'], 0)
        self.assertEqual(state['dump_state'], 'written')

    def test_fault_error_does_not_wait_for_another_threads_stderr_lock(self):
        (self.session / 'fault.json').mkdir()
        result = self.invoke('--locked-stderr')
        self.assertEqual(result.returncode, 139, result.stderr)
        state = json.loads((self.session / 'diagnostics.json').read_text())
        self.assertNotEqual(state['fault_write_error'], 0)
        self.assertEqual(state['dump_state'], 'written')

    def test_dump_status_file_failure_does_not_hide_a_successful_dump(self):
        (self.session / 'minidump.json').mkdir()
        result = self.invoke('--write')
        self.assertEqual(result.returncode, 139, result.stderr)
        state = json.loads((self.session / 'diagnostics.json').read_text())
        self.assertEqual(state['dump_state'], 'written')
        self.assertNotEqual(state['status_error'], 0)
        self.assertEqual((self.session / 'crash.dmp').read_bytes()[:4], b'MDMP')

    def test_dump_file_failure_is_acknowledged(self):
        (self.session / 'crash.dmp').mkdir()
        result = self.invoke('--write')
        self.assertEqual(result.returncode, 139, result.stderr)
        status = json.loads((self.session / 'minidump.json').read_text())
        self.assertFalse(status['success'])
        self.assertNotEqual(status['error_code'], 0)
        state = json.loads((self.session / 'diagnostics.json').read_text())
        self.assertEqual(state['dump_state'], 'failed')
        self.assertEqual(state['dump_error'], status['error_code'])

    def test_reporter_exit_startup_error_timeout_and_death_during_dump(self):
        self.isolated_fixture(helper=True)
        for mode, stage, dump_state in (
                ('exit', 'reporter_exited_before_ready', 'reporter_unavailable'),
                ('startup-error', 'handshake', 'reporter_unavailable'),
                ('timeout', 'handshake', 'reporter_unavailable'),
                ('dump-exit', 'ready', 'reporter_exited')):
            with self.subTest(mode=mode), patch.dict(os.environ, BB_DIAGNOSTIC_FIXTURE_HELPER=mode):
                result = self.invoke('--write')
                self.assertEqual(result.returncode, 139, result.stderr)
                state = json.loads((self.session / 'diagnostics.json').read_text())
                self.assertEqual(state['startup_stage'], stage)
                self.assertEqual(state['dump_state'], dump_state)
                if mode != 'dump-exit': self.assertNotEqual(state['startup_error'], 0)


@unittest.skipUnless(os.name == 'nt', 'Windows renderer assertion reporter')
class RendererAssertionTests(NativeCrashReporterTests):
    def setUp(self):
        super().setUp()
        self.fixture = self.fixture.with_name('renderer-assert-test.exe')
        if not self.fixture.is_file():
            self.skipTest('Compile renderer-assert-test.exe first.')

    # The renderer fixture always asserts; the inherited normal/fault cases are in the C fixture.
    test_fault_produces_remote_minidump_with_original_exception = None
    test_normal_exit_does_not_create_dump = None
    test_synthetic_fatal_escapes_reason_and_writes_dump = None
    test_real_write_fault_on_main_and_worker_captures_context = None
    test_missing_reporter_still_writes_fault_and_reason = None
    test_fault_file_open_failure_keeps_remote_dump = None
    test_fault_error_does_not_wait_for_another_threads_stderr_lock = None
    test_dump_status_file_failure_does_not_hide_a_successful_dump = None
    test_dump_file_failure_is_acknowledged = None
    test_reporter_exit_startup_error_timeout_and_death_during_dump = None

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
