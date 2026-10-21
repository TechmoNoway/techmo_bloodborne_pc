"""Local session reports. Collect only explicit graphics settings, never credentials."""
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import platform
import re
import statistics
import uuid
from native_identity import identify_runtime

SAFE_SETTINGS = {
    'output_res', 'upscaler', 'preset', 'live_resolution', 'fullscreen', 'sharpen',
    'model_lod', 'pc_controls', 'mouse_camera', 'effect_game_aa', 'effect_ssao',
    'effect_dof', 'effect_motion_blur', 'effect_dynamic_shadows', 'effect_ssr',
    'effect_chromatic_aberration', 'graphics_controls', 'camera_controls', 'widescreen',
    'graphics_vignette', 'graphics_ao_strength', 'graphics_shadow_scale', 'graphics_bloom',
    'graphics_saturation', 'camera_fov_scale', 'camera_distance_scale', 'camera_height_scale',
    'show_fps', 'show_cpu', 'show_gpu', 'mouse_sensitivity',
}
SAFE_ENV = {
    'BB_FPS', 'BB_FPS_LIMIT', 'BB_VBLANK_HZ', 'BB_RENDER_RES', 'BB_OUTPUT_RES',
    'BB_DMEM_MB', 'BB_FRAME_STATS', 'BB_GPU_PROFILE', 'BB_MODS_ENABLED',
    'BB_PRESENT_MODE', 'BB_LIVE_RES', 'BB_TIMEOUT',
    'BB_VK_RECORD_THREADS', 'BB_VK_RECORD_THREAD', 'BB_VK_SEGMENT_KB',
    'BB_ASYNC_SUBMIT', 'BB_DIRECT_SEGMENT', 'BB_FSR4_OPT',
    'BB_GC_BUDGET_MB', 'BB_GC_IDLE_SECONDS', 'BB_GC_PRESSURE_IDLE_SECONDS', 'BB_VRAM_IDLE_SECONDS',
    'BB_VMA_DEDICATED_MB', 'BB_GUEST_IN_PLACE', 'BB_GARLIC_VRAM', 'BB_GPU_DATA_CHECK',
    'BB_IMAGE_REQUIREMENTS_CACHE',
    'BB_PIPELINE_PREP', 'BB_STAGING_REUSE_SLACK',
    'BB_RANGE_BACKING_EPOCH',
    'BB_REQUIRED_PIPELINE_PRIORITY',
    'BB_COMPUTE_PREP', 'BB_FIRST_USE_COMPUTE', 'BB_PIPELINE_RETRY', 'BB_PREP_WORKERS',
    'BB_RED_ZONE',
    'BB_IMAGE_WRITEBACK_GC', 'BB_WRITE_TRACKING', 'BB_MEMORY_SAFE', 'BB_AUTO_DMEM', 'BB_RENDER_POOLS',
    'BB_KEEP_MIP_HASHES', 'BB_PARALLEL_IMAGE_HASH', 'BB_DETILE_SCRATCH_REUSE', 'BB_COMPUTE_SCAN', 'BB_GRAPHICS_SCAN', 'BB_GRAPHICS_SCAN_SPREAD', 'BB_FIRST_USE_PIPELINE', 'BB_PIPELINE_EARLY_FRONTEND', 'BB_REUSE_GPU_SOURCE_HASH', 'BB_IMAGE_RECYCLE_MB',
}


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return {}


def frame_summary(path, start=0, end=None):
    """Flip intervals, including loading/menus; they are not CPU execution timings."""
    samples, rows, skipped = [], [], 0
    try:
        with Path(path).open(encoding='utf-8') as stream:
            for row in csv.DictReader(stream):
                try:
                    t, ms = float(row['t_s']), float(row['frame_ms'])
                    if not math.isfinite(t) or not math.isfinite(ms) or ms <= 0:
                        raise ValueError('invalid sample')
                except (KeyError, TypeError, ValueError):
                    skipped += 1
                    continue
                if t < start or (end is not None and t > end):
                    continue
                samples.append(ms)
                rows.append(row)
    except OSError:
        return {'available': False}
    if not samples:
        return {'available': False, 'samples': 0, 'skipped_rows': skipped}
    ordered = sorted(samples)
    percentile = lambda p: ordered[max(0, math.ceil(len(ordered) * p) - 1)]
    slowest = ordered[-max(1, math.ceil(len(ordered) * .01)):]
    result = {
        'available': True, 'measurement': 'videoout_flip_interval',
        'scope': 'selected interval' if start or end is not None else 'whole session including loading and menus',
        'samples': len(samples), 'sample_duration_s': sum(samples) / 1000,
        'average_fps': 1000 / statistics.mean(samples),
        'one_percent_low_fps': 1000 / statistics.mean(slowest),
        'one_percent_low_definition': '1000 / mean of slowest ceil(sample_count * 0.01) frame intervals',
        'mean_frame_ms': statistics.mean(samples), 'p95_frame_ms': percentile(.95),
        'p99_frame_ms': percentile(.99), 'worst_frame_ms': max(samples),
        'frames_over_33_33_ms': sum(ms > 1000/30 for ms in samples),
        'frames_over_50_ms': sum(ms > 50 for ms in samples), 'skipped_rows': skipped,
        'frames_over_100_ms': sum(ms > 100 for ms in samples),
    }
    for name in ('stage_a_cpu_ms', 'present_wait_ms', 'tick_wait_ms', 'compiles', 'pipeline_wait_ms'):
        values = []
        for row in rows:
            try:
                value = float(row[name])
                if math.isfinite(value) and value >= 0:
                    values.append(value)
            except (KeyError, TypeError, ValueError):
                pass
        if values:
            result['mean_' + name] = statistics.mean(values)
            if name == 'pipeline_wait_ms':
                result['pipeline_blocking'] = {
                    'measurement': 'per-flip pipeline wait counter, including native bind waits',
                    'samples': len(values), 'total_ms': sum(values), 'peak_ms': max(values),
                    'frames_over_2_ms': sum(value > 2 for value in values),
                    'note': 'Overlaps preparation and timeline evidence; do not add those totals.',
                }
    return result


