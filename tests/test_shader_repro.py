"""Identity, instruction and comparison guards for the isolated shader reproducer."""
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('shader_repro', Path(__file__).resolve().parents[1] / 'tools/shader_repro.py')
repro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repro)


class ShaderReproTests(unittest.TestCase):
    def capture(self, program=123, permutation=4):
        return (struct.pack('<IIQ', 0x49435354, 3, 3) + b'abi' +
                struct.pack('<Q', 3) + b'key' + struct.pack('<IIQQ', 6, 5, program, permutation))

    def test_identity_matches_native_hash_combine(self):
        self.assertEqual(repro.binary_name(self.capture(), '0x000000009e3f29bf'), '0x000000000000007b_4.spv')

    def test_capture_identity_is_bounded(self):
        data = self.capture()
        for bad in (b'', data[:30], data[:8] + struct.pack('<Q', 2**64 - 1),
                    self.capture(permutation=4096), b'BAD!' + data[4:]):
            with self.subTest(data=bad), self.assertRaises(ValueError):
                repro.binary_name(bad, '0x000000009e3f29bf')
        with self.assertRaises(ValueError):
            repro.binary_name(data, '0x0000000000000001')

    def test_instruction_histogram_separates_instruction_changes_from_id_changes(self):
        def spirv(bound, operand, opcode=17):
            return struct.pack('<7I', 0x07230203, 0x10600, 0, bound, 0, (2 << 16) | opcode, operand)
        a, ids, ops = (repro.describe_spirv(x) for x in (spirv(2, 1), spirv(9, 8), spirv(2, 1, 19)))
        self.assertNotEqual(a['sha256'], ids['sha256'])
        self.assertEqual(a['opcodes'], ids['opcodes'])
        self.assertNotEqual(a['opcodes'], ops['opcodes'])

    def test_malformed_instructions_rejected(self):
        for words in ((0, 1, 2, 3, 4), (0x07230203, 0, 0, 0, 0, 17),
                      (0x07230203, 0, 0, 0, 0, (3 << 16) | 17, 1)):
            with self.subTest(words=words), self.assertRaises(ValueError):
                repro.describe_spirv(struct.pack('<' + 'I' * len(words), *words))

    def test_binary_stability_is_separate_from_raw_metadata_storage(self):
        def item(compiler='key', metadata='meta', capture='input', binary='spv'):
            return dict(compiler=compiler, stages={'stage': dict(binary='test.spv',
                spirv=dict(sha256=binary, opcodes={17: 1}), metadata_sha256=metadata, capture_sha256=capture)})
        self.assertTrue(repro.summarize([item(), item()], ['stage'])['binary_stable'])
        for changed in (item(compiler='other'), item(binary='other')):
            self.assertFalse(repro.summarize([item(), changed], ['stage'])['binary_stable'])
        report = repro.summarize([item(), item(metadata='other', capture='other')], ['stage'])
        self.assertTrue(report['binary_stable'])
        self.assertEqual(report['stages']['stage']['unique_metadata'], 2)
        self.assertEqual(report['stages']['stage']['unique_captures'], 2)


if __name__ == '__main__':
    unittest.main()
