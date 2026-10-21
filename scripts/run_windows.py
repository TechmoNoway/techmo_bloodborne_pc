#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows entry point: prepare the game image, compile the patches and start
out/bb-probe.exe using the environment and bbport.ini settings.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
../CUSA03173. Unless BB_PREBUILT=1
the port is (re)built first through MSYS2 (build.sh in the CLANG64 environment)."""
import atexit
from datetime import datetime
import os
from pathlib import Path
import shlex
import subprocess
import sys
import struct
import zlib
import traceback
from mods import remove_overlay
from session_diagnostics import Session

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PYTHON = sys.executable
LOG_STREAM = None
SESSION = None


def phase(name):
    if SESSION is not None:
        SESSION.set_phase(name)


def init_session_log():
    """Route launcher/runtime diagnostics to BB_LOG_FILE when the GUI launcher supplies one."""
    global LOG_STREAM
    path = os.environ.get('BB_LOG_FILE')
    if not path:
        return
    log_path = Path(path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    LOG_STREAM = log_path.open('a', encoding='utf-8', errors='backslashreplace', buffering=1)
    atexit.register(LOG_STREAM.close)
    sys.stdout = LOG_STREAM
    sys.stderr = LOG_STREAM
    print('=' * 80)
    print(f'Bloodborne PC session log started: {datetime.now().isoformat(timespec="seconds")}')
    print(f'Launcher root: {ROOT}')
    print(f'Log file: {log_path.resolve()}')
    print('=' * 80, flush=True)


def msys_root():
    return Path(os.environ.get('BB_MSYS2', r'C:\msys64'))


def run(arguments, capture=False, check=True, env=None):
    result = subprocess.run(
        [str(a) for a in arguments],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE if capture else LOG_STREAM,
        stderr=LOG_STREAM,
        text=True,
    )
    if capture and LOG_STREAM is not None and result.stdout:
        LOG_STREAM.write(result.stdout)
        if not result.stdout.endswith('\n'):
            LOG_STREAM.write('\n')
        LOG_STREAM.flush()
    if check and result.returncode:
        print(f'Command failed with exit code {result.returncode}: '
              f'{" ".join(shlex.quote(str(a)) for a in arguments)}', flush=True)
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config)."""
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2, or BB_PREBUILT=1 with a built out/)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    command = ('local_bin="$PWD/builds/_internal/toolchain/clang64/bin"; '
               'if [[ -f "$local_bin/clang.exe" ]]; then '
               'export PATH="$local_bin:/usr/bin:/c/Program Files/Git/cmd:$PATH"; fi; bash build.sh')
    run([bash, '-lc', command], env=env)


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def configure_frame_rate(env):
    """Resolve gameplay pace; the independent title/loading guard remains at 60.

    The user's presentation choice is preserved for every frame-rate preset.
    """
    fps = env.get('BB_FPS', '60')
    if fps not in ('30', '60', '90', 'uncap'):
        print(f'Unsupported Windows FPS preset {fps!r}; using 60 FPS.', flush=True)
        fps = '60'
    env.update(BB_FPS=fps, BB_FPS_LIMIT='0' if fps == 'uncap' else fps,
               BB_VBLANK_HZ='480' if fps == 'uncap' else '90' if fps == '90' else '60')
    return fps


