from paths import ROOT
import csv
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

from session_diagnostics import Session, classify_exit, frame_summary, gpu_summary, memory_summary
import run_windows


class SessionDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write_frames(self, rows):
        path = self.root / 'frames.csv'
        with path.open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(['t_s', 'frame_ms', 'stage_a_cpu_ms', 'compiles'])
            writer.writerows(rows)
        return path

    def test_percentiles_slowest_one_percent_and_filtered_route(self):
        path = self.write_frames([[i, 10 if i < 99 else 100, 2, 0] for i in range(100)])
        result = frame_summary(path)
        self.assertAlmostEqual(result['average_fps'], 100000 / 1090)
        self.assertEqual(result['one_percent_low_fps'], 10)
        self.assertEqual(result['p95_frame_ms'], 10)
        self.assertEqual(result['p99_frame_ms'], 10)
        self.assertEqual(result['worst_frame_ms'], 100)
        self.assertEqual(result['mean_stage_a_cpu_ms'], 2)
        self.assertEqual(frame_summary(path, 10, 20)['samples'], 11)
        self.assertEqual(frame_summary(path, 10, 20)['average_fps'], 100)

    def test_truncated_invalid_and_nonfinite_rows(self):
        path = self.write_frames([[0, 16, 1, 0], [1, 'NaN', 1, 0], [2, 0, 1, 0],
                                  ['inf', 16, 1, 0], [3, 'broken'], [4, 20, 2, 1]])
        result = frame_summary(path)
        self.assertEqual(result['samples'], 2)
        self.assertEqual(result['skipped_rows'], 4)
        self.assertFalse(frame_summary(self.root / 'missing.csv')['available'])

    def test_crash_evidence_has_priority_over_shell_exit_code(self):
        result = classify_exit(139, 'runtime', {'exception_code': '0xc0000005'})
        self.assertEqual(result['reason'], 'access_violation')
        self.assertEqual(classify_exit(139, 'runtime', log_tail='Host fault 0xc000001d at runtime')['reason'], 'illegal_instruction')
        self.assertEqual(classify_exit(-1073741819, 'runtime')['reason'], 'access_violation')
        self.assertEqual(classify_exit(1, 'runtime', log_tail='VK_ERROR_DEVICE_LOST')['reason'], 'device_lost')
        self.assertEqual(classify_exit(1, 'runtime', log_tail='VK_ERROR_OUT_OF_DEVICE_MEMORY')['reason'], 'device_memory_error')
        self.assertEqual(classify_exit(139, 'runtime')['reason'], 'unknown_exit')

    def test_preparation_errors_normal_interrupt_and_loader_failures(self):
        self.assertEqual(classify_exit(1, 'prepare')['category'], 'startup_error')
        self.assertEqual(classify_exit(0, 'runtime')['category'], 'normal')
        self.assertEqual(classify_exit(130, 'runtime')['category'], 'interrupted')
        self.assertEqual(classify_exit(0xc0000135, 'runtime')['reason'], 'missing_dependency')
        self.assertEqual(classify_exit(0xc000007b, 'runtime')['reason'], 'invalid_binary')

    def test_nonblocking_gpu_capture_records_partitions_and_skips(self):
        path = self.root / 'gpu.csv'
        path.write_text('sample,gpu_interval_ms,timestamp_count,skipped_frames,labelled_work_ms,between_submissions_ms\n'
                        '1,10,5,2,4,6\n2,12,7,5,5,7\n3,9,4,bad,NaN,9\n4,8,4,-1,20,1\n')
        report = gpu_summary(path)
        self.assertEqual(report['samples'], 4)
        self.assertEqual(report['mean_labelled_work_ms'], 4.5)
        self.assertEqual(report['mean_between_submissions_ms'], 6.5)
        self.assertEqual(report['partition_samples'], 2)
        self.assertEqual(report['skipped_frames'], 5)
        self.assertIn('bias', report['note'])

    def test_session_records_safe_settings_and_finishes_atomically(self):
        config = self.root / 'bbport.ini'
        config.write_text('upscaler=fsr3\noutput_res=1920x1080\nonline_password=secret\nonline_token=secret\n')
        env = {'BB_CONFIG': str(config), 'BB_FRAME_STATS': '1', 'BB_GPU_PROFILE': '1',
               'BB_ONLINE_PASSWORD': 'secret', 'BB_FPS': '60'}
        session = Session(self.root, env)
        self.assertEqual(Path(env['BB_FRAME_LOG']).name, 'frames.csv')
        self.assertEqual(Path(env['BB_VRAM_LOG']).name, 'vram.csv')
        self.assertEqual(Path(env['BB_GPU_LOG']).name, 'gpu.csv')
        session.set_phase('runtime')
        Path(env['BB_LOG_FILE']).write_text('Host fault 0xc0000005 at synthetic\n')
        session.finish(139)
        text = (session.directory / 'session.json').read_text()
        report = json.loads(text)
        self.assertNotIn('secret', text)
        self.assertEqual(report['status'], 'finished')
        self.assertEqual(report['exit_code_hex'], '0x0000008B')
        self.assertEqual(report['outcome']['reason'], 'access_violation')
        self.assertTrue((session.directory / 'summary.txt').is_file())
        self.assertFalse((session.directory / 'session.json.tmp').exists())

    def test_recording_off_does_not_enable_profilers(self):
        env = {}
        session = Session(self.root, env)
        self.assertNotIn('BB_FRAME_LOG', env)
        self.assertNotIn('BB_GPU_LOG', env)
        self.assertNotIn('BB_VRAM_LOG', env)
        session.finish(1)
        self.assertEqual(session.report['outcome']['stage'], 'startup')

    def write_memory(self, rows):
        path = self.root / 'vram.csv'
        with path.open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(['t_s', 'driver_available', 'device_usage_bytes', 'device_budget_bytes',
                             'gc_budget_bytes', 'tracked_image_bytes', 'guest_buffer_allocated_bytes',
                             'guest_buffer_unused_bytes', 'pending_image_bytes', 'gc_evicted_images',
                             'gc_written_back_images', 'gc_pressure', 'gc_critical'])
            writer.writerows(rows)
        return path

    def test_memory_report_tracks_shrinking_budget_and_overlapping_categories(self):
        path = self.write_memory([
            [0, 1, 3000, 4000, 4000, 1500, 1200, 300, 100, 0, 0, 0, 0],
            [.25, 1, 3100, 3200, 3200, 1400, 1200, 400, 200, 7, 2, 1, 1],
            [.5, 1, 2800, 4000, 4000, 1000, 1000, 300, 0, 9, 2, 0, 0]])
        report = memory_summary(path)
        self.assertEqual(report['samples'], 3)
        self.assertEqual(report['peak_device_usage_bytes'], 3100)
        self.assertEqual(report['minimum_device_budget_bytes'], 3200)
        self.assertAlmostEqual(report['peak_budget_fraction'], 3100 / 3200)
        self.assertEqual(report['gc_evicted_images'], 9)
        self.assertEqual(report['gc_written_back_images'], 2)
        self.assertEqual(report['peak_pending_image_bytes'], 200)
        self.assertEqual(report['pressure_samples'], 1)
        self.assertEqual(report['critical_samples'], 1)
        self.assertIn('overlap', report['note'])
        self.assertNotIn('total_category_bytes', report)

    def test_memory_report_ignores_unsupported_invalid_and_truncated_samples(self):
        good = [0, 1, 3000, 4000, 4000, 1500, 1200, 300, 100, 0, 0, 0, 0]
        rows = [good]
        for column, value in ((0, 'NaN'), (1, 0), (2, -1), (3, 0), (4, 'bad'), (11, 2)):
            bad = good.copy()
            bad[column] = value
            rows.append(bad)
        rows.append([.25, 1, 3000])
        report = memory_summary(self.write_memory(rows))
        self.assertEqual(report['samples'], 1)
        self.assertEqual(report['skipped_rows'], 7)
        self.assertFalse(memory_summary(self.root / 'missing.csv')['available'])
        self.assertFalse(memory_summary(self.write_memory([[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]]))['available'])

    def test_session_memory_capture_survives_restart_and_records_cache_overrides(self):
        env = {'BB_FRAME_STATS': '1', 'BB_GC_BUDGET_MB': '3072', 'BB_VRAM_IDLE_SECONDS': '60'}
        original = Session(self.root, env)
        capture = self.write_memory([[0, 1, 3000, 4000, 3072, 1500, 1200, 300, 0, 2, 0, 1, 0]])
        Path(env['BB_VRAM_LOG']).write_bytes(capture.read_bytes())
        original.finish(0)
        self.assertTrue(original.report['memory']['available'])
        self.assertEqual(original.report['environment']['BB_GC_BUDGET_MB'], '3072')
        self.assertIn('VRAM driver estimate peak', (original.directory / 'summary.txt').read_text())
        before = Path(env['BB_VRAM_LOG']).read_bytes()
        restarted = Session(self.root, env)
        self.assertEqual(Path(env['BB_VRAM_LOG']).parent, restarted.directory)
        self.assertEqual((original.directory / 'vram.csv').read_bytes(), before)

    def test_independent_gpu_samples_are_not_cpu_times(self):
        path = self.root / 'gpu.csv'
        path.write_text('sample,gpu_interval_ms,timestamp_count\n1,12.5,10\n2,13.5,12\n3,bad,')
        report = gpu_summary(path)
        self.assertEqual(report['samples'], 2)
        self.assertEqual(report['mean_gpu_interval_ms'], 13)
        self.assertIn('not aligned', report['note'])

    def test_restart_inherited_environment_keeps_previous_session(self):
        env = {}
        original = Session(self.root, env)
        original.set_phase('runtime')
        original.finish(0)
        before = (original.directory / 'session.json').read_bytes()
        restarted = Session(self.root, env)
        self.assertNotEqual(original.directory, restarted.directory)
        self.assertEqual((original.directory / 'session.json').read_bytes(), before)
        self.assertEqual(Path(env['BB_LOG_FILE']).parent, restarted.directory)

    def test_entry_reports_preparation_failure_and_preserves_process_exit(self):
        def failure():
            run_windows.phase('prepare')
            raise SystemExit('Synthetic preparation failure')
        original_stdout, original_stderr = sys.stdout, sys.stderr
        previous_log, previous_session = run_windows.LOG_STREAM, run_windows.SESSION
        try:
            with patch.dict(os.environ, {'BB_DATA_DIR': str(self.root)}, clear=True), \
                 patch.object(run_windows, 'main', side_effect=failure):
                self.assertEqual(run_windows.entry(), 1)
                directory = run_windows.SESSION.directory
                report = json.loads((directory / 'session.json').read_text())
                self.assertEqual(report['outcome']['category'], 'startup_error')
                self.assertEqual(report['stage'], 'prepare')
                self.assertIn('Synthetic preparation failure', (directory / 'session.log').read_text())
        finally:
            sys.stdout, sys.stderr = original_stdout, original_stderr
            if run_windows.LOG_STREAM:
                run_windows.LOG_STREAM.close()
            run_windows.LOG_STREAM, run_windows.SESSION = previous_log, previous_session


if __name__ == '__main__':
    unittest.main()
