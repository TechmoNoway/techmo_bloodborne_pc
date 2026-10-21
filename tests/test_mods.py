from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import run_windows

spec = importlib.util.spec_from_file_location('bbmods', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        self.assets = self.game / 'dvdroot_ps4' / 'chr'
        self.assets.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'original executable')
        (self.assets / 'a.dcx').write_bytes(b'original')
        (self.assets / 'b.dcx').write_bytes(b'untouched')
        self.moddir = self.root / 'mods'

    def mod(self, name, contents=b'mod', file='a.dcx'):
        root = self.moddir / name
        folder = root / 'dvdroot_ps4' / 'chr'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / file).write_bytes(contents)
        return root

    def test_merge_replacement_new_file_and_directory_listing(self):
        a = self.mod('A')
        self.mod('A', b'new', 'new.dcx')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'mod')
        self.assertEqual((folder / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual((folder / 'new.dcx').read_bytes(), b'new')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx', 'new.dcx'})
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertFalse((self.assets / 'new.dcx').exists())
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')

    def test_last_mod_wins_and_reordering_changes_winner(self):
        a, b = self.mod('A', b'A'), self.mod('B', b'B')
        for layers, winner in [([('A', a), ('B', b)], b'B'), ([('B', b), ('A', a)], b'A')]:
            result = mods.build_overlay(self.game, self.root / 'out', layers)
            self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), winner)

    def test_selection_disable_and_new_mod(self):
        for name in ['C', 'B', 'A']:
            self.mod(name)
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['B', 'A', 'deleted'], 'disabled': ['A']}))
        self.assertEqual(mods.selected(self.moddir, config), ['B', 'C'])

    def test_no_mod_returns_original_game(self):
        self.assertEqual(mods.build_overlay(self.game, self.root / 'out', []), self.game)
        self.assertFalse((self.root / 'out').exists())

    def test_directory_conflict_does_not_modify_base(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/chr/a.dcx').unlink()
        nested = a / 'dvdroot_ps4/chr/b.dcx'
        nested.mkdir()
        (nested / 'child').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((self.assets / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual(list((self.root / 'out').iterdir()), [])

    def test_mod_symlinks_rejected(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/escape').symlink_to(self.assets, target_is_directory=True)
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_executable_replacement_rejected(self):
        a = self.mod('A')
        (a / 'eboot.bin').write_bytes(b'unsupported')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_shadps4_sibling_folder_and_global_disable(self):
        legacy = Path(str(self.game) + '-mods')
        folder = legacy / 'dvdroot_ps4/chr'
        folder.mkdir(parents=True)
        (folder / 'a.dcx').write_bytes(b'legacy')
        for enabled, expected in [('1', b'legacy'), ('0', b'original')]:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir), '--enabled', enabled],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'dvdroot_ps4/chr/a.dcx').read_bytes(), expected)

    def test_app0_wrapped_mod_discovered(self):
        self.moddir.mkdir()
        (self.moddir / 'Wrapped/app0/dvdroot_ps4').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Wrapped'])

    def test_other_case_replaces_the_game_file(self):
        root = self.moddir / 'Upper'
        (root / 'DVDROOT_PS4/Chr').mkdir(parents=True)
        (root / 'DVDROOT_PS4/Chr/A.DCX').write_bytes(b'upper')
        result = mods.build_overlay(self.game, self.root / 'out', [('Upper', root)])
        folder = result / 'dvdroot_ps4/chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'upper')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx'})
        self.assertEqual({p.name for p in result.iterdir()}, {'dvdroot_ps4', 'eboot.bin'})

    def test_wrapped_and_bare_layouts(self):
        layouts = {'Title': 'CUSA03173/dvdroot_ps4/chr', 'Archive': 'Archive v1.2/dvdroot_ps4/chr',
                   'Bare': 'chr', 'Nested': 'Nested/app0/dvdroot_ps4/chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Archive/readme.txt').write_text('notes')
        (self.moddir / 'Junk/stuff').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Archive', 'Bare', 'Nested', 'Title'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())

    def test_invalid_profile_fails(self):
        self.mod('A')
        config = self.root / 'mods.json'
        config.write_text('{"disabled":"A"}')
        with self.assertRaises(ValueError):
            mods.selected(self.moddir, config)

    def test_read_only_catalog_uses_runtime_layout_and_reports_bad_entries(self):
        self.mod('Costume', file='a.dcx')
        self.mod('Lighting', file='a.dcx')
        self.mod('Lighting', file='new.dcx')
        (self.moddir / 'NotAMod/stuff').mkdir(parents=True)
        (self.moddir / 'Download.zip').write_bytes(b'not extracted')
        catalog = mods.inspect_mods(self.game, self.moddir)
        rows = {row['name']: row for row in catalog['mods']}
        self.assertEqual(rows['Costume']['files'], ['dvdroot_ps4/chr/a.dcx'])
        self.assertEqual(set(rows['Lighting']['files']), {'dvdroot_ps4/chr/a.dcx', 'dvdroot_ps4/chr/new.dcx'})
        self.assertEqual(rows['NotAMod']['error'], 'layout')
        self.assertEqual(rows['Download.zip']['error'], 'archive')
        self.assertFalse((self.root / 'out').exists())
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
            '--mods-dir', str(self.moddir), '--inspect'], capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(result.stdout), catalog)

    def test_catalog_direct_and_legacy_match_runtime_and_can_be_disabled(self):
        root = self.moddir / 'dvdroot_ps4/chr'
        root.mkdir(parents=True)
        (root / 'a.dcx').write_bytes(b'direct')
        legacy = Path(str(self.game) + '-mods') / 'dvdroot_ps4/chr'
        legacy.mkdir(parents=True)
        (legacy / 'a.dcx').write_bytes(b'legacy')
        catalog = mods.inspect_mods(self.game, self.moddir)
        self.assertTrue(catalog['direct'])
        self.assertEqual([row['kind'] for row in catalog['mods']], ['legacy', 'direct'])
        config = self.root / 'mods.json'
        for direct, sibling, expected in [(True, True, b'direct'), (False, True, b'legacy'), (False, False, b'original')]:
            config.write_text(json.dumps({'direct_enabled': direct, 'legacy_enabled': sibling}))
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--mods-dir', str(self.moddir), '--out', str(self.root / 'out'), '--config', str(config)],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'dvdroot_ps4/chr/a.dcx').read_bytes(), expected)

    def test_disabled_invalid_link_does_not_block_other_mods(self):
        self.mod('Good')
        linked = self.moddir / 'Linked'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(self.moddir / 'Good'), str(linked))
        else:
            linked.symlink_to(self.moddir / 'Good', target_is_directory=True)
        catalog = mods.inspect_mods(self.game, self.moddir)
        self.assertEqual(next(row for row in catalog['mods'] if row['name'] == 'Linked')['error'], 'invalid')
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['Linked', 'Good'], 'disabled': ['Linked']}))
        self.assertEqual(mods.selected(self.moddir, config), ['Good'])
        result = mods.build_overlay(self.game, self.root / 'out', [('Good', self.moddir / 'Good')])
        self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')
        mods.remove_overlay(linked)

    def test_catalog_rejects_nested_junction_and_executable_replacement(self):
        bad = self.mod('Executable')
        (bad / 'eboot.bin').write_bytes(b'unsupported')
        linked = self.mod('NestedLink') / 'dvdroot_ps4/escape'
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(self.assets), str(linked))
        else:
            linked.symlink_to(self.assets, target_is_directory=True)
        try:
            rows = mods.inspect_mods(self.game, self.moddir)['mods']
            self.assertTrue(all(row['error'] == 'invalid' for row in rows))
            self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        finally:
            mods.remove_overlay(linked)

    def test_invalid_config_is_preserved_and_global_disable_bypasses_it(self):
        self.mod('A')
        config = self.root / 'mods.json'
        for content in ['[]', '{"order":null}', '{"legacy_enabled":1}', '{bad']:
            config.write_text(content)
            with self.assertRaises((ValueError, TypeError)):
                mods.load_config(config)
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--mods-dir', str(self.moddir), '--out', str(self.root / 'out'), '--config', str(config), '--enabled', '0'],
                capture_output=True, text=True, check=True)
            self.assertEqual(Path(result.stdout.strip()), self.game)
            self.assertEqual(config.read_text(), content)

    def test_run_uses_overlay_propagates_exit_and_cleans_view(self):
        self.mod('A')
        mounted = {}
        def prepare(arguments, capture=False, check=True, env=None):
            if Path(arguments[1]).name == 'mods.py':
                return str(mods.build_overlay(self.game, self.root / 'out', [('A', self.moddir / 'A')]))
            return '' if capture else 0
        def probe(command, **kwargs):
            game = Path(command[command.index('--app0') + 1])
            mounted.update(path=str(game), content=(game / 'dvdroot_ps4/chr/a.dcx').read_text())
            return 7
        env = dict(BB_PREBUILT='1', BB_DATA_DIR=str(self.root), BB_GAME_DIR=str(self.game),
                   BB_MODS_DIR=str(self.moddir), BB_MODS_ENABLED='1', BB_CONFIG=str(self.root / 'bbport.ini'))
        (self.root / 'out').mkdir(exist_ok=True)
        (self.root / 'out/bb-probe.exe').write_bytes(b'synthetic runtime fixture')
        with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', ['run_windows.py']), \
             patch.object(run_windows, 'run', side_effect=prepare), \
             patch.object(run_windows, 'build') as build, \
             patch('memory_preflight.configure_memory', return_value=(None,None)), \
             patch.object(run_windows.subprocess, 'call', side_effect=probe):
            self.assertEqual(run_windows.main(), 7)
            build.assert_not_called()
        self.assertEqual(mounted['content'], 'mod')
        self.assertFalse(Path(mounted['path']).exists())
        self.assertEqual((self.assets/'a.dcx').read_bytes(), b'original')

    def test_controller_only_startup_still_prepares_title_quit_menu(self):
        import engine_assets
        config = self.root / 'bbport.ini'
        config.write_text(''.join(f'{key}=0\n' for key in
                          ('pc_controls', 'widescreen', 'camera_controls', 'graphics_controls',
                           'change_appearance', 'rebirth')))
        (self.root / 'out').mkdir()
        (self.root / 'out/bb-probe.exe').write_bytes(b'synthetic runtime fixture')
        menus = self.root / 'engine-assets/pc-menus'

        def prepare(arguments, capture=False, check=True, env=None):
            if Path(arguments[1]).name == 'mods.py':
                return str(self.game)
            return '' if capture else 0

        def probe(command, **kwargs):
            self.assertEqual(os.environ['BB_PC_MENU_ASSETS'], str(menus))
            self.assertEqual(os.environ['BB_ENGINE_IMAGE_SHA256'], 'verified-image')
            return 0

        env = dict(BB_PREBUILT='1', BB_DATA_DIR=str(self.root), BB_GAME_DIR=str(self.game),
                   BB_CONFIG=str(config))
        with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', ['run_windows.py']), \
             patch.object(run_windows, 'run', side_effect=prepare), \
             patch.object(run_windows.subprocess, 'call', side_effect=probe), \
             patch('memory_preflight.configure_memory', return_value=(None,None)), \
             patch.object(engine_assets, 'verify_executable', return_value='verified-image'), \
             patch.object(engine_assets, 'prepare_menu_assets', return_value=menus) as prepare_menus:
            self.assertEqual(run_windows.main(), 0)
            prepare_menus.assert_called_once_with(self.game, self.root / 'out/eboot.elf',
                self.root / 'engine-assets', self.root / 'out/bb-engine-assets.exe', rebirth=False)
