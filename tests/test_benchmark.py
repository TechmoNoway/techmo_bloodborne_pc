import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('benchmark', Path(__file__).parents[1] / 'tools/benchmark.py')
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class BenchmarkTests(unittest.TestCase):
    def test_memory_helper_does_not_count_as_duplicate_game(self):
        self.assertEqual(benchmark.select_game_pid([9904, 21752], {21752}), 21752)

    def test_wait_for_game_when_only_headless_helper_exists(self):
        self.assertIsNone(benchmark.select_game_pid([9904], {5555}))

    def test_real_duplicate_game_windows_are_not_arbitrarily_selected(self):
        with self.assertRaisesRegex(RuntimeError, 'PIDs: 12, 34'):
            benchmark.select_game_pid([12, 34], {12, 34})

    def test_other_launcher_window_cannot_select_wrong_runtime(self):
        self.assertIsNone(benchmark.select_game_pid([12], {34}))

    def test_one_game_works_without_memory_helper(self):
        self.assertEqual(benchmark.select_game_pid([34], {34, 99}), 34)

    def analyze(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frames.csv'
            with path.open('w', newline='', encoding='utf-8') as stream:
                writer = csv.writer(stream)
                writer.writerow(['ProcessID', 'SwapChainAddress', 'MsBetweenPresents', 'Dropped'])
                writer.writerows(rows)
            return benchmark.analyze(path)

    def test_fps_and_tail_do_not_average_instantaneous_fps(self):
        result = self.analyze([(1, 'a', 900, 0), (1, 'a', 10, 0), (1, 'a', 30, 1)])['dominant_swapchain']
        self.assertEqual(result['average_present_fps'], 50)
        self.assertAlmostEqual(result['one_percent_low_fps'], 1000 / 30)
        self.assertEqual(result['dropped_presents'], 1)

    def test_swapchains_and_processes_are_separate(self):
        report = self.analyze([(1, 'a', 999, 0), (1, 'a', 10, 0), (1, 'a', 10, 0),
                               (1, 'b', 999, 0), (1, 'b', 100, 0),
                               (2, 'a', 999, 0), (2, 'a', 200, 0)])
        self.assertEqual(len(report['swapchains']), 3)
        self.assertEqual(report['dominant_swapchain']['average_present_fps'], 100)

    def test_invalid_intervals_are_ignored_without_hiding_stalls(self):
        result = self.analyze([(1, 'a', 0, 0), (1, 'a', 'NA', 0), (1, 'a', 'nan', 0),
                               (1, 'a', 'inf', 0), (1, 'a', -1, 0), (1, 'a', 300, 0)])['dominant_swapchain']
        self.assertEqual(result['frames'], 1)
        self.assertEqual(result['over_100ms'], 1)
        self.assertEqual(result['worst_frame_ms'], 300)

    def test_empty_capture_is_not_success(self):
        self.assertIsNone(self.analyze([])['dominant_swapchain'])

    def test_actual_presentmon_260_header(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frames.csv'
            path.write_text('Application,ProcessID,SwapChainAddress,msBetweenPresents,Dropped\n'
                            'bb-probe.exe,5,0x123,0,0\n'
                            'bb-probe.exe,5,0x123,20,0\n', encoding='utf-8-sig')
            report = benchmark.analyze(path)
            self.assertEqual(report['dominant_swapchain']['average_present_fps'], 50)


if __name__ == '__main__':
    unittest.main()
