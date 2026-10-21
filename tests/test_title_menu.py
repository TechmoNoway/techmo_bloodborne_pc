"""Exercise the native title-asset tool with synthetic movies, without a game dump."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def tag(code, body=b''):
    if len(body) < 63:
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 63, len(body)) + body


def matrix(x, y):
    width = max(abs(x).bit_length(), abs(y).bit_length()) + 1
    bits = '00' + f'{width:05b}' + f'{x & ((1 << width)-1):0{width}b}' + f'{y & ((1 << width)-1):0{width}b}'
    bits += '0' * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, 'big')


def place(name, depth, cid, y=0):
    return tag(26, struct.pack('<BHH', 0x26, depth, cid) + matrix(0, y) + name.encode() + b'\0')


def movie(list_id=32, list_name='CommandList'):
    rows = b''.join(place(f'Item_{i}_0', 21-i*5, 7, i*800) for i in range(5))
    sprite = tag(39, struct.pack('<HH', list_id, 1) + rows + tag(1) + tag(0))
    parent = tag(39, struct.pack('<HH', list_id+1, 1) + place(list_name, 1, list_id) + tag(1) + tag(0))
    raw = bytearray(b'FWS\x09' + b'\0'*4 + b'\x08\x00' + struct.pack('<HH', 24*256, 1) + sprite + parent + tag(1) + tag(0))
    struct.pack_into('<I', raw, 4, len(raw))
    return bytes(raw)


def tags(raw, start, end):
    while start < end:
        code, = struct.unpack_from('<H', raw, start)
        start += 2
        size = code & 63
        code >>= 6
        if size == 63:
            size, = struct.unpack_from('<I', raw, start)
            start += 4
        if start + size > len(raw):
            raise ValueError('Tag outside movie')
        yield code, start, start+size
        if code == 0:
            break
        start += size


def rows(raw, list_id):
    start = 8 + (5 + 4*(raw[8] >> 3) + 7)//8 + 4
    for code, begin, end in tags(raw, start, len(raw)):
        if code != 39 or struct.unpack_from('<H', raw, begin)[0] != list_id:
            continue
        result = []
        for code, begin, end in tags(raw, begin+4, end):
            if code != 26:
                continue
            flags, depth, cid = struct.unpack_from('<BHH', raw, begin)
            if flags != 0x26:
                raise ValueError('Unexpected fixture flags')
            bits = ''.join(f'{byte:08b}' for byte in raw[begin+5:end])
            if bits[:2] != '00':
                raise ValueError('Unexpected scale/rotation')
            width = int(bits[2:7], 2)
            x = int(bits[7:7+width], 2)
            y = int(bits[7+width:7+width*2], 2)
            x -= (1 << width) if x & (1 << (width-1)) else 0
            y -= (1 << width) if y & (1 << (width-1)) else 0
            offset = begin+5+(7+width*2+7)//8
            name = raw[offset:raw.index(0, offset, end)].decode()
            result.append((name, depth, cid, x, y))
        return result
    raise ValueError('Missing command-list sprite')


class MenuPatchScopeTests(unittest.TestCase):
    def test_menu_patch_is_separate_from_renderer_and_input_changes(self):
        patch = (ROOT / 'engine/techmo-menu.patch').read_text(encoding='utf-8')
        paths = set(re.findall(r'^\+\+\+ b/(.+)$', patch, re.MULTILINE))
        self.assertEqual(paths, {'gpu/shim/engine/option_menu.cpp', 'tools/engine/menu_assets.cpp',
                                 'tools/engine/pc_option_messages.tsv', 'start/assets.cpp', 'start/main.cpp'})


class TitleMenuAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = ROOT / 'out/bb-engine-assets.exe'
        if not cls.tool.is_file():
            raise unittest.SkipTest('Native asset tool is exercised by the native CI job')

    def run_tool(self, source, destination):
        return subprocess.run([str(self.tool), 'title', str(source), str(destination)],
                              capture_output=True, text=True, timeout=20)

    def test_sixth_row_preserves_the_original_five_for_all_title_variants(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, list_id in [('title.gfx', 32), ('title_dlc.gfx', 48), ('title_dlc_eu.gfx', 57)]:
                with self.subTest(movie=name):
                    original = movie(list_id)
                    source, output = root / name, root / (name+'.cached')
                    source.write_bytes(original)
                    result = self.run_tool(source, output)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    prepared = output.read_bytes()
                    before, after = rows(original, list_id), rows(prepared, list_id)
                    self.assertEqual(after[:5], before)
                    self.assertEqual(len(after), 6)
                    self.assertEqual(after[5][0], 'Item_5_0')
                    self.assertEqual(after[5][2:], (7, 0, 4000))
                    self.assertNotIn(after[5][1], [row[1] for row in before])
                    self.assertEqual(struct.unpack_from('<I', prepared, 4)[0], len(prepared))
                    self.assertEqual(source.read_bytes(), original)

    def test_unsupported_movie_is_not_written_or_modified(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory)/'title.gfx', Path(directory)/'cache.gfx'
            original = movie(list_name='UnrelatedList')
            source.write_bytes(original)
            result = self.run_tool(source, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), original)

    def test_source_cannot_be_used_as_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'title.gfx'
            original = movie()
            source.write_bytes(original)
            self.assertNotEqual(self.run_tool(source, source).returncode, 0)
            self.assertEqual(source.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