def gpu_summary(path):
    values = []
    labelled_work, between_submissions, skipped = [], [], []
    try:
        with Path(path).open(encoding='utf-8') as stream:
            for row in csv.DictReader(stream):
                try:
                    ms = float(row['gpu_interval_ms'])
                    if math.isfinite(ms) and ms >= 0:
                        values.append(ms)
                        # Optional columns: preserve compatibility with older captures.
                        try:
                            work, gap = float(row['labelled_work_ms']), float(row['between_submissions_ms'])
                            if all(math.isfinite(v) and v >= 0 for v in (work, gap)) and abs(work + gap - ms) <= .001:
                                labelled_work.append(work)
                                between_submissions.append(gap)
                        except (KeyError, ValueError, TypeError):
                            pass
                        try:
                            lost = int(row['skipped_frames'])
                            if lost >= 0: skipped.append(lost)
                        except (KeyError, ValueError, TypeError):
                            pass
                except (KeyError, ValueError, TypeError):
                    pass
    except OSError:
        pass
    report = {'available': bool(values), 'samples': len(values),
            'mean_gpu_interval_ms': statistics.mean(values) if values else None,
            'measurement': 'recorded GPU timestamp interval; includes intervening barriers, copies and waits',
            'note': 'Independent GPU samples; not aligned one-to-one with VideoOut flip rows. Profiling adds overhead.'}
    if labelled_work:
        report.update(mean_labelled_work_ms=statistics.mean(labelled_work),
                      mean_between_submissions_ms=statistics.mean(between_submissions),
                      partition_samples=len(labelled_work))
    if skipped:
        report['skipped_frames'] = max(skipped)
        report['note'] += ' Busy query slices skip sampling; skipped frames can bias GPU averages.'
    return report


def memory_summary(path):
    """Low-frequency driver estimates, not texture totals or OS-wide GPU usage."""
    rows, skipped = [], 0
    try:
        with Path(path).open(encoding='utf-8') as stream:
            for row in csv.DictReader(stream):
                try:
                    if int(row['driver_available']) != 1:
                        skipped += 1
                        continue
                    t = float(row['t_s'])
                    if not math.isfinite(t) or t < 0:
                        raise ValueError('invalid timestamp')
                    values = {key: int(row[key]) for key in (
                        'device_usage_bytes', 'device_budget_bytes', 'gc_budget_bytes',
                        'tracked_image_bytes', 'guest_buffer_allocated_bytes',
                        'guest_buffer_unused_bytes', 'pending_image_bytes',
                        'gc_evicted_images', 'gc_written_back_images', 'gc_pressure', 'gc_critical')}
                    for key in ('pending_buffer_bytes', 'buffer_evicted_bytes', 'buffer_freed_bytes',
                                'gc_tiled_attempts', 'gc_tiled_writebacks', 'gc_tiled_bytes', 'gc_tiled_kept', 'image_recycle_bytes'):
                        values[key] = int(row[key]) if key in row else 0  # Older headers remain readable.
                    for key in ('gc_tiled_ms', 'gc_tiled_max_ms'):
                        values[key] = float(row[key]) if key in row else 0.0
                        if not math.isfinite(values[key]):
                            raise ValueError('invalid writeback timing')
                    if any(value < 0 for value in values.values()) or values['device_budget_bytes'] == 0:
                        raise ValueError('invalid memory sample')
                    if values['gc_pressure'] not in (0, 1) or values['gc_critical'] not in (0, 1):
                        raise ValueError('invalid pressure flags')
                    rows.append(values)
                except (KeyError, ValueError, TypeError):
                    skipped += 1
    except OSError:
        return {'available': False}
    if not rows:
        return {'available': False, 'samples': 0, 'skipped_rows': skipped}
    result = {
        'available': True, 'samples': len(rows), 'skipped_rows': skipped,
        'measurement': 'VK_EXT_memory_budget process heap estimate; sampled at most 4 Hz',
        'peak_device_usage_bytes': max(row['device_usage_bytes'] for row in rows),
        'mean_device_usage_bytes': statistics.mean(row['device_usage_bytes'] for row in rows),
        'minimum_device_budget_bytes': min(row['device_budget_bytes'] for row in rows),
        'peak_budget_fraction': max(row['device_usage_bytes'] / row['device_budget_bytes'] for row in rows),
        'pressure_samples': sum(row['gc_pressure'] for row in rows),
        'critical_samples': sum(row['gc_critical'] for row in rows),
        'gc_evicted_images': max(row['gc_evicted_images'] for row in rows),
        'gc_written_back_images': max(row['gc_written_back_images'] for row in rows),
        'note': 'Independent low-frequency samples can miss peaks. Tracked images, guest buffers and pending frees overlap; do not add them to driver usage. Shared VMA block slack, upscaler internals and driver allocations are not individually attributed.',
    }
    for name in ('tracked_image_bytes', 'guest_buffer_allocated_bytes', 'guest_buffer_unused_bytes',
                 'pending_image_bytes', 'pending_buffer_bytes', 'image_recycle_bytes'):
        result['peak_' + name] = max(row[name] for row in rows)
    for name in ('buffer_evicted_bytes', 'buffer_freed_bytes', 'gc_tiled_attempts',
                 'gc_tiled_writebacks', 'gc_tiled_bytes', 'gc_tiled_kept', 'gc_tiled_ms', 'gc_tiled_max_ms'):
        result[name] = max(row[name] for row in rows)
    return result


