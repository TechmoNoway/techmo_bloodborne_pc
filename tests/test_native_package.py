from paths import ROOT
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('verify_native_package', ROOT / 'tools/verify_native_package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class NativePackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.out = Path(temporary.name) / 'portable package' / 'out'
        self.out.mkdir(parents=True)
        self.manifest = {'schema_version': 1, 'binaries': {}, 'runtime_dlls': {}}
        for name in package.REQUIRED_BINARIES | {'SDL3.dll', 'libc++.dll', 'libunwind.dll'}:
            data = name.encode()
            (self.out / name).write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            if name.endswith('.exe'):
                self.manifest['binaries'][name] = {'sha256': digest}
            else:
                self.manifest['runtime_dlls'][name] = digest
        self.write_manifest()

    def write_manifest(self):
        (self.out / 'native-build.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def test_complete_package(self):
        self.assertEqual(package.verify(self.out), (4, 3))

    def test_missing_sdl(self):
        (self.out / 'SDL3.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'missing native dependency: SDL3.dll'):
            package.verify(self.out)

    def test_missing_transitive_dll(self):
        (self.out / 'libunwind.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'missing native dependency: libunwind.dll'):
            package.verify(self.out)

    def test_mismatched_dll(self):
        (self.out / 'SDL3.dll').write_bytes(b'wrong DLL version')
        with self.assertRaisesRegex(ValueError, 'differs from build manifest: SDL3.dll'):
            package.verify(self.out)

    def test_manifest_without_sdl(self):
        del self.manifest['runtime_dlls']['SDL3.dll']
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, 'manifest is missing SDL3.dll'):
            package.verify(self.out)

    def test_missing_crash_reporter_record(self):
        del self.manifest['binaries']['bb-crash-reporter.exe']
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, 'missing binaries'):
            package.verify(self.out)


if __name__ == '__main__':
    unittest.main()
