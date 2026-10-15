"""Red-zone emitter, linker and real Windows write-fault regression checks."""
from paths import ROOT
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import link_modules
import red_zone
from test_probe import package


def sites(offset=16, body='8902488b4424a0', lengths=(-2, 5)):
    return red_zone.validate_sites([[offset, body, lengths]])


def linked_fixture(code, plan, enabled=True, patches=None, relocs=(), native_module=False, names=()):
    main = dict(identity=lambda name: None, libraries={}, modules={},
                ph=[dict(type=1, vaddr=0, memsz=4096, filesz=4096, flags=5),
                    dict(type=0x61000001, vaddr=256)])
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        (out / 'boot.bin').write_bytes(package(code, relocs=relocs, names=names, capabilities=1))
        if patches is not None:
            (out / 'patches.bin').write_bytes(patches)
        library = dict(elf=b'\x31\xc0\xc3'.ljust(4096, b'\0'),
                       ph=[dict(type=1, vaddr=0, offset=0, memsz=4096, filesz=4096, flags=5)],
                       tags={12: 0}, relocs=[], symbols=[], sha256='fixture')
        with patch.object(link_modules, 'module', side_effect=[main, library]), \
             patch.object(red_zone, 'load_sites', return_value=(plan, dict(functions=1, uncovered_stores=[]))), \
             contextlib.redirect_stdout(io.StringIO()):
            link_modules.link(Path('fixture'), out, ['fixture.prx'] if native_module else [], red_zones=enabled,
                              runtime_patches=out / 'patches.bin' if patches is not None else None)
        return (out / 'boot-linked.bin').read_bytes(), json.loads((out / 'link.json').read_text())


def patch_file(offset, body, version=2):
    header = struct.pack('<8sQQ', b'BBPATCH2', 0x400000, 1) if version == 2 else struct.pack('<8sQ', b'BBPATCH1', 1)
    return header + struct.pack('<QQ', offset, len(body)) + body