def compilation_summary(path):
    """Stream bounded compile evidence; slow events do not count all compilations."""
    patterns = (
        ('foreground', re.compile(r'Compile stall: (shader module|graphics pipeline|graphics frontend|compute pipeline|compute frontend) (\w+) (\d+(?:\.\d+)?) ms \(foreground\)')),
        ('background', re.compile(r'Pipeline prepared: (gfx|compute) (\w+) (\d+(?:\.\d+)?) ms \(background\)')),
        ('wait', re.compile(r'Pipeline preparation wait: (gfx|compute) (\w+) (\d+(?:\.\d+)?) ms')),
    )
    counts = {'shader module': 0, 'graphics pipeline': 0, 'graphics frontend': 0, 'compute pipeline': 0, 'compute frontend': 0}
    foreground_ms = background_ms = wait_ms = 0.0
    background_events = wait_events = failures = skipped = 0
    largest = []
    timelines = []
    timeline_count = 0
    timeline_stages = {}
    frontend_events = []
    cache_paths = []
    cache_path_counts = {}
    required_reasons = {stage: {reason: 0 for reason in ('cold_shader', 'new_permutation', 'not_prepared')}
                        for stage in ('graphics pipeline', 'compute pipeline')}
    result = {'available': False,
              'measurement': 'foreground operations >=8 ms; background operations >=8 ms; foreground waits >=2 ms',
              'note': 'Thresholded log events omit shorter operations. Background work can overlap required foreground waits; do not add those totals. Hashes identify pipeline state, not game assets.'}
    try:
        with Path(path).open(encoding='utf-8', errors='replace') as stream:
            for line_number, line in enumerate(stream, 1):
                if 'Pipeline preparation: on,' in line:
                    result.update(available=True, preparation_enabled=True)
                if 'Compute pipeline preparation: on,' in line:
                    result.update(available=True, compute_preparation_enabled=True)
                if 'Pipeline preparation failed:' in line:
                    failures += 1
                    result['available'] = True
                summary = re.search(r'(Compute pipeline|Pipeline) preparation: (\d+) queued, (\d+) adopted, (\d+(?:\.\d+)?) ms foreground waits, (\d+) retained', line)
                if summary:
                    pipeline_kind, queued, adopted, elapsed, retained = summary.groups()
                    elapsed = float(elapsed)
                    if math.isfinite(elapsed) and 0 <= int(adopted) <= int(queued):
                        field = 'compute_preparation' if pipeline_kind == 'Compute pipeline' else 'preparation'
                        result['available'] = True
                        result[field] = {'queued': int(queued), 'adopted': int(adopted),
                                         'foreground_wait_ms': elapsed, 'retained': int(retained)}
                required = re.search(r'Pipeline required submissions: (gfx|compute) (\d+)', line)
                if required:
                    field = 'required_graphics_submissions' if required[1] == 'gfx' else 'required_compute_submissions'
                    result[field] = int(required[2])
                    result['available'] = True
                reason = re.search(r'Pipeline required: (gfx|compute) (?:0x)?[0-9a-fA-F]+ reason=(cold_shader|new_permutation|not_prepared)\b', line)
                if reason:
                    stage = 'graphics pipeline' if reason[1] == 'gfx' else 'compute pipeline'
                    required_reasons[stage][reason[2]] += 1
                    result['available'] = True
                enabled = re.search(r'Pipeline early frontend: (on|off),', line)
                if enabled: result.update(early_frontend_enabled=enabled[1] == 'on', available=True)
                ready = re.search(r'Pipeline early ready: gfx (0x[0-9a-fA-F]+) metadata_ms=([\d.]+)', line)
                if ready:
                    try:
                        if math.isfinite(float(ready[2])):
                            result['early_frontends_published'] = result.get('early_frontends_published', 0) + 1
                    except ValueError: skipped += 1
                front = re.search(r'Pipeline frontend: gfx (0x[0-9a-fA-F]+) total_ms=([\d.]+) descriptor_ms=([\d.]+) layout_ms=([\d.]+) state_ms=([\d.]+) snapshot_ms=([\d.]+) enqueue_ms=([\d.]+)', line)
                if front:
                    try:
                        durations = [float(v) for v in front.groups()[1:]]
                        if any(not math.isfinite(v) or v < 0 for v in durations): raise ValueError('invalid frontend')
                        event = dict(zip(('total_ms', 'descriptor_ms', 'layout_ms', 'state_ms', 'snapshot_ms', 'enqueue_ms'), durations))
                        event.update(hash=front[1], log_line=line_number)
                        frontend_events.append(event);frontend_events.sort(key=lambda v: v['total_ms'], reverse=True);del frontend_events[20:]
                        result['available'] = True
                    except ValueError: skipped += 1
                if re.search(r'Pipeline first use: gfx (?:0x)?[0-9a-fA-F]+ native queued; wait at bind', line):
                    result['first_use_deferred'] = result.get('first_use_deferred', 0) + 1
                    result['available'] = True
                if re.search(r'Pipeline first use: compute (?:0x)?[0-9a-fA-F]+ native queued; wait at bind', line):
                    result['compute_first_use_deferred'] = result.get('compute_first_use_deferred', 0) + 1
                    result['available'] = True
                cache_path = re.search(r'Pipeline cache lookup: (gfx|compute) (0x[0-9a-fA-F]+) warmup=(not_run|disabled|unavailable|profile_missing|profile_mismatch|absent|loaded|rejected) rejection=(none|invalid_record|missing_metadata|stage_mismatch|unsupported_motion_feature|legacy_auxiliary_missing|compiler_mismatch|missing_compiler_stamp|incompatible_metadata|invalid_spirv|specialization_conflict|record_version_or_type|record_exception) shader=(cold_shader|new_permutation|known) preparation=(foreground|lookahead|required_worker|first_use_deferred)', line)
                if cache_path:
                    stage, key, state, rejection, shader, preparation = cache_path.groups()
                    category = f'{stage}:{state}:{rejection}:{preparation}'
                    cache_path_counts[category] = cache_path_counts.get(category, 0) + 1
                    if len(cache_paths) < 64:
                        cache_paths.append(dict(stage=stage, hash=key, warmup=state, rejection=rejection,
                                                shader=shader, preparation=preparation, log_line=line_number))
                    result.update(available=True, cache_paths=cache_paths, cache_path_counts=cache_path_counts)
                seed = re.search(r'Pipeline native cache: (\d+) compatible seed bytes', line)
                saved = re.search(r'Pipeline native cache: saved (\d+) payload bytes', line)
                if seed: result['native_cache_seed_bytes'] = int(seed[1])
                if saved: result['native_cache_saved_bytes'] = int(saved[1])
                warm = re.search(r'Pipeline warmup: (\d+) loaded, (\d+) rejected, (\d+) records', line)
                if warm: result['warmup'] = dict(loaded=int(warm[1]), rejected=int(warm[2]), records=int(warm[3]))
                rejection = re.search(r'Pipeline warmup rejected: (compiler_mismatch|missing_compiler_stamp|missing_metadata|incompatible_metadata|invalid_spirv|stage_mismatch|unsupported_motion_feature|legacy_auxiliary_missing|specialization_conflict|record_version_or_type|record_exception|invalid_record) (\d+)', line)
                if rejection:
                    result.setdefault('startup_rejection_counts', {})[rejection[1]] = int(rejection[2])
                    result['available'] = True
                repair = re.search(r'Shader repair scan: (\d+) candidates, (\d+) previous failures unchanged', line)
                if repair:
                    result['shader_repair_scan'] = dict(candidates=int(repair[1]), suppressed_unchanged=int(repair[2]))
                    result['available'] = True
                rebuild = re.search(r'Shader rebuild summary: (\d+) rebuilt, (\d+) current, (\d+) skipped, (?:(\d+) uncaptured, )?(\d+(?:\.\d+)?) ms', line)
                if rebuild and math.isfinite(float(rebuild[5])):
                    result['shader_rebuild'] = dict(rebuilt=int(rebuild[1]), current=int(rebuild[2]),
                        skipped=int(rebuild[3]), uncaptured=int(rebuild[4] or 0), wall_ms=float(rebuild[5]))
                    result['available'] = True
                timeline = re.search(r'Pipeline timeline: (gfx_frontend|gfx_bind|gfx|compute_bind|compute) (0x[0-9a-fA-F]+) origin=(required|lookahead) queued=([\d.]+) start=([\d.]+) need=([\d.]+) queue_ms=([\d.]+) compile_ms=([\d.]+) lead_ms=(-?[\d.]+) remaining_ms=([\d.]+)', line)
                if timeline:
                    try:
                        values = list(map(float, timeline.groups()[3:]))
                        if any(not math.isfinite(v) for v in values) or any(v < 0 for i, v in enumerate(values) if i != 5):
                            raise ValueError('invalid timeline')
                        timeline_count += 1
                        event = dict(zip(('queued_at_ms', 'started_at_ms', 'needed_at_ms', 'queue_ms', 'compile_ms', 'lead_ms', 'remaining_ms'), values))
                        event.update(stage=timeline[1], hash=timeline[2], origin=timeline[3], log_line=line_number)
                        stage = timeline_stages.setdefault(timeline[1], {
                            'events': 0, 'remaining_ms': 0.0, 'peak_remaining_ms': 0.0,
                            'peak_compile_ms': 0.0,
                        })
                        stage['events'] += 1
                        stage['remaining_ms'] += event['remaining_ms']
                        stage['peak_remaining_ms'] = max(stage['peak_remaining_ms'], event['remaining_ms'])
                        stage['peak_compile_ms'] = max(stage['peak_compile_ms'], event['compile_ms'])
                        capacity = re.search(r'capacity_ms=([\d.]+)', line)
                        if capacity:
                            value = float(capacity[1])
                            if math.isfinite(value) and value >= 0: event['capacity_wait_ms'] = value
                        admission = re.search(r'admission_ms=([\d.]+)', line)
                        if admission:
                            value = float(admission[1])
                            if math.isfinite(value) and value >= 0: event['admission_wait_ms'] = value
                        timelines.append(event)
                        timelines.sort(key=lambda e: e['remaining_ms'], reverse=True)
                        del timelines[20:]
                        result['available'] = True
                    except ValueError:
                        skipped += 1
                retries = re.search(r'Pipeline retries: (\d+) passes, (\d+) draws recovered', line)
                scan = re.search(r'(Compute|Graphics) lookahead: (\d+) scanner attempts, (\d+) known selections', line)
                if scan and int(scan[3]) <= int(scan[2]):
                    result[scan[1].lower() + '_lookahead'] = {'attempts': int(scan[2]), 'known_selections': int(scan[3])}
                    result['available'] = True
                if retries:
                    result['retries'] = {'passes': int(retries[1]), 'recovered_draws': int(retries[2])}
                    result['available'] = True
                matched = False
                for kind, pattern in patterns:
                    match = pattern.search(line)
                    if not match:
                        continue
                    matched = True
                    groups = match.groups()
                    stage, key, duration = groups
                    if kind != 'foreground':
                        stage = 'graphics pipeline' if stage == 'gfx' else 'compute pipeline'
                    duration = float(duration)
                    if not math.isfinite(duration) or duration < 0 or not re.fullmatch(r'(?:0x)?[0-9a-fA-F]+', key):
                        skipped += 1
                        break
                    result['available'] = True
                    if kind == 'foreground':
                        counts[stage] += 1
                        foreground_ms += duration
                    elif kind == 'background':
                        background_events += 1
                        background_ms += duration
                    else:
                        wait_events += 1
                        wait_ms += duration
                    largest.append({'kind': kind, 'stage': stage, 'hash': key, 'duration_ms': duration, 'log_line': line_number})
                    largest.sort(key=lambda event: event['duration_ms'], reverse=True)
                    del largest[20:]
                    break
                if not matched and any(prefix in line for prefix in ('Compile stall:', 'Pipeline prepared:', 'Pipeline preparation wait:')):
                    skipped += 1
    except OSError:
        pass  # Keep partial streamed evidence and the same report shape.
    result.update(foreground_slow_events=counts, foreground_slow_ms=foreground_ms,
                  background_slow_events=background_events, background_slow_ms=background_ms,
                  logged_wait_events=wait_events, logged_wait_ms=wait_ms,
                  preparation_failures=failures, skipped_events=skipped, largest_events=largest)
    result['required_miss_reasons'] = required_reasons
    result['timeline_events'] = timeline_count
    result['timeline_stages'] = timeline_stages
    result['timeline_largest'] = timelines
    result['frontend_largest'] = frontend_events
    return result


