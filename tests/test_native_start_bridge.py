from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from run_techmo import native_arguments

class NativeStartBridgeTests(unittest.TestCase):
    def test_game_folder_is_not_forwarded_as_a_probe_argument(self):
        env={}
        game=str(Path('game with spaces').resolve())
        self.assertEqual(native_arguments([game,'--timeout','30'],env),['--timeout','30'])
        self.assertEqual(env['BB_GAME_DIR'],game)

    def test_configured_game_and_probe_options_are_preserved(self):
        env={'BB_GAME_DIR':'already configured'}
        self.assertEqual(native_arguments(['--timeout','30'],env),['--timeout','30'])
        self.assertEqual(env['BB_GAME_DIR'],'already configured')

    def test_launcher_named_game_folder_is_consumed(self):
        env={}
        game=str(Path('game with spaces').resolve())
        self.assertEqual(native_arguments(['--game-dir',game,'--timeout','30'],env),['--timeout','30'])
        self.assertEqual(env['BB_GAME_DIR'],game)

    def test_missing_named_game_folder_is_rejected(self):
        for args in (['--game-dir'], ['--game-dir','--timeout','30']):
            with self.subTest(args=args), self.assertRaises(ValueError):
                native_arguments(args,{})
