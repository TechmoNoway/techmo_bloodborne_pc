"""The custom frontend keeps upstream engine-setting semantics and a narrow input patch."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BackendParityTests(unittest.TestCase):
    def test_engine_choices_and_defaults_are_upstream(self):
        for name in ('Choices.cs', 'EngineSwitches.cs', 'GameEnvironment.cs', 'IniFile.cs', 'LauncherSettings.cs'):
            with self.subTest(name=name):
                upstream = (ROOT / 'launcher/windows/BloodborneLauncher/Core' / name).read_text(encoding='utf-8-sig')
                hosted = (ROOT / 'apps/lantern-backend/Core' / name).read_text(encoding='utf-8-sig')
                self.assertEqual(hosted.replace('Techmo.Lantern', 'BloodborneLauncher'), upstream)

    def test_native_compatibility_patch_only_touches_input_and_mod_loading(self):
        patch = (ROOT / 'engine/techmo-compat.patch').read_text(encoding='utf-8')
        paths = set(re.findall(r'^\+\+\+ b/(.+)$', patch, re.MULTILINE))
        self.assertEqual(paths, {'gpu/shim/input/bindings.cpp', 'gpu/shim/input/bindings.h',
                                 'gpu/shim/input/input.cpp', 'start/mods.cpp'})

    def test_techmo_capture_and_key_labels_are_preserved(self):
        # The renderer/configuration adapter translates SDL numbering, leaving the
        # user's existing capture UX and keyboard/mouse slot representation intact.
        reference = ROOT.parent / 'techmo_launcher/apps/launcher'
        for name in ('InputKeyNames.cs', 'MainWindow.InputCapture.cs', 'MainWindow.Input.cs'):
            with self.subTest(name=name):
                self.assertEqual((ROOT / 'apps/launcher' / name).read_bytes(), (reference / name).read_bytes())