def resource_work_summary(path):
    """Optional CPU-side timing/counter columns; older captures stay readable."""
    fields = ('hash_ms hash_snapshot_ms hash_mb hash_reused_mb unchanged_mip_mb '
              'hash_parallel_jobs detile_ms upload_record_ms host_copy_ms detile_scratch_reuses').split()
    totals = dict.fromkeys(fields, 0.0)
    peaks = dict.fromkeys(fields, 0.0)
    samples = skipped = 0
    try:
        with Path(path).open(encoding='utf-8') as stream:
            reader = csv.DictReader(stream)
            if not set(fields) <= set(reader.fieldnames or ()):
                return {'available': False}
            optional = ('image_alloc_ms', 'image_view_ms', 'image_format_ms', 'image_recycle_hits',
                        'image_query_ms', 'image_vma_ms', 'image_query_hits', 'image_query_misses',
                        'image_query_bypasses')
            for key in optional:
                if key in (reader.fieldnames or ()):
                    fields.append(key); totals[key] = peaks[key] = 0.0
            for row in reader:
                try:
                    values = {key: float(row[key]) for key in fields}
                    frame_ms = float(row['frame_ms'])
                    if not math.isfinite(frame_ms) or frame_ms <= 0 or any(not math.isfinite(v) or v < 0 for v in values.values()):
                        raise ValueError('invalid resource-work sample')
                except (KeyError, TypeError, ValueError):
                    skipped += 1
                    continue
                samples += 1
                for key, value in values.items():
                    totals[key] += value
                    peaks[key] = max(peaks[key], value)
    except OSError:
        return {'available': False}
    return {'available': bool(samples), 'samples': samples, 'skipped_rows': skipped,
            'totals': totals, 'peaks': peaks,
            'note': 'CPU operation wall times can overlap/nest; do not add them to frame time. Detile/upload timings are CPU command setup, not GPU transfer duration. MB is decimal.'}


