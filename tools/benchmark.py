"""External, PID-scoped PresentMon capture; never writes game configuration."""
import argparse
import csv
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
PM = ROOT / 'builds/_internal/benchmark-tools/PresentMon-2.6.0-x64.exe'
PM_SHA256 = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
TARGETS = {
    'lantern': (ROOT.parent / 'BloodbornePC', Path('engine/out/bb-probe.exe')),
    'techmo': (ROOT.parent / 'techmo_launcher', Path('out/bb-probe.exe')),
    'techmo-v2': (ROOT, Path('out/bb-probe.exe')),
}


def sha256(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def analyze(csv_path):
    """Never mix independent swap chains or claim present FPS equals simulation FPS."""
    groups = {}
    with open(csv_path, encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        if not {'processid', 'swapchainaddress', 'msbetweenpresents'} <= {
                name.strip().casefold() for name in (reader.fieldnames or [])}:
            raise ValueError('Not a PresentMon v1 metrics CSV')
        for row in reader:
            row = {name.strip().casefold(): value for name, value in row.items() if name}
            key = (row['processid'], row['swapchainaddress'])
            group = groups.setdefault(key, {'times': [], 'dropped': 0, 'rows': 0})
            group['rows'] += 1
            group['dropped'] += row.get('dropped') == '1'
            # First interval can begin before the capture window. Ignore it per swap chain.
            if group['rows'] == 1:
                continue
            try:
                value = float(row['msbetweenpresents'])
            except (ValueError, TypeError):
                continue
            if math.isfinite(value) and value > 0:
                group['times'].append(value)
    results = []
    for (pid, swapchain), group in groups.items():
        times = group['times']
        if not times:
            continue
        slowest = sorted(times, reverse=True)[:max(1, math.ceil(len(times) * .01))]
        results.append({
            'process_id': pid, 'swapchain': swapchain, 'frames': len(times),
            'observed_seconds': sum(times) / 1000,
            'average_present_fps': 1000 * len(times) / sum(times),
            'one_percent_low_fps': 1000 * len(slowest) / sum(slowest),
            'p99_frame_ms': percentile(times, .99), 'worst_frame_ms': max(times),
            'over_33ms': sum(v > 33 for v in times),
            'over_50ms': sum(v > 50 for v in times),
            'over_100ms': sum(v > 100 for v in times),
            'dropped_presents': group['dropped'], 'csv_rows': group['rows'],
        })
    results.sort(key=lambda value: value['frames'], reverse=True)
    return {'dominant_swapchain': results[0] if results else None, 'swapchains': results,
            'definitions': {
                'fps': '1000 / mean(MsBetweenPresents); present submission rate, not simulation or generated-frame classification',
                'one_percent_low': '1000 / mean(slowest ceil(1% of valid frame intervals))',
                'first_interval': 'Excluded separately for every process/swap chain',
                'foreground': 'Alt-tab, menus and loading remain in the data; inspect raw CSV',
            }}


def snapshot(package):
    # Do not copy account tokens, passwords, input profiles, save files or game assets.
    ini = package / 'bbport.ini'
    settings = {}
    if ini.exists():
        for line in ini.read_text(encoding='utf-8-sig').splitlines():
            if '=' not in line or line.lstrip().startswith('#'):
                continue
            key, value = line.split('=', 1)
            if not key.startswith(('online_', 'bind.')):
                settings[key] = value
    launcher = package / 'launcher-settings.json'
    if launcher.exists():
        keys = ('Resolution', 'Fullscreen', 'PresentMode', 'HdrOutput', 'Upscaler',
                'Preset', 'Fps', 'FrameStats', 'GpuProfile', 'AudioStats', 'DetailedLogs')
        values = json.loads(launcher.read_text(encoding='utf-8-sig'))
        settings['launcher'] = {key: values[key] for key in keys if key in values}
    native_launcher = package / 'user/launcher/settings.json'
    if native_launcher.exists():
        keys = ('fullscreen', 'present_mode', 'fps_mode', 'frame_stats', 'gpu_profile',
                'audio_stats', 'fsr_profile', 'legacy_shaders', 'exp_fast_draws',
                'exp_compile_skip', 'exp_occlusion', 'exp_frame_pacing', 'exp_vram_cleanup',
                'exp_vram_budget', 'draw_pipe', 'readbacks', 'sound_output')
        values = json.loads(native_launcher.read_text(encoding='utf-8-sig'))
        settings['lantern_launcher'] = {key: values[key] for key in keys if key in values}
    version = package / 'package.json'
    if version.exists():
        settings['package_version'] = json.loads(version.read_text(encoding='utf-8-sig'))
    return settings


def select_game_pid(matches, visible_pids):
    # bb-probe is also used by the headless --hold-memory-pool helper.
    # Wait for a window even when it is the only process, rather than measuring the helper.
    games = sorted(set(matches) & set(visible_pids))
    if len(games) > 1:
        raise RuntimeError('More than one game window matches this runtime '
                           f'(PIDs: {", ".join(map(str, games))}). Close the extra game window first.')
    return games[0] if games else None


class Windows:
    def __init__(self):
        self.kernel = C.WinDLL('kernel32', use_last_error=True)
        self.psapi = C.WinDLL('psapi', use_last_error=True)
        self.kernel.OpenProcess.argtypes = (W.DWORD, W.BOOL, W.DWORD)
        self.kernel.OpenProcess.restype = W.HANDLE
        self.kernel.CloseHandle.argtypes = (W.HANDLE,)
        self.kernel.QueryFullProcessImageNameW.argtypes = (W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD))
        self.kernel.GetProcessTimes.argtypes = (W.HANDLE,) + (C.POINTER(W.FILETIME),) * 4
        self.kernel.WaitForSingleObject.argtypes = (W.HANDLE, W.DWORD)
        self.psapi.EnumProcesses.argtypes = (C.POINTER(W.DWORD), W.DWORD, C.POINTER(W.DWORD))
        self.psapi.GetProcessMemoryInfo.argtypes = (W.HANDLE, C.c_void_p, W.DWORD)
        self.user = C.WinDLL('user32', use_last_error=True)
        self.window_callback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        self.user.EnumWindows.argtypes = (self.window_callback, W.LPARAM)
        self.user.IsWindowVisible.argtypes = (W.HWND,)
        self.user.GetWindowThreadProcessId.argtypes = (W.HWND, C.POINTER(W.DWORD))

    def visible_processes(self):
        pids = set()

        def visit(window, _):
            if self.user.IsWindowVisible(window):
                pid = W.DWORD()
                if self.user.GetWindowThreadProcessId(window, C.byref(pid)):
                    pids.add(pid.value)
            return True

        callback = self.window_callback(visit)
        if not self.user.EnumWindows(callback, 0):
            raise C.WinError(C.get_last_error())
        return pids

    def find(self, executable):
        ids = (W.DWORD * 8192)()
        count = W.DWORD()
        if not self.psapi.EnumProcesses(ids, C.sizeof(ids), C.byref(count)):
            raise C.WinError(C.get_last_error())
        matches = []
        for pid in ids[:count.value // C.sizeof(W.DWORD)]:
            handle = self.kernel.OpenProcess(0x1000, False, pid)
            if not handle:
                continue
            try:
                buffer = C.create_unicode_buffer(32768)
                length = W.DWORD(len(buffer))
                if self.kernel.QueryFullProcessImageNameW(handle, 0, buffer, C.byref(length)):
                    if os.path.normcase(buffer.value) == os.path.normcase(str(executable)):
                        matches.append(pid)
            finally:
                self.kernel.CloseHandle(handle)
        return select_game_pid(matches, self.visible_processes()) if matches else None


class MemoryCounters(C.Structure):
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD)] + [
        (name, C.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class CounterUnion(C.Union):
    _fields_ = [('large', C.c_longlong), ('double', C.c_double)]


class CounterValue(C.Structure):
    _fields_ = [('status', W.DWORD), ('value', CounterUnion)]


class CounterItem(C.Structure):
    _fields_ = [('name', W.LPWSTR), ('value', CounterValue)]


class Telemetry:
    def __init__(self, windows, pid):
        self.windows = windows
        self.handle = windows.kernel.OpenProcess(0x00100000 | 0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.pdh = C.WinDLL('pdh')
        self.pdh.PdhOpenQueryW.argtypes = (W.LPCWSTR, C.c_size_t, C.POINTER(W.HANDLE))
        self.pdh.PdhAddEnglishCounterW.argtypes = (W.HANDLE, W.LPCWSTR, C.c_size_t, C.POINTER(W.HANDLE))
        self.pdh.PdhCollectQueryData.argtypes = (W.HANDLE,)
        self.pdh.PdhCloseQuery.argtypes = (W.HANDLE,)
        self.pdh.PdhGetFormattedCounterArrayW.argtypes = (W.HANDLE, W.DWORD,
            C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p)
        self.query = W.HANDLE()
        self.counters = {}
        self.errors = {}
        code = self.pdh.PdhOpenQueryW(None, 0, C.byref(self.query))
        if code:
            self.errors['query'] = hex(code & 0xffffffff)
        else:
            for key, path in {
                'gpu_dedicated_mib': rf'\GPU Process Memory(pid_{pid}_*)\Dedicated Usage',
                'gpu_shared_mib': rf'\GPU Process Memory(pid_{pid}_*)\Shared Usage',
                'gpu_busiest_engine_percent': rf'\GPU Engine(pid_{pid}_*)\Utilization Percentage',
            }.items():
                counter = W.HANDLE()
                code = self.pdh.PdhAddEnglishCounterW(self.query, path, 0, C.byref(counter))
                if not code:
                    self.counters[key] = counter
                else:
                    self.errors[key] = hex(code & 0xffffffff)
        self.previous = None
        self.sample()

    def alive(self):
        return self.windows.kernel.WaitForSingleObject(self.handle, 0) == 258

    def counter(self, counter, percent):
        size, count = W.DWORD(), W.DWORD()
        fmt = 0x200 if percent else 0x400  # PDH_FMT_DOUBLE / LARGE
        code = self.pdh.PdhGetFormattedCounterArrayW(counter, fmt, C.byref(size), C.byref(count), None)
        if (code & 0xffffffff) != 0x800007D2 or not size.value:
            return None
        buffer = C.create_string_buffer(size.value)
        code = self.pdh.PdhGetFormattedCounterArrayW(counter, fmt, C.byref(size), C.byref(count), buffer)
        if code:
            return None
        items = C.cast(buffer, C.POINTER(CounterItem))
        values = [(item.value.value.double if percent else item.value.value.large)
                  for item in items[:count.value] if item.value.status in (0, 1)]
        if not values:
            return None
        return max(values) if percent else sum(values) / (1024 * 1024)

    def sample(self):
        result = {}
        memory = MemoryCounters()
        memory.cb = C.sizeof(memory)
        if self.windows.psapi.GetProcessMemoryInfo(self.handle, C.byref(memory), memory.cb):
            result['ram_working_set_mib'] = memory.WorkingSetSize / (1024 * 1024)
            result['ram_private_commit_mib'] = memory.PrivateUsage / (1024 * 1024)
        stamps = [W.FILETIME() for _ in range(4)]
        now = time.monotonic()
        if self.windows.kernel.GetProcessTimes(self.handle, *(C.byref(stamp) for stamp in stamps)):
            cpu = sum((stamp.dwHighDateTime << 32) | stamp.dwLowDateTime for stamp in stamps[2:]) / 1e7
            if self.previous:
                old_time, old_cpu = self.previous
                result['cpu_machine_percent'] = 100 * (cpu - old_cpu) / (now - old_time) / (os.cpu_count() or 1)
            self.previous = now, cpu
        if self.query and not self.pdh.PdhCollectQueryData(self.query):
            for key, counter in self.counters.items():
                result[key] = self.counter(counter, key.endswith('percent'))
        return result

    def close(self):
        if self.query:
            self.pdh.PdhCloseQuery(self.query)
        self.windows.kernel.CloseHandle(self.handle)


def capture(args):
    package, relative = TARGETS[args.target]
    executable = (package / relative).resolve()
    if not executable.is_file():
        raise RuntimeError(f'Game runtime not found: {executable}')
    if not PM.exists() or sha256(PM) != PM_SHA256:
        raise RuntimeError(f'PresentMon missing or SHA256 mismatch: {PM}')
    windows = Windows()
    print(f'Target: {args.target} / {executable}\nOpen the game and load your test location first.', flush=True)
    deadline = time.monotonic() + 300
    pid = windows.find(executable)
    while pid is None and time.monotonic() < deadline:
        time.sleep(2)
        pid = windows.find(executable)
    if pid is None:
        raise RuntimeError('Game window not found within 5 minutes. Start the game, then retry.')
    print(f'Game window found: PID {pid} (background helpers excluded).', flush=True)
    label = input('Run label (e.g. quality-warm-1): ').strip() or 'run'
    print('Frame Generation OFF; match resolution, quality, effects, FPS limit and present mode.')
    input(f'Press ENTER when ready. You have {args.delay}s to return to the game; capture lasts {args.seconds}s. ')
    # Recheck PID/path after potentially long user input; never attach to a stale PID.
    if windows.find(executable) != pid:
        raise RuntimeError('Game process changed while preparing; restart capture.')
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    folder = ROOT.parent / 'benchmarks/results' / f'{stamp}-{args.target}'
    folder.mkdir(parents=True)
    metadata = {'target': args.target, 'label': label, 'pid': pid,
                'process_selection': 'Exact executable path and visible top-level window; headless helpers excluded',
                'executable': str(executable), 'executable_sha256': sha256(executable),
                'created_utc': datetime.now(timezone.utc).isoformat(),
                'delay_seconds': args.delay, 'requested_seconds': args.seconds,
                'settings_at_start': snapshot(package),
                'presentmon_version': '2.6.0', 'presentmon_sha256': PM_SHA256,
                'telemetry_definition': '1 Hz, PID scoped; GPU memory is Windows accounting, summed across adapters; busiest engine is not whole-GPU utilization',
                'frame_generation': 'User must disable; not automatically verified',
                'route': 'Manual gameplay; no scripted movement or save changes'}
    command = [str(PM), '--process_id', str(pid), '--output_file', str(folder / 'presentmon.csv'),
               '--v1_metrics', '--no_console_stats', '--no_track_input', '--no_track_gpu',
               '--delay', str(args.delay), '--timed', str(args.seconds),
               '--terminate_after_timed', '--session_name', f'TechmoAB_{stamp}']
    metadata['command'] = command
    telemetry = Telemetry(windows, pid)
    metadata['telemetry_errors'] = telemetry.errors
    (folder / 'run.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    started = time.monotonic()
    interrupted = False
    try:
        with open(folder / 'presentmon.log', 'w', encoding='utf-8') as log, \
             open(folder / 'telemetry.csv', 'w', encoding='utf-8', newline='') as stream:
            columns = ['seconds_since_capture_start', 'ram_working_set_mib', 'ram_private_commit_mib',
                       'cpu_machine_percent', 'gpu_dedicated_mib', 'gpu_shared_mib', 'gpu_busiest_engine_percent']
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if not telemetry.alive() or elapsed > args.delay + args.seconds + 30:
                    process.send_signal(__import__('signal').CTRL_BREAK_EVENT)
                    break
                if args.delay <= elapsed < args.delay + args.seconds:
                    row = telemetry.sample()
                    row['seconds_since_capture_start'] = elapsed - args.delay
                    writer.writerow(row)
                    stream.flush()
                time.sleep(1)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
            metadata['presentmon_exit_code'] = process.returncode
    except KeyboardInterrupt:
        interrupted = True
        if 'process' in locals() and process.poll() is None:
            process.send_signal(__import__('signal').CTRL_BREAK_EVENT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait()
    finally:
        telemetry.close()
        metadata['interrupted'] = interrupted
        metadata['elapsed_seconds'] = time.monotonic() - started
        metadata['settings_at_end'] = snapshot(package)
        metadata['settings_changed'] = metadata['settings_at_start'] != metadata['settings_at_end']
        (folder / 'run.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    csv_path = folder / 'presentmon.csv'
    if not csv_path.exists():
        raise RuntimeError(f'No frame capture. Check {folder / "presentmon.log"}. ETW may require administrator rights.')
    report = analyze(csv_path)
    with open(folder / 'telemetry.csv', encoding='utf-8', newline='') as stream:
        samples = list(csv.DictReader(stream))
    peaks = {}
    for column in ('ram_working_set_mib', 'ram_private_commit_mib', 'gpu_dedicated_mib', 'gpu_shared_mib'):
        values = [float(row[column]) for row in samples if row.get(column)]
        peaks[column] = max(values) if values else None
    report['sampled_memory_peaks_mib'] = peaks
    report['settings_changed'] = metadata['settings_changed']
    (folder / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    main = report['dominant_swapchain']
    lines = [f'Target: {args.target}', f'Label: {label}', f'Runtime SHA256: {metadata["executable_sha256"]}']
    if main:
        lines += [f'Average present FPS: {main["average_present_fps"]:.2f}',
                  f'1% low FPS: {main["one_percent_low_fps"]:.2f}',
                  f'P99 / worst frame: {main["p99_frame_ms"]:.2f} / {main["worst_frame_ms"]:.2f} ms',
                  f'Frames over 33 / 50 / 100 ms: {main["over_33ms"]} / {main["over_50ms"]} / {main["over_100ms"]}',
                  f'Observed frame intervals: {main["observed_seconds"]:.2f} seconds']
    else:
        lines += ['INVALID: no valid game presents recorded.']
    lines += [f'{key} peak: {value:.2f} MiB' if value is not None else f'{key}: unavailable'
              for key, value in peaks.items()]
    lines += [f'Settings changed during run: {metadata["settings_changed"]}',
              'Frame Generation must be OFF; present FPS is not simulation FPS.',
              'Manual route. Menus, loading and alt-tab are not automatically excluded.',
              'GPU memory: Windows accounting for this PID across adapters, sampled at 1 Hz.']
    (folder / 'summary.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))
    print(f'Results: {folder}')
    if not report['dominant_swapchain']:
        raise RuntimeError('No valid game presents recorded; this is not a successful benchmark.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=TARGETS)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--delay', type=int, default=10)
    parser.add_argument('--analyze', type=Path)
    args = parser.parse_args()
    if args.analyze:
        print(json.dumps(analyze(args.analyze), indent=2))
        return
    if not args.target or not 1 <= args.seconds <= 3600 or not 0 <= args.delay <= 120:
        parser.error('--target required; seconds 1..3600; delay 0..120')
    capture(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f'ERROR: {error}', flush=True)
        raise SystemExit(1)
