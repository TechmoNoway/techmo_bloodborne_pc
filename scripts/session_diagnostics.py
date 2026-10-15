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

SAFE_SETTINGS = {
    'output_res', 'upscaler', 'preset', 'live_resolution', 'fullscreen', 'sharpen',
    'model_lod', 'pc_controls', 'mouse_camera', 'effect_game_aa', 'effect_ssao',
    'effect_dof', 'effect_motion_blur', 'effect_dynamic_shadows', 'effect_ssr',
    'effect_chromatic_aberration', 'graphics_controls', 'camera_controls', 'widescreen',
    'graphics_vignette', 'graphics_ao_strength', 'graphics_shadow_scale', 'graphics_bloom',
    'graphics_saturation', 'camera_fov_scale', 'camera_distance_scale', 'camera_height_scale',
    'show_fps', 'mouse_sensitivity',
}
SAFE_ENV = {
    'BB_FPS', 'BB_FPS_LIMIT', 'BB_VBLANK_HZ', 'BB_RENDER_RES', 'BB_OUTPUT_RES',
    'BB_DMEM_MB', 'BB_FRAME_STATS', 'BB_GPU_PROFILE', 'BB_MODS_ENABLED',
    'BB_PRESENT_MODE', 'BB_LIVE_RES', 'BB_TIMEOUT',
    'BB_VK_RECORD_THREADS', 'BB_VK_RECORD_THREAD', 'BB_VK_SEGMENT_KB',
    'BB_ASYNC_SUBMIT', 'BB_DIRECT_SEGMENT', 'BB_FSR4_OPT',
    'BB_GC_BUDGET_MB', 'BB_GC_IDLE_SECONDS', 'BB_GC_PRESSURE_IDLE_SECONDS', 'BB_VRAM_IDLE_SECONDS',
    'BB_VMA_DEDICATED_MB', 'BB_GUEST_IN_PLACE', 'BB_GARLIC_VRAM', 'BB_GPU_DATA_CHECK',
    'BB_PIPELINE_PREP', 'BB_STAGING_REUSE_SLACK',
    'BB_COMPUTE_PREP', 'BB_PIPELINE_RETRY', 'BB_PREP_WORKERS',
    'BB_RED_ZONE',
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
                    for key in ('pending_buffer_bytes', 'buffer_evicted_bytes', 'buffer_freed_bytes'):
                        values[key] = int(row[key]) if key in row else 0  # Older headers remain readable.
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
                 'pending_image_bytes', 'pending_buffer_bytes'):
        result['peak_' + name] = max(row[name] for row in rows)
    for name in ('buffer_evicted_bytes', 'buffer_freed_bytes'):
        result[name] = max(row[name] for row in rows)
    return result


def compilation_summary(path):
    """Stream bounded compile evidence; slow events do not count all compilations."""
    patterns = (
        ('foreground', re.compile(r'Compile stall: (shader module|graphics pipeline|compute pipeline) (\w+) (\d+(?:\.\d+)?) ms \(foreground\)')),
        ('background', re.compile(r'Pipeline prepared: (gfx|compute) (\w+) (\d+(?:\.\d+)?) ms \(background\)')),
        ('wait', re.compile(r'Pipeline preparation wait: (gfx|compute) (\w+) (\d+(?:\.\d+)?) ms')),
    )
    counts = {'shader module': 0, 'graphics pipeline': 0, 'compute pipeline': 0}
    foreground_ms = background_ms = wait_ms = 0.0
    background_events = wait_events = failures = skipped = 0
    largest = []
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
                retries = re.search(r'Pipeline retries: (\d+) passes, (\d+) draws recovered', line)
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
    return result


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
    if not exception:
        found = re.search(r'(?:Guest|Host) fault (0x[0-9a-fA-F]{8})', log_tail)
        exception = found.group(1).lower() if found else ''
    if exception:
        reasons = {'0xc0000005': 'access_violation', '0xc000001d': 'illegal_instruction',
                   '0xc00000fd': 'stack_overflow', '0xc0000094': 'divide_by_zero'}
        return {'category': 'crash', 'reason': reasons.get(exception, 'native_exception'),
                'exception_code': exception, 'evidence': 'native exception record'}
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
        return classify_exit(code, phase, {'exception_code': f'0x{unsigned:08x}'})
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
                           memory=memory_summary(self.directory / 'vram.csv'), native_fault=fault)
        self.report['minidump'] = read_json(self.directory / 'minidump.json')
        self.report['artifacts'] = [path.name for path in self.directory.iterdir() if path.is_file() and not path.name.endswith('.tmp')]
        write_json(self.directory / 'session.json', self.report)
        outcome = self.report['outcome']
        text = [f"Techmo session: {outcome['category']} ({outcome['reason']})",
                f"Exit: {code} / {self.report['exit_code_hex']}", f"Stage: {self.report['stage']}"]
        performance = self.report['performance']
        if performance.get('available'):
            text += [f"Average: {performance['average_fps']:.2f} FPS; 1% low: {performance['one_percent_low_fps']:.2f} FPS",
                     f"Frame time p95/p99: {performance['p95_frame_ms']:.2f}/{performance['p99_frame_ms']:.2f} ms",
                     f"Scope: {performance['scope']}"]
        memory = self.report['memory']
        compilation = self.report['compilation']
        if compilation.get('available'):
            text += [f"Slow foreground compile events: {sum(compilation['foreground_slow_events'].values())}; accumulated: {compilation['foreground_slow_ms']:.2f} ms",
                     f"Logged pipeline waits: {compilation['logged_wait_events']}; accumulated: {compilation['logged_wait_ms']:.2f} ms"]
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