def texture_activity_summary(path):
    """Bounded sampled evidence; cumulative counters for an object are not summed."""
    objects, creates, allocations = {}, [], []
    reuse_summary = {}
    events = 0
    try:
        with Path(path).open(encoding='utf-8', errors='replace') as stream:
            for line_number, line in enumerate(stream, 1):
                allocation = re.search(r'Image allocation: width=(\d+) height=(\d+) depth=(\d+) format=(\d+) mips=(\d+) layers=(\d+) bytes=(\d+) dedicated=([01]) query_ms=([\d.]+) vma_ms=([\d.]+)', line)
                if allocation:
                    try:
                        query, vma = map(float, allocation.groups()[-2:])
                        if not all(math.isfinite(v) and v >= 0 for v in (query, vma)):
                            continue
                        event = dict(zip(('width', 'height', 'depth', 'format', 'mips', 'layers', 'size_bytes'), map(int, allocation.groups()[:7])))
                        event.update(dedicated=bool(int(allocation[8])), query_ms=query, vma_ms=vma,
                                     wall_ms=query + vma, log_line=line_number)
                        cache = re.search(r'query_cache=(hit|miss|bypass|skipped)\b', line)
                        if cache: event['query_cache'] = cache[1]
                        allocations.append(event)
                        allocations.sort(key=lambda e: e['wall_ms'], reverse=True)
                        del allocations[20:]
                    except ValueError:
                        pass
                match = re.search(r'Texture refresh: uid=(\d+) address=(0x[0-9a-fA-F]+) size=(\d+) count=(\d+) flags=(\d+) new_backing=([01]) source_total_mb=([\d.]+) uploaded_mb=([\d.]+) unchanged_mb=([\d.]+) outcome=(uploaded|whole_match|mips_match)', line)
                if match:
                    uid, address, size, count, flags, backing, source, uploaded, unchanged, outcome = match.groups()
                    numbers = [float(source), float(uploaded), float(unchanged)]
                    if any(not math.isfinite(n) or n < 0 for n in numbers): continue
                    events += 1
                    if uid in objects or len(objects) < 256:
                        objects[uid] = dict(uid=int(uid), address=address, size_bytes=int(size), refresh_count=int(count),
                            flags=int(flags), new_backing=bool(int(backing)), source_total_mb=numbers[0],
                            uploaded_mb=numbers[1], unchanged_mb=numbers[2], outcome=outcome, log_line=line_number)
                        gpu = re.search(r'gpu_source=([01]) gpu_writes=(\d+) write_address=(0x[0-9a-fA-F]+|0) write_size=(\d+) hash_reused_mb=([\d.]+)', line)
                        if gpu and math.isfinite(float(gpu[5])):
                            objects[uid].update(gpu_source=bool(int(gpu[1])), gpu_write_count=int(gpu[2]),
                                gpu_write_address=gpu[3], gpu_write_size=int(gpu[4]), hash_reused_mb=float(gpu[5]))
                        check = re.search(r'hash_check=([01]) hash_blockers=(0x[0-9a-fA-F]+|0) hash_candidates=(\d+) hash_hits=(\d+)', line)
                        if check:
                            names = ('disabled tracking_off not_gpu_dirty not_gpu_source backing_changed mips_missing cpu_dirty '
                                     'not_fully_tracked tracking_gap mapping_busy mapping_changed host_write_busy host_reference_invalid host_write_changed').split()
                            mask = int(check[2], 0)
                            objects[uid].update(hash_reuse_checked=bool(int(check[1])), hash_blockers=mask,
                                hash_block_reasons=[name for bit,name in enumerate(names) if mask & (1 << bit)],
                                hash_candidates=int(check[3]), hash_hits=int(check[4]))
                summary = re.search(r'GPU hash reuse: candidates=(\d+) reused=(\d+)', line)
                if summary and int(summary[2]) <= int(summary[1]):
                    names = ('disabled tracking_off not_gpu_dirty not_gpu_source backing_changed mips_missing cpu_dirty '
                             'not_fully_tracked tracking_gap mapping_busy mapping_changed host_write_busy host_reference_invalid host_write_changed').split()
                    counts = {name: re.search(r'\b' + name + r'=(\d+)\b', line) for name in names}
                    if all(counts.values()) and int(summary[1]) >= reuse_summary.get('candidates', 0):
                        reuse_summary = dict(candidates=int(summary[1]), reused=int(summary[2]),
                            blocked={name: int(value[1]) for name,value in counts.items()}, log_line=line_number,
                            note='Cumulative multi-cause counts, sampled across threads; do not sum reasons or repeated snapshots.')
                created = re.search(r'Texture create: uid=(\d+) address=(0x[0-9a-fA-F]+) size=(\d+) width=(\d+) height=(\d+) format=(\d+) ms=([\d.]+)', line)
                if created:
                    ms = float(created[7])
                    if not math.isfinite(ms) or ms < 0: continue
                    creates.append(dict(uid=int(created[1]), address=created[2], size_bytes=int(created[3]),
                        width=int(created[4]), height=int(created[5]), format=int(created[6]), wall_ms=ms, log_line=line_number))
                    creates.sort(key=lambda event: event['wall_ms'], reverse=True)
                    del creates[20:]
    except OSError:
        pass
    return dict(available=bool(events or creates or reuse_summary or allocations), hash_reuse=reuse_summary, sampled_refresh_events=events,
                repeated_images=sorted(objects.values(), key=lambda event: event['source_total_mb'], reverse=True)[:20],
                slow_creates=creates, slow_allocations=allocations, note='Sampled per-object cumulative evidence. Do not sum snapshots or infer all invalidations. Dirty flags overlap; new_backing also covers layout changes. Allocation query and VMA times are parts of image-create time, not additional costs.')


