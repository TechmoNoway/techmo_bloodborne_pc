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
    for name in ('stage_a_cpu_ms', 'present_wait_ms', 'tick_wait_ms', 'compiles'):
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
                           gpu_performance=gpu_summary(self.directory / 'gpu.csv'), native_fault=fault)
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
        text += ['Evidence and settings: session.json', 'Runtime/preparation log: session.log',
                 'Crash signatures identify the failure, not necessarily its underlying cause.']
        (self.directory / 'summary.txt').write_text('\n'.join(text) + '\n', encoding='utf-8')
