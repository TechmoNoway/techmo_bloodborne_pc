from paths import ROOT
import hashlib
import json
import tempfile
import unittest
import importlib.util
from unittest.mock import patch
from pathlib import Path
from native_identity import identify_runtime
from session_diagnostics import Session, classify_exit

spec = importlib.util.spec_from_file_location('native_build_identity', ROOT / 'tools/write_native_build_info.py')
build_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_info)


class RuntimeIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / 'folder labelled old version' / 'bb-probe.exe'
        self.runtime.parent.mkdir()
        self.runtime.write_bytes(b'actual new runtime')

    def manifest(self, data):
        (self.runtime.parent / 'native-build.json').write_text(json.dumps(data))

    def test_actual_hash_matches_selected_runtime_manifest_independent_of_data_root(self):
        digest = hashlib.sha256(self.runtime.read_bytes()).hexdigest()
        self.manifest({'binaries': {'bb-probe.exe': {'sha256': digest}},
                       'source_identity': {'app_version': '1.3.0', 'commit': 'abc', 'dirty': True}})
        session = Session(self.root / 'different data root', {})
        identity = session.record_runtime(self.runtime)
        self.assertTrue(identity['manifest_match'])
        self.assertEqual(identity['sha256'], digest)
        self.assertEqual(identity['build_identity']['app_version'], '1.3.0')
        self.assertEqual(session.report['native_build']['binaries']['bb-probe.exe']['sha256'], digest)
        self.runtime.write_bytes(b'replaced without updating manifest')
        identity = session.record_runtime(self.runtime)
        self.assertFalse(identity['manifest_match'])
        self.assertEqual(identity['manifest_status'], 'mismatch')
        self.assertNotIn('build_identity', identity)

    def test_missing_malformed_and_unrecorded_metadata_do_not_invent_version(self):
        for metadata in (None, [], {'binaries': []}, {'binaries': {'bb-probe.exe': None}}):
            if metadata is not None: self.manifest(metadata)
            identity, _ = identify_runtime(self.runtime)
            self.assertTrue(identity['available'])
            self.assertIsNone(identity['manifest_match'])
        self.runtime.unlink()
        identity, _ = identify_runtime(self.runtime)
        self.assertFalse(identity['available'])

    def test_empty_fault_and_reporter_failure_are_preserved_in_session(self):
        session = Session(self.root, {})
        session.set_phase('runtime')
        (session.directory / 'fault.json').write_bytes(b'')
        status = {'startup_error': 2, 'reporter_ready': False, 'phase': 'initialized'}
        (session.directory / 'diagnostics.json').write_text(json.dumps(status))
        session.finish(0xc0000005)
        self.assertEqual(session.report['crash_evidence']['fault_record'], 'empty')
        self.assertFalse(session.report['crash_evidence']['dump_present'])
        self.assertEqual(session.report['diagnostics'], status)
        self.assertEqual(session.report['outcome']['evidence'], 'process exit code only')
        self.assertIn('Fault instruction/cause unavailable', (session.directory / 'summary.txt').read_text())

    def test_source_archive_does_not_borrow_a_parent_repositories_commit(self):
        project = self.root / 'apps/launcher/Techmo.Launcher.csproj'
        project.parent.mkdir(parents=True)
        project.write_text('<Project><PropertyGroup><Version>1.3.0</Version></PropertyGroup></Project>')
        with patch.object(build_info.subprocess, 'check_output', return_value=str(ROOT)) as git:
            identity = build_info.source_identity(self.root)
        self.assertEqual(identity['app_version'], '1.3.0')
        self.assertFalse(identity['git_available'])
        self.assertNotIn('commit', identity)
        self.assertEqual(git.call_count, 1)

    def test_exception_evidence_keeps_exit_codes_separate_from_context(self):
        record = classify_exit(139, 'runtime', {'exception_code': '0xc0000005'})
        logged = classify_exit(139, 'runtime', log_tail='Host fault 0xc0000005 at unknown')
        exit_only = classify_exit(-1073741819, 'runtime')
        self.assertEqual(record['evidence'], 'native exception record')
        self.assertEqual(logged['evidence'], 'native exception log')
        self.assertEqual(exit_only['evidence'], 'process exit code only')
        self.assertEqual(exit_only['reason'], 'access_violation')
        self.assertNotIn('instruction_address', exit_only)
