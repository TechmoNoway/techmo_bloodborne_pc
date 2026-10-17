from paths import ROOT
import unittest
import struct
import subprocess
import engine_assets
import tempfile
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace


class EngineAssetTests(unittest.TestCase):
    def test_dcx_roundtrip(self):
        raw = b'MSB fixture\0' * 500
        packed = engine_assets.dcx_pack(raw)
        self.assertEqual(len(packed[:0x4c]), 0x4c)
        self.assertEqual(engine_assets.dcx_unpack(packed), raw)

    def test_bad_size_or_truncated_stream_is_rejected(self):
        packed = bytearray(engine_assets.dcx_pack(b'unchanged source' * 20))
        struct.pack_into('>I', packed, 0x1c, 1)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(packed)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(engine_assets.dcx_pack(b'valid')[:-4])

    def test_wrong_layout_does_not_mutate_input(self):
        raw = bytearray(b'not a supported Dream layout')
        before = bytes(raw)
        with self.assertRaisesRegex(ValueError, 'original 1.09'):
            engine_assets.mirror_layout(raw)
        self.assertEqual(raw, before)

    def test_menu_cache_rebuilds_corrupt_output_and_changed_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            movie = game / 'dvdroot_ps4/menu/optionsetting.gfx'
            messages = game / 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'
            for p in (movie, messages):
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'own dump')
            tool = root / 'tool'
            tool.write_bytes(b'generator version 1')
            calls = []
            def generate(args, **kwargs):
                calls.append(args[1])
                Path(args[3]).write_bytes(Path(args[2]).read_bytes() + b' prepared')
                return SimpleNamespace(returncode=0, stderr='')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run', generate):
                cache = engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                cached = cache / movie.relative_to(game)
                cached.write_bytes(b'corrupt')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 3)
                self.assertEqual(movie.read_bytes(), b'own dump')
                messages.write_bytes(b'updated language')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 4)
                tool.write_bytes(b'generator version 2')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 6)

    def test_title_movies_get_the_quit_game_slot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            for name in ('menu/optionsetting.gfx', 'msg/engus/menu.msgbnd.dcx', 'menu/title.gfx', 'menu/title_dlc.gfx',
                         'menu/title_dlc_eu.gfx'):
                p = game / 'dvdroot_ps4' / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'own dump')
            tool = root / 'tool'
            tool.write_bytes(b'tool')
            calls = []
            def generate(args, **kwargs):
                calls.append((args[1], Path(args[2]).name))
                Path(args[3]).write_bytes(b'prepared')
                return SimpleNamespace(returncode=0, stderr='')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run', generate):
                cache = engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
            self.assertIn(('title', 'title.gfx'), calls)
            self.assertIn(('title', 'title_dlc.gfx'), calls)
            self.assertIn(('title', 'title_dlc_eu.gfx'), calls)
            self.assertTrue((cache / 'dvdroot_ps4/menu/title.gfx').is_file())

    def test_title_movie_failure_does_not_publish_a_partial_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            originals = {}
            for name in ('menu/optionsetting.gfx', 'msg/engus/menu.msgbnd.dcx',
                         'menu/title.gfx', 'menu/title_dlc_eu.gfx'):
                source = game / 'dvdroot_ps4' / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b'original ' + name.encode())
                originals[source] = source.read_bytes()
            tool = root / 'tool'
            tool.write_bytes(b'generator')

            def generate(args, **kwargs):
                if Path(args[2]).name == 'title_dlc_eu.gfx':
                    return SimpleNamespace(returncode=1, stderr='unsupported EU title')
                Path(args[3]).write_bytes(b'prepared')
                return SimpleNamespace(returncode=0, stderr='')

            with patch.object(engine_assets, 'verify_executable'), \
                 patch.object(engine_assets.subprocess, 'run', generate):
                with self.assertRaisesRegex(ValueError, 'unsupported EU title'):
                    engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
            self.assertFalse((root / 'cache/pc-menus/manifest.json').exists())
            self.assertFalse(list((root / 'cache').rglob('*.tmp')))
            for source, original in originals.items():
                self.assertEqual(source.read_bytes(), original)

    def test_menu_generation_failure_does_not_publish_manifest_or_change_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            for name in ('menu/optionsetting.gfx', 'msg/engus/menu.msgbnd.dcx'):
                p = game / 'dvdroot_ps4' / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'original')
            tool = root / 'tool'
            tool.write_bytes(b'tool')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run',
                    return_value=SimpleNamespace(returncode=1, stderr='unsupported movie')):
                with self.assertRaisesRegex(ValueError, 'unsupported movie'):
                    engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
            self.assertFalse((root / 'cache/pc-menus/manifest.json').exists())
            self.assertTrue(all(p.read_bytes() == b'original' for p in game.rglob('*') if p.is_file()))


@unittest.skipUnless((ROOT / 'out/bb-engine-assets.exe').is_file(), 'build bb-engine-assets first')
class TitleMovieGeneratorTests(unittest.TestCase):
    @staticmethod
    def tag(code, body=b''):
        if len(body) < 63:
            return struct.pack('<H', code << 6 | len(body)) + body
        return struct.pack('<HI', code << 6 | 63, len(body)) + body

    @classmethod
    def row(cls, index, depth, y):
        # Scaleform MATRIX: no scale/rotation, signed translate x=0 and y in twips.
        width = max(1, y.bit_length() + 1)
        bits = '00' + f'{width:05b}' + '0' * width + f'{y:0{width}b}'
        bits += '0' * (-len(bits) % 8)
        matrix = int(bits, 2).to_bytes(len(bits) // 8, 'big')
        return cls.tag(26, struct.pack('<BHH', 0x26, depth, 7) + matrix +
                       f'Item_{index}_0'.encode() + b'\0')

    @classmethod
    def movie(cls, list_name=b'CommandList'):
        end = cls.tag(1) + cls.tag(0)
        rows = b''.join(cls.row(i, 21 - i * 5, i * 800) for i in range(5))
        command_list = cls.tag(39, struct.pack('<HH', 32, 1) + rows + end)
        container = cls.tag(39, struct.pack('<HH', 40, 1) +
                            cls.tag(26, struct.pack('<BHH', 0x22, 1, 32) + list_name + b'\0') + end)
        body = b'\x08\0' + struct.pack('<HH', 24 << 8, 1) + command_list + container + end
        return b'GFX\x09' + struct.pack('<I', len(body) + 8) + body

    def test_native_generator_adds_sixth_row_with_original_pitch_and_unique_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / 'title.gfx', Path(temporary) / 'prepared.gfx'
            original = self.movie()
            source.write_bytes(original)
            result = subprocess.run([str(ROOT / 'out/bb-engine-assets.exe'), 'title', str(source), str(output)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            prepared = output.read_bytes()
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(struct.unpack_from('<I', prepared, 4)[0], len(prepared))
            for i in range(5):
                self.assertIn(self.row(i, 21 - i * 5, i * 800), prepared)
            self.assertEqual(prepared.count(b'Item_5_0\0'), 1)
            self.assertIn(self.row(5, 22, 4000), prepared)

    def test_missing_command_list_fails_without_writing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / 'title.gfx', Path(temporary) / 'prepared.gfx'
            original = self.movie(b'OtherList')
            source.write_bytes(original)
            result = subprocess.run([str(ROOT / 'out/bb-engine-assets.exe'), 'title', str(source), str(output)],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('CommandList', result.stderr)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