def upload_summary(path):
    """Separate source-byte counters and nearby frame times, without causal inference."""
    samples = skipped = large = slow_large = 0
    image_peak = buffer_peak = allocation_peak = 0.0
    largest = []
    try:
        with Path(path).open(encoding='utf-8') as stream:
            for row in csv.DictReader(stream):
                try:
                    values = {key: float(row[key]) for key in ('t_s', 'frame_ms', 'image_mb', 'buffer_mb', 'alloc_mb')}
                    if any(not math.isfinite(value) or value < 0 for value in values.values()) or values['frame_ms'] == 0:
                        raise ValueError('invalid upload sample')
                except (KeyError, TypeError, ValueError):
                    skipped += 1
                    continue
                samples += 1
                image_peak = max(image_peak, values['image_mb'])
                buffer_peak = max(buffer_peak, values['buffer_mb'])
                allocation_peak = max(allocation_peak, values['alloc_mb'])
                if max(values['image_mb'], values['buffer_mb']) >= 64:
                    large += 1
                    slow_large += values['frame_ms'] > 1000/30
                    largest.append(values)
                    largest.sort(key=lambda event: event['frame_ms'], reverse=True)
                    del largest[20:]
    except OSError:
        return {'available': False}
    return {'available': bool(samples), 'samples': samples, 'skipped_rows': skipped,
            'peak_image_mb_per_flip': image_peak, 'peak_buffer_mb_per_flip': buffer_peak,
            'peak_allocation_mb_per_flip': allocation_peak,
            'flips_with_upload_counter_at_least_64_mb': large,
            'large_upload_flips_over_33_33_ms': slow_large, 'slowest_large_upload_flips': largest,
            'measurement': 'separate image source mip bytes, buffer upload bytes and allocation bytes per flip; decimal MB',
            'note': 'Image/buffer counters can describe the same data at different stages; do not add them. Co-occurrence does not prove a cause. Older builds count full image refresh attempts, including skipped mips.'}


def classify_exit(code, phase, fault=None, log_tail=''):
    fault = fault or {}
    exception = str(fault.get('exception_code', '')).lower()
    evidence = 'native exception record'
    if not exception:
        found = re.search(r'(?:Guest|Host) fault (0x[0-9a-fA-F]{8})', log_tail)
        exception = found.group(1).lower() if found else ''
        evidence = 'native exception log'
    if exception:
        reasons = {'0xc0000005': 'access_violation', '0xc000001d': 'illegal_instruction',
                   '0xc00000fd': 'stack_overflow', '0xc0000094': 'divide_by_zero',
                   '0xe0424223': 'renderer_assertion'}
        return {'category': 'crash', 'reason': reasons.get(exception, 'native_exception'),
                'exception_code': exception, 'evidence': evidence}
    unsigned = int(code) & 0xffffffff
    if code == 0:
        return {'category': 'normal', 'reason': 'normal_exit'}
    if code == 130:
        return {'category': 'interrupted', 'reason': 'interrupted'}
    if phase != 'runtime':
        return {'category': 'startup_error', 'reason': 'preparation_failed', 'stage': phase}
    if 'VK_ERROR_DEVICE_LOST' in log_tail or 'ErrorDeviceLost' in log_tail:
        return {'category': 'crash', 'reason': 'device_lost', 'evidence': 'Vulkan error in log'}
    if 'VK_ERROR_OUT_OF_DEVICE_MEMORY' in log_tail or 'ErrorOutOfDeviceMemory' in log_tail:
        return {'category': 'crash', 'reason': 'device_memory_error', 'evidence': 'Vulkan error in log'}
    if unsigned == 0xc0000135:
        return {'category': 'startup_error', 'reason': 'missing_dependency'}
    if unsigned == 0xc000007b:
        return {'category': 'startup_error', 'reason': 'invalid_binary'}
    if unsigned in (0xc0000005, 0xc000001d, 0xc00000fd, 0xc0000094):
        result = classify_exit(code, phase, {'exception_code': f'0x{unsigned:08x}'})
        result.update(evidence='process exit code only',
                      note='No exception context was recorded; the fault instruction and cause are unknown.')
        return result
    return {'category': 'abnormal_exit', 'reason': 'unknown_exit',
            'note': 'The exit code alone does not establish the root cause.'}


