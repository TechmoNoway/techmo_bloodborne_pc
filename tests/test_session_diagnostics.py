from paths import ROOT
import csv
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

from session_diagnostics import (Session, classify_exit, frame_summary, gpu_summary, memory_summary,
                                 compilation_summary, upload_summary)
from session_diagnostics import resource_work_summary, texture_activity_summary
import run_windows


class SessionDiagnosticsTests(unittest.TestCase):
    def test_startup_repair_and_rejection_evidence_are_separate(self):
        path = self.root / 'repair.log'
        path.write_text('Shader repair scan: 2 candidates, 3 previous failures unchanged\n'
            'Shader rebuild summary: 2 rebuilt, 10 current, 3 skipped, 4 uncaptured, 15.25 ms\n'
            'Pipeline warmup rejected: compiler_mismatch 7\n'
            'Pipeline warmup rejected: missing_compiler_stamp 9\n'
            'Pipeline warmup rejected: arbitrary_text 123\n')
        result = compilation_summary(path)
        self.assertEqual(result['shader_repair_scan'], dict(candidates=2, suppressed_unchanged=3))
        self.assertEqual(result['shader_rebuild'], dict(rebuilt=2,current=10,skipped=3,uncaptured=4,wall_ms=15.25))
        self.assertEqual(result['startup_rejection_counts'], dict(compiler_mismatch=7,missing_compiler_stamp=9))
        self.assertEqual(result['foreground_slow_ms'], 0)
        self.assertEqual(result['logged_wait_ms'], 0)

    def test_old_or_malformed_shader_rebuild_summary(self):
        path = self.root / 'old-repair.log'
        path.write_text('Shader rebuild summary: 1 rebuilt, 2 current, 3 skipped, 4.50 ms\n'
                        'Shader rebuild summary: 9 rebuilt, 9 current, 9 skipped, .. ms\n')
        result = compilation_summary(path)
        self.assertEqual(result['shader_rebuild'], dict(rebuilt=1,current=2,skipped=3,uncaptured=0,wall_ms=4.5))

    def test_pipeline_timeline_keeps_queue_compile_and_overlap_separate(self):
        path = self.root / 'timeline.log'
        lines = ['Pipeline native cache: 123 compatible seed bytes',
                 'Pipeline native cache: saved 456 payload bytes',
                 'Graphics lookahead: 40 scanner attempts, 20 known selections']
        lines += [f'Pipeline timeline: gfx 0xabc origin=required queued=10 start=12 need=9 queue_ms=2 compile_ms={i} lead_ms=-1 remaining_ms={i+3}' for i in range(30)]
        lines += ['Pipeline timeline: gfx 0xabc origin=required queued=nan',
                  'Pipeline timeline: gfx 0xabc origin=required queued=10 start=12 need=9 queue_ms=2 compile_ms=.. lead_ms=-1 remaining_ms=2']
        path.write_text('\n'.join(lines))
        result=compilation_summary(path)
        self.assertEqual(result['timeline_events'],30)
        self.assertEqual(len(result['timeline_largest']),20)
        self.assertEqual(result['timeline_largest'][0]['remaining_ms'],32)
        self.assertEqual(result['timeline_largest'][0]['lead_ms'],-1)
        self.assertEqual(result['logged_wait_ms'],0)
        self.assertEqual(result['native_cache_seed_bytes'],123)
        self.assertEqual(result['native_cache_saved_bytes'],456)
        self.assertEqual(result['graphics_lookahead']['known_selections'],20)

    def test_texture_snapshots_keep_latest_object_and_bound_evidence(self):
        path=self.root/'textures.log'
        line='Texture refresh: uid=1 address=0x100000 size=4096 count={} flags=3 new_backing=0 source_total_mb={} uploaded_mb=0 unchanged_mb=1 outcome=mips_match'
        path.write_text(line.format(64,64)+'\n'+line.format(128,128)+'\n'+
            'Texture create: uid=2 address=0x200000 size=4096 width=32 height=32 format=37 ms=174.2\n'+
            'Texture refresh: uid=3 address=0x100000 size=4096 count=64 flags=3')
        result=texture_activity_summary(path)
        self.assertEqual(result['sampled_refresh_events'],2)
        self.assertEqual(len(result['repeated_images']),1)
        self.assertEqual(result['repeated_images'][0]['source_total_mb'],128)
        self.assertEqual(result['slow_creates'][0]['wall_ms'],174.2)
        self.assertFalse(texture_activity_summary(self.root/'missing')['available'])

    def test_resource_work_columns_keep_cpu_timings_separate_and_accept_old_captures(self):
        path=self.root/'work.csv'
        path.write_text('t_s,frame_ms\n0,16\n')
        self.assertFalse(resource_work_summary(path)['available'])
        fields='hash_ms hash_snapshot_ms hash_mb hash_reused_mb unchanged_mip_mb hash_parallel_jobs detile_ms upload_record_ms host_copy_ms detile_scratch_reuses'.split()
        path.write_text('frame_ms,'+','.join(fields)+'\n16,'+','.join(['2']*len(fields))+'\n17,'+','.join(['3']*len(fields))+'\n18,nan,'+','.join(['1']*(len(fields)-1))+'\n')
        result=resource_work_summary(path)
        self.assertEqual((result['samples'],result['skipped_rows']),(2,1))
        self.assertEqual(result['totals']['hash_ms'],5)
        self.assertEqual(result['peaks']['detile_ms'],3)
        self.assertIn('not GPU transfer',result['note'])

    def test_optional_image_setup_columns_keep_old_samples_readable(self):
        path=self.root/'setup.csv'
        fields=('hash_ms hash_snapshot_ms hash_mb hash_reused_mb unchanged_mip_mb '
                'hash_parallel_jobs detile_ms upload_record_ms host_copy_ms detile_scratch_reuses '
                'image_alloc_ms image_view_ms image_format_ms image_recycle_hits').split()
        path.write_text('frame_ms,'+','.join(fields)+'\n16,'+','.join(['2']*len(fields))+'\n')
        report=resource_work_summary(path)
        self.assertEqual(report['totals']['image_alloc_ms'],2)
        self.assertEqual(report['totals']['image_recycle_hits'],2)
        path.write_text('frame_ms,'+','.join(fields)+'\n16,'+','.join(['2']*(len(fields)-1))+'\n')
        self.assertFalse(resource_work_summary(path)['available'])

    def test_compute_scanner_report_is_distinct_from_required_work(self):
        path=self.root/'scanner.log'
        path.write_text('Compute lookahead: 15 scanner attempts, 7 known selections\nPipeline required submissions: compute 2\n')
        result=compilation_summary(path)
        self.assertEqual(result['compute_lookahead'],{'attempts':15,'known_selections':7})
        self.assertEqual(result['required_compute_submissions'],2)
        path.write_text('Compute lookahead: 1 scanner attempts, 2 known selections\n')
        self.assertNotIn('compute_lookahead',compilation_summary(path))

    def test_image_query_columns_and_cache_evidence(self):
        path = self.root / 'query.csv'
        fields = ('hash_ms hash_snapshot_ms hash_mb hash_reused_mb unchanged_mip_mb '
                  'hash_parallel_jobs detile_ms upload_record_ms host_copy_ms detile_scratch_reuses '
                  'image_query_ms image_vma_ms image_query_hits image_query_misses image_query_bypasses').split()
        path.write_text('frame_ms,' + ','.join(fields) + '\n16,' + ','.join(['2'] * len(fields)) + '\n')
        result = resource_work_summary(path)
        self.assertEqual(result['totals']['image_query_ms'], 2)
        self.assertEqual(result['totals']['image_vma_ms'], 2)
        self.assertEqual(result['totals']['image_query_hits'], 2)
        log = self.root / 'query.log'
        log.write_text('Image allocation: width=128 height=128 depth=1 format=37 mips=1 '
                       'layers=1 bytes=65536 dedicated=0 query_ms=0.005 vma_ms=20 query_cache=hit\n')
        event = texture_activity_summary(log)['slow_allocations'][0]
        self.assertEqual(event['query_cache'], 'hit')
        self.assertEqual(event['vma_ms'], 20)

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

    def test_pipeline_handoff_waits_are_distinct_from_compile_counts(self):
        path = self.root / 'frames.csv'
        path.write_text('t_s,frame_ms,compiles,pipeline_wait_ms\n0,10,0,0.001\n1,50,0,35.000\n')
        report = frame_summary(path)
        self.assertEqual(report['mean_compiles'], 0)
        self.assertAlmostEqual(report['mean_pipeline_wait_ms'], 17.5005)
        self.assertAlmostEqual(report['pipeline_blocking']['total_ms'], 35.001)
        self.assertEqual(report['pipeline_blocking']['peak_ms'], 35)
        self.assertEqual(report['pipeline_blocking']['frames_over_2_ms'], 1)

    def test_pipeline_blocking_keeps_missing_and_invalid_counters_unknown(self):
        path = self.root / 'frames.csv'
        path.write_text('t_s,frame_ms,pipeline_wait_ms\n0,10,NaN\n1,20,-1\n2,30,\n')
        self.assertNotIn('pipeline_blocking', frame_summary(path))
        path.write_text('t_s,frame_ms,pipeline_wait_ms\n0,10,1\n1,120,114.008\n2,20,inf\n')
        report = frame_summary(path, 1, 2)
        self.assertAlmostEqual(report['pipeline_blocking']['total_ms'], 114.008)
        self.assertEqual(report['frames_over_100_ms'], 1)

    def test_tiled_gc_counters_are_cumulative_and_old_headers_remain_compatible(self):
        import csv
        fields = ('t_s driver_available device_usage_bytes device_budget_bytes gc_budget_bytes '
                  'tracked_image_bytes guest_buffer_allocated_bytes guest_buffer_unused_bytes '
                  'pending_image_bytes gc_evicted_images gc_written_back_images gc_pressure gc_critical '
                  'gc_tiled_attempts gc_tiled_writebacks gc_tiled_bytes gc_tiled_kept gc_tiled_ms gc_tiled_max_ms').split()
        path = self.root / 'tiled.csv'
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for t in (1, 2):
                row = dict.fromkeys(fields, 0)
                row.update(t_s=t, driver_available=1, device_usage_bytes=512, device_budget_bytes=1024,
                           gc_budget_bytes=1024, gc_tiled_attempts=3, gc_tiled_writebacks=2,
                           gc_tiled_bytes=8192, gc_tiled_ms=4.5, gc_tiled_max_ms=3.0)
                writer.writerow(row)
        report = memory_summary(path)
        self.assertEqual(report['gc_tiled_writebacks'], 2)
        self.assertEqual(report['gc_tiled_bytes'], 8192)
        self.assertEqual(report['gc_tiled_ms'], 4.5)
        self.assertEqual(report['gc_tiled_max_ms'], 3.0)
        # Corrupt timings must not turn into valid performance measurements.
        path.write_text(path.read_text().replace('4.5', 'nan'))
        self.assertFalse(memory_summary(path)['available'])

    def test_crash_evidence_has_priority_over_shell_exit_code(self):
        result = classify_exit(139, 'runtime', {'exception_code': '0xc0000005'})
        self.assertEqual(result['reason'], 'access_violation')
        self.assertEqual(classify_exit(23, 'runtime', {'exception_code': '0xe0424223'})['reason'], 'renderer_assertion')
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

    def test_compile_report_separates_foreground_work_background_work_and_waits(self):
        path = self.root / 'session.log'
        path.write_text('Pipeline preparation: on, 1 worker, 32 outstanding results maximum\n'
                        'Compile stall: shader module 0x12 120.50 ms (foreground)\n'
                        'Compile stall: graphics pipeline 0x34 30.00 ms (foreground)\n'
                        'Pipeline prepared: gfx 0x56 200.00 ms (background)\n'
                        'Pipeline preparation wait: gfx 0x56 2.75 ms\n'
                        'Pipeline preparation failed: gfx 0x78 synthetic; compiling required draw\n'
                        'Pipeline preparation: 3 queued, 2 adopted, 3.12 ms foreground waits, 1 retained\n'
                        'Pipeline preparation: 3 queued, 2 adopted, 3.12 ms foreground waits, 1 retained\n')
        report = compilation_summary(path)
        self.assertEqual(report['foreground_slow_ms'], 150.5)
        self.assertEqual(report['background_slow_ms'], 200)
        self.assertEqual(report['logged_wait_ms'], 2.75)
        self.assertEqual(report['preparation']['adopted'], 2)  # Repeated shutdown snapshots are not added.
        self.assertEqual(report['preparation']['foreground_wait_ms'], 3.12)
        self.assertEqual(report['preparation_failures'], 1)
        self.assertEqual(report['largest_events'][0]['kind'], 'background')

    def test_early_frontend_timing_and_native_capacity_are_separate(self):
        path = self.root / 'early.log'
        path.write_text('Pipeline early frontend: on, native continuation on same worker\n'
                        'Pipeline early ready: gfx 0x123 metadata_ms=1.000\n'
                        'Pipeline frontend: gfx 0x123 total_ms=46.000 descriptor_ms=1.000 layout_ms=2.000 state_ms=3.000 snapshot_ms=1.000 enqueue_ms=39.000\n'
                        'Pipeline timeline: gfx_frontend 0x123 origin=lookahead queued=1 start=2 need=3 queue_ms=1 compile_ms=1 lead_ms=2 remaining_ms=0 capacity_ms=0\n'
                        'Pipeline timeline: gfx_bind 0x123 origin=lookahead queued=1 start=3 need=4 queue_ms=2 compile_ms=200 lead_ms=3 remaining_ms=199 capacity_ms=39 admission_ms=2\n')
        report = compilation_summary(path)
        self.assertTrue(report['early_frontend_enabled'])
        self.assertEqual(report['early_frontends_published'], 1)
        self.assertEqual(report['frontend_largest'][0]['enqueue_ms'], 39)
        self.assertEqual(report['timeline_largest'][0]['capacity_wait_ms'], 39)
        self.assertEqual(report['timeline_largest'][0]['admission_wait_ms'], 2)
        self.assertEqual(report['timeline_events'], 2)
        self.assertEqual(report['timeline_stages']['gfx_bind']['peak_compile_ms'], 200)
        self.assertEqual(report['timeline_stages']['gfx_bind']['remaining_ms'], 199)
        self.assertEqual(report['timeline_stages']['gfx_frontend']['remaining_ms'], 0)

    def test_hash_block_reasons_keep_cumulative_multicauses_and_ignore_truncation(self):
        path = self.root / 'hash-reasons.log'
        names = ('disabled tracking_off not_gpu_dirty not_gpu_source backing_changed mips_missing cpu_dirty '
                 'not_fully_tracked tracking_gap mapping_busy mapping_changed host_write_busy host_reference_invalid host_write_changed').split()
        counts = ' '.join(name + '=7' for name in names)
        path.write_text('GPU hash reuse: candidates=10 reused=2 ' + counts + '\n' +
                        'GPU hash reuse: candidates=10 reused=2 ' + counts + '\n' +
                        'GPU hash reuse: candidates=11 reused=2 disabled=7\n' +
                        'Texture refresh: uid=79 address=0x106a4d0000 size=9437184 count=64 flags=68 new_backing=0 source_total_mb=600.000 uploaded_mb=9.437 unchanged_mb=0.000 outcome=uploaded gpu_source=1 gpu_writes=80 write_address=0x106a4d1000 write_size=4096 hash_reused_mb=0.000 hash_check=1 hash_blockers=0x2400 hash_candidates=64 hash_hits=0\n')
        report = texture_activity_summary(path)
        self.assertEqual(report['hash_reuse']['candidates'], 10)
        self.assertEqual(report['hash_reuse']['blocked']['mapping_changed'], 7)
        image = report['repeated_images'][0]
        self.assertEqual(image['hash_block_reasons'], ['mapping_changed', 'host_write_changed'])
        self.assertEqual(image['hash_candidates'], 64)

    def test_deferred_bind_and_gpu_source_evidence(self):
        path = self.root / 'session.log'
        path.write_text('Pipeline first use: gfx 0x12 native queued; wait at bind\n'
                        'Pipeline timeline: gfx_bind 0x12 origin=required queued=10.000 start=11.000 need=30.000 queue_ms=1.000 compile_ms=80.000 lead_ms=20.000 remaining_ms=61.000\n'
                        'Texture refresh: uid=79 address=0x106a4d0000 size=9437184 count=64 flags=68 new_backing=0 source_total_mb=600.000 uploaded_mb=9.437 unchanged_mb=0.000 outcome=uploaded gpu_source=1 gpu_writes=80 write_address=0x106a4d1000 write_size=4096 hash_reused_mb=9.437\n')
        report = compilation_summary(path)
        self.assertEqual(report['first_use_deferred'], 1)
        self.assertEqual(report['timeline_largest'][0]['stage'], 'gfx_bind')
        image = texture_activity_summary(path)['repeated_images'][0]
        self.assertTrue(image['gpu_source'])
        self.assertEqual(image['gpu_write_size'], 4096)
        self.assertEqual(image['hash_reused_mb'], 9.437)

    def test_compile_report_bounds_evidence_and_ignores_invalid_or_truncated_events(self):
        path = self.root / 'session.log'
        lines = [f'Compile stall: compute pipeline 0x12 {i}.00 ms (foreground)\n' for i in range(8, 80)]
        lines += ['Compile stall: shader module 0x12 NaN ms (foreground)\n',
                  'Pipeline preparation wait: gfx 0x34 -12 ms\n',
                  'Pipeline prepared: gfx 0x56 ... ms (background)\n',
                  'Compile stall: graphics pipeline secret 40 ms (foreground)\n',
                  'Pipeline prepared: gfx 0x12']
        path.write_text(''.join(lines))
        report = compilation_summary(path)
        self.assertEqual(report['foreground_slow_events']['compute pipeline'], 72)
        self.assertEqual(len(report['largest_events']), 20)
        self.assertEqual(report['largest_events'][0]['duration_ms'], 79)
        self.assertEqual(report['skipped_events'], 5)
        self.assertNotIn('secret', json.dumps(report))
        self.assertFalse(compilation_summary(self.root / 'missing.log')['available'])

    def test_compute_preparation_required_misses_and_retries_are_recorded(self):
        path = self.root / 'session.log'
        path.write_text('Compute pipeline preparation: on, 1 worker, 8 outstanding results maximum\n'
                        'Pipeline prepared: compute 0x12 90.00 ms (background)\n'
                        'Pipeline preparation wait: compute 0x12 40.00 ms\n'
                        'Compute pipeline preparation: 3 queued, 2 adopted, 40.40 ms foreground waits, 1 retained\n'
                        'Pipeline required submissions: gfx 2\n'
                        'Pipeline required submissions: compute 1\n'
                        'Pipeline required: gfx 0x12 reason=cold_shader primitive=4\n'
                        'Pipeline required: compute 0x12 reason=new_permutation\n'
                        'Pipeline retries: 3 passes, 7 draws recovered\n')
        report = compilation_summary(path)
        self.assertTrue(report['compute_preparation_enabled'])
        self.assertEqual(report['background_slow_ms'], 90)
        self.assertEqual(report['logged_wait_ms'], 40)
        self.assertEqual(report['foreground_slow_ms'], 0)
        self.assertEqual(report['compute_preparation']['adopted'], 2)
        self.assertEqual(report['required_graphics_submissions'], 2)
        self.assertEqual(report['required_compute_submissions'], 1)
        self.assertEqual(report['required_miss_reasons']['graphics pipeline']['cold_shader'], 1)
        self.assertEqual(report['required_miss_reasons']['compute pipeline']['new_permutation'], 1)
        self.assertEqual(report['retries']['recovered_draws'], 7)
        self.assertEqual(report['largest_events'][0]['stage'], 'compute pipeline')
        self.assertEqual(report['skipped_events'], 0)

    def test_deferred_compute_and_warmup_provenance_keep_bind_wait_separate(self):
        path = self.root / 'compute.log'
        path.write_text('Pipeline first use: compute 0x12 native queued; wait at bind\n'
                        'Pipeline cache lookup: compute 0x12 warmup=rejected rejection=missing_compiler_stamp shader=cold_shader preparation=first_use_deferred\n'
                        'Pipeline cache lookup: gfx 0x34 warmup=absent rejection=none shader=known preparation=lookahead\n'
                        'Compile stall: compute frontend 0x12 9.00 ms (foreground)\n'
                        'Pipeline timeline: compute_bind 0x12 origin=required queued=1 start=2 need=30 queue_ms=1 compile_ms=139 lead_ms=29 remaining_ms=110 capacity_ms=0 admission_ms=0\n')
        report = compilation_summary(path)
        self.assertEqual(report['compute_first_use_deferred'], 1)
        self.assertEqual(report['foreground_slow_events']['compute frontend'], 1)
        self.assertEqual(report['cache_paths'][0]['warmup'], 'rejected')
        self.assertEqual(report['cache_paths'][0]['rejection'], 'missing_compiler_stamp')
        self.assertEqual(report['cache_path_counts']['compute:rejected:missing_compiler_stamp:first_use_deferred'], 1)
        self.assertEqual(report['timeline_stages']['compute_bind']['peak_remaining_ms'], 110)
        self.assertEqual(report['foreground_slow_ms'], 9)
        self.assertEqual(report['logged_wait_ms'], 0)
        path.write_text('Pipeline cache lookup: compute 0x12 warmup=absent rejection=none shader=known preparation=lookahead\n' * 100)
        report = compilation_summary(path)
        self.assertEqual(len(report['cache_paths']), 64)
        self.assertEqual(report['cache_path_counts']['compute:absent:none:lookahead'], 100)

    def test_upload_report_keeps_counters_separate_and_finds_large_slow_flips(self):
        path = self.root / 'frames.csv'
        path.write_text('t_s,frame_ms,image_mb,buffer_mb,alloc_mb\n'
                        '10,16,400,180,88\n20,138,402,187,0\n30,257,113,43,88.4\n'
                        '40,50,0,0,200\n50,16,NaN,1,1\n60,16,-1,2,1\n70,8\n')
        report = upload_summary(path)
        self.assertEqual(report['samples'], 4)
        self.assertEqual(report['skipped_rows'], 3)
        self.assertEqual(report['peak_image_mb_per_flip'], 402)
        self.assertEqual(report['peak_buffer_mb_per_flip'], 187)
        self.assertEqual(report['peak_allocation_mb_per_flip'], 200)
        self.assertEqual(report['flips_with_upload_counter_at_least_64_mb'], 3)
        self.assertEqual(report['large_upload_flips_over_33_33_ms'], 2)
        self.assertEqual(report['slowest_large_upload_flips'][0]['t_s'], 30)
        self.assertNotIn('total_upload_mb', report)
        self.assertFalse(upload_summary(self.root / 'missing.csv')['available'])

    def test_session_includes_compile_evidence_and_safe_comparison_overrides(self):
        env = {'BB_PIPELINE_PREP': '0', 'BB_STAGING_REUSE_SLACK': '0'}
        session = Session(self.root, env)
        Path(env['BB_LOG_FILE']).write_text('Compile stall: graphics pipeline 0x123 20.00 ms (foreground)\n')
        session.finish(0)
        self.assertEqual(session.report['environment']['BB_PIPELINE_PREP'], '0')
        self.assertEqual(session.report['environment']['BB_STAGING_REUSE_SLACK'], '0')
        self.assertEqual(session.report['compilation']['foreground_slow_ms'], 20)
        self.assertIn('Slow foreground compile events: 1', (session.directory / 'summary.txt').read_text())

    def test_summary_includes_native_bind_blocking_without_adding_overlapping_counters(self):
        session = Session(self.root, {})
        (session.directory / 'frames.csv').write_text(
            't_s,frame_ms,pipeline_wait_ms\n0,16,0.001\n1,187,114.008\n')
        Path(session.env['BB_LOG_FILE']).write_text(
            'Pipeline preparation wait: gfx 0x12 21.15 ms\n'
            'Pipeline timeline: gfx_bind 0x12 origin=required queued=1 start=2 need=30 queue_ms=1 compile_ms=139 lead_ms=29 remaining_ms=110\n')
        session.finish(0)
        text = (session.directory / 'summary.txt').read_text()
        self.assertIn('Pipeline blocking (frame CSV, includes native bind): 114.01 ms total; peak 114.01 ms', text)
        self.assertIn('peak remaining wait 110.00 ms; peak native compile 139.00 ms', text)
        self.assertIn('do not add them', text)

    def test_image_allocation_parts_keep_bounded_evidence_and_accept_old_logs(self):
        path = self.root / 'allocations.log'
        template = ('Image allocation: width=512 height=512 depth=1 format=37 mips=1 '
                    'layers=1 bytes=1048576 dedicated=0 query_ms=2 vma_ms={}\n')
        path.write_text(''.join(template.format(i) for i in range(30)) + template.format('..'))
        report = texture_activity_summary(path)
        self.assertTrue(report['available'])
        self.assertEqual(len(report['slow_allocations']), 20)
        self.assertEqual(report['slow_allocations'][0]['wall_ms'], 31)
        self.assertEqual(report['slow_allocations'][0]['query_ms'], 2)
        self.assertFalse(report['slow_allocations'][0]['dedicated'])
        path.write_text('older log\n')
        self.assertEqual(texture_activity_summary(path)['slow_allocations'], [])

    def test_csv_pipeline_blocking_is_reported_even_without_compile_log_events(self):
        session = Session(self.root, {})
        (session.directory / 'frames.csv').write_text('t_s,frame_ms,pipeline_wait_ms\n1,187,114.008\n')
        session.finish(0)
        self.assertFalse(session.report['compilation']['available'])
        self.assertIn('Pipeline blocking (frame CSV, includes native bind): 114.01 ms total',
                      (session.directory / 'summary.txt').read_text())

    def write_memory(self, rows, buffer_columns=False):
        path = self.root / 'vram.csv'
        with path.open('w', newline='') as stream:
            writer = csv.writer(stream)
            columns = ['t_s', 'driver_available', 'device_usage_bytes', 'device_budget_bytes',
                       'gc_budget_bytes', 'tracked_image_bytes', 'guest_buffer_allocated_bytes',
                       'guest_buffer_unused_bytes', 'pending_image_bytes', 'gc_evicted_images',
                       'gc_written_back_images', 'gc_pressure', 'gc_critical']
            if buffer_columns:
                columns += ['pending_buffer_bytes', 'buffer_evicted_bytes', 'buffer_freed_bytes']
            writer.writerow(columns)
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
        self.assertEqual(report['peak_pending_buffer_bytes'], 0)
        self.assertEqual(report['buffer_freed_bytes'], 0)

    def test_memory_report_distinguishes_retired_slots_from_freed_allocations(self):
        base = [0, 1, 3000, 4000, 4000, 1500, 1200, 300, 0, 0, 0, 1, 0]
        rows = [base + [128, 512, 0], base + [0, 1024, 256],
                base + [-1, 1024, 256], base + [128, 512]]
        report = memory_summary(self.write_memory(rows, buffer_columns=True))
        self.assertEqual(report['samples'], 2)
        self.assertEqual(report['skipped_rows'], 2)
        self.assertEqual(report['peak_pending_buffer_bytes'], 128)
        self.assertEqual(report['buffer_evicted_bytes'], 1024)
        self.assertEqual(report['buffer_freed_bytes'], 256)

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