class RedZoneTests(unittest.TestCase):
    def test_reviewed_plan_integrity_and_crash_function_coverage(self):
        plan, metadata = red_zone.load_sites()
        self.assertEqual(len(plan), 607)
        self.assertEqual(metadata['functions'], 153)
        self.assertEqual(len(metadata['uncovered_stores']), 42)
        critical = next(site for site in plan if site[0] == 0x28cf4ce)
        self.assertEqual(sum(n < 0 for n in critical[2]), 24)
        with tempfile.TemporaryDirectory() as tmp:
            table = Path(tmp) / 'sites.json'
            table.write_bytes(red_zone.SITE_FILE.read_bytes() + b' ')
            with patch.object(red_zone, 'SITE_FILE', table), self.assertRaisesRegex(ValueError, 'reviewed'):
                red_zone.load_sites()

    def test_wraps_only_stores_preserving_stack_load_and_jump_targets(self):
        image = bytearray(64)
        image[16:23] = bytes.fromhex('8902488b4424a0')
        code, report = red_zone.protect(image, sites(), 65536, [(0, 64)])
        self.assertEqual(code[:15], red_zone.LEA_DOWN + b'\x89\x02' + red_zone.LEA_UP)
        self.assertEqual(code[15:20], bytes.fromhex('488b4424a0'))
        self.assertEqual(16 + 5 + struct.unpack_from('<i', image, 17)[0], 65536)
        self.assertEqual(65536 + 25 + struct.unpack_from('<i', code, 21)[0], 23)
        self.assertEqual(image[21:23], b'\xcc\xcc')
        self.assertEqual((report['applied_sites'], report['protected_stores']), (1, 1))
        self.assertEqual(len(code) % 16, 0)

    def test_mismatch_and_non_executable_bytes_are_untouched(self):
        for image, executable, key in [(bytearray(64), [(0, 64)], 'changed_sites'),
                                      (bytearray(64), [(0, 22)], 'outside_code_sites')]:
            if key == 'outside_code_sites':
                image[16:23] = sites()[0][1]
            before = bytes(image)
            code, report = red_zone.protect(image, sites(), 65536, executable)
            self.assertEqual(bytes(image), before)
            self.assertEqual(code, b'')
            self.assertEqual(report[key], ['0x10'])

    def test_patch_and_relocation_overlap_boundaries(self):
        for blocked, expected in [([(9, 8)], 0), ([(8, 8)], 1), ([(22, 8)], 0),
                                  ([(23, 8)], 1), ([(0, 16), (16, 1)], 0)]:
            with self.subTest(blocked=blocked):
                image = bytearray(64)
                image[16:23] = sites()[0][1]
                _, report = red_zone.protect(image, sites(), 65536, [(0, 64)], blocked)
                self.assertEqual(report['applied_sites'], expected)

    def test_failure_leaves_entire_image_unchanged(self):
        image = bytearray(64)
        image[16:23] = sites()[0][1]
        before = bytes(image)
        with self.assertRaisesRegex(ValueError, 'image limit'):
            red_zone.protect(image, sites(), red_zone.IMAGE_LIMIT - 1, [(0, 64)])
        self.assertEqual(bytes(image), before)

    def test_rejects_incomplete_unsafe_plan_structure(self):
        invalid = [[[16, '8902', [-2]]], [[16, '8902488b4424a0', [-2, 4]]],
                   [[16, '8902488b4424a0', [2, 5]]], [[-1, '8902488b4424a0', [-2, 5]]],
                   [[16, '8902488b4424a0', [-2, 5]], [20, '8902488b4424a0', [-2, 5]]],
                   [[16, 'not hex', [-2, 5]]], [[16, '8902488b4424a0', [-2, True]]]]
        for plan in invalid:
            with self.subTest(plan=plan), self.assertRaises(ValueError):
                red_zone.validate_sites(plan)

    def test_selected_patch_formats_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'patches.bin'
            for version in (1, 2):
                data = patch_file(16, b'\x90\x90', version)
                path.write_bytes(data)
                self.assertEqual(red_zone.patch_ranges(path), [(16, 2)])
                for invalid in (data[:-1], data + b'\0', data[:12], b'BADPATCH' + data[8:]):
                    path.write_bytes(invalid)
                    with self.assertRaises(ValueError):
                        red_zone.patch_ranges(path)
            path.write_bytes(struct.pack('<8sQQ', b'BBPATCH2', 0x800000, 0))
            with self.assertRaisesRegex(ValueError, 'base'):
                red_zone.patch_ranges(path)

    def test_opt_out_is_explicit_and_strict(self):
        self.assertTrue(red_zone.enabled({}))
        self.assertFalse(red_zone.enabled({'BB_RED_ZONE': '0'}))
        with self.assertRaises(ValueError):
            red_zone.enabled({'BB_RED_ZONE': 'false'})

    def test_session_records_current_preparation_coverage(self):
        from session_diagnostics import Session, SAFE_ENV
        self.assertIn('BB_RED_ZONE', SAFE_ENV)
        session = Session.__new__(Session)
        session.report = {}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'link.json'
            path.write_text(json.dumps({'red_zone': dict(enabled=True, applied_sites=607,
                            protected_stores=1854, changed_sites=['0x10'], conflicting_sites=[],
                            patched_ranges=[[16, 7]], private_field='excluded')}))
            session.record_red_zone(path)
        self.assertEqual(session.report['red_zone'], dict(enabled=True, applied_sites=607,
                         protected_stores=1854, changed_sites=1, conflicting_sites=0, outside_code_sites=0))

    def test_linker_adds_rx_segment_and_preserves_metadata(self):
        code = bytes(16) + sites()[0][1]
        data, report = linked_fixture(code, sites())
        self.assertEqual(struct.unpack_from('<8s6Q', data), (b'BBPROBE5', 65568, 0, 2, 0, 0, 1))
        self.assertEqual(struct.unpack_from('<Q', data, 56)[0], 256)
        self.assertEqual(struct.unpack_from('<6Q', data, 112), (0, 4096, 5, 65536, 32, 5))
        self.assertEqual(report['red_zone']['applied_sites'], 1)
        disabled, report = linked_fixture(code, sites(), enabled=False)
        self.assertEqual(disabled[-4096:], code.ljust(4096, b'\0'))
        self.assertFalse(report['red_zone']['enabled'])

    def test_linker_rejects_total_patch_or_relocation_conflicts(self):
        code = bytes(16) + sites()[0][1]
        with self.assertRaisesRegex(ValueError, 'no compatible'):
            linked_fixture(code, sites(), patches=patch_file(22, b'\x90'))
        with self.assertRaisesRegex(ValueError, 'no compatible'):
            linked_fixture(code, sites(), relocs=[(9, 0, 0, 0)])

    @unittest.skipUnless((ROOT / 'out/bb-probe.exe').exists(), 'build native probe first')
    def test_real_loader_executes_extra_rx_segment(self):
        # SysV entry keeps RSI (argv) intact. Store into a red-zone scratch slot
        # via RDX so it is classified like a non-stack guest output pointer.
        prefix = bytes.fromhex('488d5424a0b878563412')
        body = bytes.fromhex('8902909090')
        # A mismatch traps; a preserved scratch value reaches a named import.
        tail = bytes.fromhex('8b4424a03d785634127506ff25040000000f0b9090') + bytes(8)
        plan = sites(len(prefix), body.hex(), (-2, 1, 1, 1))
        code = prefix + body + tail
        data, _ = linked_fixture(code, plan, native_module=True,
                                 relocs=[(len(code) - 8, 1, 0, 0)], names=['red-zone-ok'])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            result = subprocess.run([str(ROOT / 'out/bb-probe.exe'), str(path), '--cpu-only'],
                                    capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('first unsupported PS4 import: red-zone-ok', result.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows VEH write fault')
    def test_windows_write_fault_preserves_red_zone_stack_and_flags(self):
        # The child isolates machine code/VEH execution from the test runner.
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--fault-fixture'],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['protected_trials'], 100)
        self.assertEqual(report['protected_failures'], 0)
        self.assertEqual(report['faults'], 200)


def fault_fixture():
    import ctypes as c
    from ctypes import wintypes as w
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.SetErrorMode(2)  # No interactive crash dialog in a failing child.
    kernel.VirtualAlloc.argtypes = [c.c_void_p, c.c_size_t, w.DWORD, w.DWORD]
    kernel.VirtualAlloc.restype = c.c_void_p
    kernel.VirtualProtect.argtypes = [c.c_void_p, c.c_size_t, w.DWORD, c.POINTER(w.DWORD)]
    kernel.VirtualProtect.restype = w.BOOL
    kernel.VirtualFree.argtypes = [c.c_void_p, c.c_size_t, w.DWORD]
    kernel.AddVectoredExceptionHandler.argtypes = [w.ULONG, c.c_void_p]
    kernel.AddVectoredExceptionHandler.restype = c.c_void_p
    kernel.RemoveVectoredExceptionHandler.argtypes = [c.c_void_p]

    class Record(c.Structure):
        _fields_ = [('code', w.DWORD), ('flags', w.DWORD), ('record', c.c_void_p),
                    ('address', c.c_void_p), ('count', w.DWORD), ('info', c.c_size_t * 15)]

    class Pointers(c.Structure):
        _fields_ = [('record', c.POINTER(Record)), ('context', c.c_void_p)]

    page = kernel.VirtualAlloc(None, 4096, 0x3000, 4)
    if not page:
        raise c.WinError(c.get_last_error())
    old = w.DWORD()
    faults = 0

    @c.WINFUNCTYPE(c.c_long, c.POINTER(Pointers))
    def handler(pointers):
        nonlocal faults
        record = pointers.contents.record.contents
        if record.code == 0xc0000005 and record.count >= 2 and record.info[0] == 1 and record.info[1] == page:
            if not kernel.VirtualProtect(page, 4096, 4, c.byref(old)):
                return 0
            faults += 1
            return -1
        return 0

    token = kernel.AddVectoredExceptionHandler(1, handler)
    if not token:
        raise c.WinError(c.get_last_error())
    sentinel = 0x1122334455667788
    # Windows ABI: RCX -> result, RDX -> protected page. Only volatile registers
    # are used. R8 saves entry RSP. Store-fault resumes at the same instruction.
    prefix = bytes.fromhex('4989e048b8') + struct.pack('<Q', sentinel) + bytes.fromhex('48894424a04839c9')
    if kernel.IsProcessorFeaturePresent(39):  # PF_AVX_INSTRUCTIONS_AVAILABLE
        prefix += bytes.fromhex('c5fc57c0c5fcc2c000')  # YMM0 all ones: live AVX state
    body = bytes.fromhex('89024c8b5424a0')
    tail = bytes.fromhex('4c89119c58488941084889e04c29c048894110488b0248894118')
    if kernel.IsProcessorFeaturePresent(39):
        tail += bytes.fromhex('c5f877')
    tail += b'\xc3'
    original = prefix + body + tail
    result = dict(protected_trials=100, protected_failures=0, control_corruption=0)
    try:
        for protected in (False, True):
            image = bytearray(original.ljust(4096, b'\xcc'))
            if protected:
                tramp, _ = red_zone.protect(image, sites(len(prefix), body.hex(), (-2, 5)), 4096, [(0, len(original))])
                image.extend(tramp)
            allocation = kernel.VirtualAlloc(None, len(image), 0x3000, 4)
            if not allocation:
                raise c.WinError(c.get_last_error())
            try:
                c.memmove(allocation, bytes(image), len(image))
                if not kernel.VirtualProtect(allocation, len(image), 0x20, c.byref(old)):
                    raise c.WinError(c.get_last_error())
                callback = c.WINFUNCTYPE(None, c.c_void_p, c.c_void_p)(allocation)
                for _ in range(100):
                    output = (c.c_uint64 * 4)()
                    c.memset(page, 0, 4096)
                    if not kernel.VirtualProtect(page, 4096, 2, c.byref(old)):
                        raise c.WinError(c.get_last_error())
                    callback(c.addressof(output), page)
                    valid = output[0] == sentinel and output[1] & 0x41 == 0x40 and output[2] == 0 and output[3] == sentinel & 0xffffffff
                    result['protected_failures' if protected else 'control_corruption'] += int(not valid)
            finally:
                kernel.VirtualFree(allocation, 0, 0x8000)
    finally:
        kernel.RemoveVectoredExceptionHandler(token)
        kernel.VirtualFree(page, 0, 0x8000)
    result['faults'] = faults
    print(json.dumps(result))


if __name__ == '__main__':
    if '--fault-fixture' in sys.argv:
        fault_fixture()
    else:
        unittest.main()