class Session:
    def __init__(self, root, env):
        self.root = Path(root)
        self.env = env
        self.directory = Path(env.get('BB_SESSION_DIR') or self.root / 'logs' /
                              (datetime.now().strftime('%Y-%m-%d_%H-%M-%S') + '-' + uuid.uuid4().hex[:8]))
        # In-game restart inherits the environment. Never overwrite its previous session.
        if (self.directory / 'session.json').exists():
            self.directory = self.directory.parent / (datetime.now().strftime('%Y-%m-%d_%H-%M-%S') + '-' + uuid.uuid4().hex[:8])
            env['BB_LOG_FILE'] = str(self.directory / 'session.log')
        self.directory.mkdir(parents=True, exist_ok=True)
        env['BB_SESSION_DIR'] = str(self.directory.resolve())
        env.setdefault('BB_LOG_FILE', str(self.directory / 'session.log'))
        if env.get('BB_FRAME_STATS') == '1':
            env['BB_FRAME_LOG'] = str(self.directory / 'frames.csv')
            env['BB_VRAM_LOG'] = str(self.directory / 'vram.csv')
        if env.get('BB_GPU_PROFILE') == '1':
            env['BB_GPU_LOG'] = str(self.directory / 'gpu.csv')
        self.report = {'schema_version': 1, 'started_utc': datetime.now(timezone.utc).isoformat(),
                       'status': 'running', 'stage': 'startup', 'platform': platform.platform(),
                       'processor': platform.processor(), 'log': str(Path(env['BB_LOG_FILE']).resolve()),
                       'native_build': read_json(self.root / 'out/native-build.json')}
        self.set_phase('startup')

    def snapshot(self):
        self.report['settings'] = {}
        try:
            for line in Path(self.env.get('BB_CONFIG', self.root / 'bbport.ini')).read_text(errors='replace').splitlines():
                key, _, value = line.partition('=')
                if key.strip() in SAFE_SETTINGS:
                    self.report['settings'][key.strip()] = value.strip()[:256]
        except OSError:
            pass
        self.report['environment'] = {key: self.env[key][:256] for key in SAFE_ENV if key in self.env}

    def record_red_zone(self, path):
        """Called only after this session's successful guest linker step."""
        protection = read_json(path).get('red_zone', {})
        keys = ('enabled', 'table_sha256', 'total_sites', 'applied_sites',
                'protected_stores', 'functions', 'uncovered_stores',
                'trampoline_base', 'trampoline_bytes')
        self.report['red_zone'] = {key: protection[key] for key in keys if key in protection}
        for key in ('changed_sites', 'conflicting_sites', 'outside_code_sites'):
            self.report['red_zone'][key] = len(protection.get(key, []))

    def record_runtime(self, executable):
        identity, manifest = identify_runtime(executable)
        self.report['runtime_identity'] = identity
        self.report['native_build'] = manifest
        write_json(self.directory / 'session.json', self.report)
        return identity

    def set_phase(self, phase):
        self.report['stage'] = phase
        self.snapshot()
        write_json(self.directory / 'session.json', self.report)

    def finish(self, code):
        self.snapshot()
        try:
            with Path(self.env['BB_LOG_FILE']).open('rb') as stream:
                stream.seek(max(0, stream.seek(0, 2) - 1024 * 1024))
                tail = stream.read().decode('utf-8', errors='replace')
        except OSError:
            tail = ''
        fault = read_json(self.directory / 'fault.json')
        self.report.update(status='finished', ended_utc=datetime.now(timezone.utc).isoformat(),
                           exit_code=code, exit_code_hex=f'0x{code & 0xffffffff:08X}',
                           outcome=classify_exit(code, self.report['stage'], fault, tail),
                           performance=frame_summary(self.directory / 'frames.csv'),
                           gpu_performance=gpu_summary(self.directory / 'gpu.csv'),
                           compilation=compilation_summary(self.env['BB_LOG_FILE']),
                           resource_uploads=upload_summary(self.directory / 'frames.csv'),
                           resource_work=resource_work_summary(self.directory / 'frames.csv'),
                           texture_activity=texture_activity_summary(self.env['BB_LOG_FILE']),
                           memory=memory_summary(self.directory / 'vram.csv'), native_fault=fault)
        self.report['minidump'] = read_json(self.directory / 'minidump.json')
        self.report['diagnostics'] = read_json(self.directory / 'diagnostics.json')
        if self.report['outcome']['category'] == 'crash':
            fault_path = self.directory / 'fault.json'
            try:
                fault_state = 'recorded' if fault else 'empty' if fault_path.stat().st_size == 0 else 'invalid'
            except OSError:
                fault_state = 'missing_or_unreadable'
            self.report['crash_evidence'] = {
                'fault_record': fault_state,
                'dump_present': (self.directory / 'crash.dmp').is_file(),
                'diagnostics_available': bool(self.report['diagnostics']),
                'note': 'An exit code alone does not identify the fault instruction or cause.',
            }
        self.report['artifacts'] = [path.name for path in self.directory.iterdir() if path.is_file() and not path.name.endswith('.tmp')]
        write_json(self.directory / 'session.json', self.report)
        outcome = self.report['outcome']
        text = [f"Techmo session: {outcome['category']} ({outcome['reason']})",
                f"Exit: {code} / {self.report['exit_code_hex']}", f"Stage: {self.report['stage']}"]
        identity = self.report.get('runtime_identity', {})
        if identity:
            text += [f"Runtime EXE SHA-256: {identity.get('sha256', 'unavailable')}; manifest: {identity.get('manifest_status', 'unavailable')}"]
        if 'crash_evidence' in self.report:
            evidence = self.report['crash_evidence']
            text += [f"Crash evidence: fault={evidence['fault_record']}; dump present={evidence['dump_present']}"]
            if not fault:
                text += ['Fault instruction/cause unavailable; inspect diagnostics.json and minidump.json for collection failures.']
        performance = self.report['performance']
        if performance.get('available'):
            text += [f"Average: {performance['average_fps']:.2f} FPS; 1% low: {performance['one_percent_low_fps']:.2f} FPS",
                     f"Frame time p95/p99: {performance['p95_frame_ms']:.2f}/{performance['p99_frame_ms']:.2f} ms",
                     f"Scope: {performance['scope']}"]
        memory = self.report['memory']
        compilation = self.report['compilation']
        blocking = performance.get('pipeline_blocking')
        if blocking:
            text += [f"Pipeline blocking (frame CSV, includes native bind): {blocking['total_ms']:.2f} ms total; peak {blocking['peak_ms']:.2f} ms; {blocking['frames_over_2_ms']} frames over 2 ms"]
        if compilation.get('available'):
            text += [f"Slow foreground compile events: {sum(compilation['foreground_slow_events'].values())}; accumulated: {compilation['foreground_slow_ms']:.2f} ms",
                     f"Legacy wait log events: {compilation['logged_wait_events']}; accumulated: {compilation['logged_wait_ms']:.2f} ms (excludes native bind timelines)"]
            native = compilation.get('timeline_stages', {}).get('gfx_bind')
            if native:
                text += [f"Native graphics bind timelines: {native['events']} logged; peak remaining wait {native['peak_remaining_ms']:.2f} ms; peak native compile {native['peak_compile_ms']:.2f} ms"]
            if blocking or native:
                text += ['Pipeline counters and timeline/preparation waits overlap; do not add them.']
            if 'preparation' in compilation:
                prep = compilation['preparation']
                text += [f"Prepared pipelines: {prep['adopted']} adopted / {prep['queued']} queued; total foreground waits: {prep['foreground_wait_ms']:.2f} ms"]
            if 'compute_preparation' in compilation:
                prep = compilation['compute_preparation']
                text += [f"Compute pipelines: {prep['adopted']} adopted / {prep['queued']} queued; total foreground waits: {prep['foreground_wait_ms']:.2f} ms"]
            if 'required_graphics_submissions' in compilation or 'required_compute_submissions' in compilation:
                text += [f"Required compiler submissions: graphics {compilation.get('required_graphics_submissions', 0)}, compute {compilation.get('required_compute_submissions', 0)} (included in queued totals)"]
            if 'retries' in compilation:
                retries = compilation['retries']
                text += [f"Preparation retries: {retries['passes']} passes; {retries['recovered_draws']} draws recovered"]
        uploads = self.report['resource_uploads']
        work = self.report['resource_work']
        if work.get('available'):
            totals = work['totals']
            text += [f"Image hashing: {totals['hash_mb']:.1f} MB in {totals['hash_ms']:.2f} ms; snapshot {totals['hash_snapshot_ms']:.2f} ms; {totals['hash_parallel_jobs']:.0f} parallel jobs",
                     f"Unchanged mips avoided: {totals['unchanged_mip_mb']:.1f} MB; detile scratch reuses: {totals['detile_scratch_reuses']:.0f}"]
        if work.get('available') and 'image_alloc_ms' in work['totals']:
            totals = work['totals']
            text += [f"Image setup CPU: allocation {totals['image_alloc_ms']:.2f} ms; views {totals.get('image_view_ms', 0):.2f} ms; format queries {totals.get('image_format_ms', 0):.2f} ms; retired-image reuses {totals.get('image_recycle_hits', 0):.0f}"]
            if 'image_query_ms' in totals:
                text += [f"Image allocation size queries: {totals['image_query_ms']:.2f} ms; VMA creation/allocation {totals.get('image_vma_ms', 0):.2f} ms; cache hits/misses/bypasses {totals.get('image_query_hits', 0):.0f}/{totals.get('image_query_misses', 0):.0f}/{totals.get('image_query_bypasses', 0):.0f}"]
        if uploads.get('available'):
            text += [f"Upload peaks per flip: image {uploads['peak_image_mb_per_flip']:.2f} MB; buffer {uploads['peak_buffer_mb_per_flip']:.2f} MB (separate stages)",
                     f"Large upload flips >=64 MB: {uploads['flips_with_upload_counter_at_least_64_mb']}; also >33.33 ms: {uploads['large_upload_flips_over_33_33_ms']}"]
        if memory.get('available'):
            text += [f"VRAM driver estimate peak: {memory['peak_device_usage_bytes'] / (1024**2):.1f} MiB; minimum budget: {memory['minimum_device_budget_bytes'] / (1024**2):.1f} MiB",
                     f"Cache pressure samples: {memory['pressure_samples']}; critical: {memory['critical_samples']}",
                     f"Buffer evictions: {memory['buffer_evicted_bytes'] / (1024**2):.1f} MiB; allocations returned to Vulkan: {memory['buffer_freed_bytes'] / (1024**2):.1f} MiB"]
        text += ['Evidence and settings: session.json', 'Runtime/preparation log: session.log',
                 'Crash signatures identify the failure, not necessarily its underlying cause.']
        (self.directory / 'summary.txt').write_text('\n'.join(text) + '\n', encoding='utf-8')
