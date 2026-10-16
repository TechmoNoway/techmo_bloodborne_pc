from paths import ROOT
import unittest
from run_windows import configure_frame_rate


class WindowsFrameRateTests(unittest.TestCase):
    def test_supported_presets_override_inherited_unbounded_values(self):
        for fps in ('30', '60', '90'):
            with self.subTest(fps=fps):
                env = {'BB_FPS': fps, 'BB_FPS_LIMIT': '0', 'BB_VBLANK_HZ': '480'}
                self.assertEqual(configure_frame_rate(env), fps)
                self.assertEqual(env['BB_FPS_LIMIT'], fps)
                self.assertEqual(env['BB_VBLANK_HZ'], '90' if fps == '90' else '60')

    def test_default_and_retired_presets_are_capped_at_60(self):
        for value in (None, '120', 'Unlimited', '', '0', '480', 'bad'):
            with self.subTest(value=value):
                env = {'BB_VBLANK_HZ': '0', 'BB_FPS_LIMIT': '0'}
                if value is not None:
                    env['BB_FPS'] = value
                self.assertEqual(configure_frame_rate(env), '60')
                self.assertEqual(env['BB_FPS'], '60')
                self.assertEqual(env['BB_FPS_LIMIT'], '60')
                self.assertEqual(env['BB_VBLANK_HZ'], '60')

    def test_unlimited_preserves_user_presentation_choice(self):
        for mode in ('Mailbox', 'Immediate', 'Fifo', 'FifoRelaxed'):
            with self.subTest(mode=mode):
                env = {'BB_FPS': 'uncap', 'BB_PRESENT_MODE': mode}
                self.assertEqual(configure_frame_rate(env), 'uncap')
                self.assertEqual(env['BB_FPS_LIMIT'], '0')
                self.assertEqual(env['BB_VBLANK_HZ'], '480')
                self.assertEqual(env['BB_PRESENT_MODE'], mode)

    def test_fixed_presets_preserve_user_presentation_choice(self):
        for fps in ('30', '60', '90'):
            for mode in ('Mailbox', 'Immediate', 'Fifo', 'FifoRelaxed'):
                env = {'BB_FPS': fps, 'BB_PRESENT_MODE': mode}
                configure_frame_rate(env)
                self.assertEqual(env['BB_PRESENT_MODE'], mode)


if __name__ == '__main__':
    unittest.main()