def main():
    phase('validation')
    arguments = sys.argv[1:]
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', ROOT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = Path(os.environ['BB_CONFIG'])
    from online import configure_online, read_settings
    try:
        override = configure_online(read_settings(config) if config.exists() else {}, data, os.environ)
        print('Online: configured server profile and automatic game redirect' if override else 'Online: offline mode')
    except (OSError, ValueError) as error:
        sys.exit(f'Online configuration: {error}')
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    # The last folder that worked is remembered, so run.bat alone starts the game afterwards.
    remembered = out / 'game_dir.txt'
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else ROOT.parent / 'CUSA03173'
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)
    # The in-game menu's "Apply and restart" runs this launcher again (probe.c runtime_restart).
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    phase('mods')
    merged = Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    overlay = merged if merged.resolve() != original else None
    memory_leases = []
    try:
        for script, extra in (('prepare.py', []), ('link_libc.py', []),
                              ('content_profile.py', ['--sku', os.environ.get('BB_CONTENT_SKU', 'full')])):
            phase(script.removesuffix('.py'))
            run([PYTHON, SCRIPTS / script, merged, '--out', out, *extra])
        prebuilt = os.environ.get('BB_PREBUILT') == '1'
        if not prebuilt:
            phase('build')
            build()
        # Engine features are prepared locally and only mounted after runtime byte checks.
        for key in ('BB_DREAM_MIRROR_ASSET', 'BB_ENGINE_IMAGE_SHA256', 'BB_PC_MENU_ASSETS', 'BB_REBIRTH_ASSET'):
            os.environ.pop(key, None)
        # Quit Game is available with controller-only settings too; it does not depend
        # on enabling camera, graphics or keyboard/mouse enhancements.
        try:
            from engine_assets import prepare_mirror, verify_executable, prepare_menu_assets
            os.environ['BB_ENGINE_IMAGE_SHA256'] = verify_executable(out / 'eboot.elf')
            if settings_value(config, 'change_appearance') == '1':
                asset = prepare_mirror(merged, out / 'eboot.elf', data / 'engine-assets')
                os.environ['BB_DREAM_MIRROR_ASSET'] = str(asset)
                print(f'Engine: Dream mirror prepared locally: {asset}', flush=True)
            try:
                menus = prepare_menu_assets(merged, out / 'eboot.elf', data / 'engine-assets',
                                            out / 'bb-engine-assets.exe', rebirth=settings_value(config, 'rebirth') == '1')
                os.environ['BB_PC_MENU_ASSETS'] = str(menus)
                print(f'Engine: native PC menus prepared locally: {menus}', flush=True)
                if settings_value(config, 'rebirth') == '1':
                    os.environ['BB_REBIRTH_ASSET'] = str(menus / 'dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx')
            except (OSError, ValueError) as error:
                print(f'Engine: native PC menus disabled: {error}', flush=True)
        except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
            print(f'Engine: enhancement preparation skipped: {error}', flush=True)
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if os.environ.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                os.environ.pop(key, None)
        # The read-only title/loading guard works even when optional PC features or
        # local menu asset preparation are disabled. Unknown images retain its 60 FPS ceiling.
        if not os.environ.get('BB_ENGINE_IMAGE_SHA256'):
            try:
                from engine_assets import verify_executable
                os.environ['BB_ENGINE_IMAGE_SHA256'] = verify_executable(out / 'eboot.elf')
            except (OSError, ValueError, IndexError, struct.error) as error:
                print(f'Frame pacing: unknown executable; retaining safe menu ceiling: {error}', flush=True)
        fps = configure_frame_rate(os.environ)
        scaled_render = scaled_output = None
        if not os.environ.get('BB_RENDER_RES'):
            sizes = run([PYTHON, SCRIPTS / 'patches.py', '--print-scaled', '--settings', config], capture=True, check=False)
            if sizes and len(sizes.split()) == 2:
                scaled_render, scaled_output = sizes.split()
        live = '0'
        if scaled_output:
            live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = out / 'bb-gpu-capabilities.exe'
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
        phase('memory_preflight')
        from memory_preflight import configure_memory
        probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
        try:
            render,output=configure_memory(probe,os.environ,
                os.environ.get('BB_RENDER_RES'),os.environ.get('BB_OUTPUT_RES'),live=='1',leases=memory_leases)
            if render:
                os.environ['BB_RENDER_RES']=render
        except (OSError,ValueError) as error:
            sys.exit(f'Memory preflight: {error}')
        phase('patches')
        run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
             '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
             '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
             '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
             '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
        # Selected writes must be known before creating red-zone detours: an
        # external patch may otherwise replace a detour or only part of it.
        phase('link_modules')
        run([PYTHON, SCRIPTS / 'link_modules.py', merged, '--out', out,
             '--runtime-patches', out / 'patches.bin'])
        if SESSION is not None:
            SESSION.record_red_zone(out / 'link.json')
        probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
        phase('native_startup')
        if not probe.is_file():
            sys.exit(f'Native runtime not found: {probe}. Build Techmo with tools/build-native.ps1 first.')
        if SESSION is not None:
            identity = SESSION.record_runtime(probe)
            print(f"Runtime identity: SHA-256={identity.get('sha256', 'unavailable')}; "
                  f"manifest={identity.get('manifest_status', 'unavailable')}", flush=True)
            if build_identity := identity.get('build_identity'):
                print(f"Runtime build: app={build_identity.get('app_version', 'unknown')}; "
                      f"source={build_identity.get('description', build_identity.get('commit', 'unknown'))}; "
                      f"modified={build_identity.get('dirty', 'unknown')}", flush=True)
            if identity.get('manifest_match') is False:
                print('Runtime identity: build metadata differs from the selected EXE; version claims are unverified.', flush=True)
        command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
                   '--patches', out / 'patches.bin', '--app0', merged,
                   '--user', os.environ.get('BB_USER_DIR', data / 'user'),
                   '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
        # MSYS2's DLLs (libc++, SDL3, FFmpeg, ...). System32 is searched before PATH, so the
        # Vulkan loader stays the one installed with the GPU driver.
        local_bin = ROOT / 'builds/_internal/toolchain/clang64/bin'
        os.environ['PATH'] = os.pathsep.join([str(probe.parent), str(local_bin),
                                            str(msys_root() / 'clang64/bin'), os.environ.get('PATH', '')])
        print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
        phase('runtime')
        try:
            status = subprocess.call(
                [str(c) for c in command],
                cwd=ROOT,
                stdout=LOG_STREAM,
                stderr=LOG_STREAM,
            )
        except KeyboardInterrupt:
            status = 130
        print(f'bb-probe exited with code {status}', flush=True)
        return status
    finally:
        for lease in memory_leases:
            lease.close()
        if overlay:
            remove_overlay(overlay)


def entry():
    global SESSION
    SESSION = Session(Path(os.environ.get('BB_DATA_DIR', ROOT)), os.environ)
    init_session_log()
    code = 1
    try:
        code = main()
    except SystemExit as error:
        code = error.code if isinstance(error.code, int) else 1
        if error.code and not isinstance(error.code, int):
            print(error.code, file=sys.stderr, flush=True)
    except KeyboardInterrupt:
        code = 130
    except Exception:
        traceback.print_exc()
    finally:
        if LOG_STREAM:
            LOG_STREAM.flush()
        try:
            SESSION.finish(code)
        except (OSError, ValueError) as error:
            print(f'Could not finish session report: {error}', file=sys.stderr, flush=True)
    return code


if __name__ == '__main__':
    sys.exit(entry())
