"""Techmo session reporting around the pinned native starter."""
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime
from session_diagnostics import Session

ROOT = Path(__file__).resolve().parents[1]

def native_arguments(arguments, env):
    """Techmo accepts a game folder; 0.9.37's starter forwards all argv to bb-probe."""
    arguments = list(arguments)
    if arguments and not arguments[0].startswith('-'):
        env['BB_GAME_DIR'] = str(Path(arguments.pop(0)).expanduser().resolve())
    return arguments


def main():
    env = os.environ.copy()
    # Data/cache stay local; only an explicit launcher save/profile choice overrides saves.
    env['BB_DATA_DIR'] = str(ROOT)
    if env.pop('TECHMO_USE_SELECTED_USER', '') != '1':
        env['BB_USER_DIR'] = str(ROOT / 'user')
    env['BB_GPU_USER_DIR'] = str(ROOT / 'user')
    env['BB_CONFIG'] = str(ROOT / 'bbport.ini')
    env['PYTHONUTF8'] = '1'
    env['BB_TECHMO_BINDINGS'] = '1'
    arguments = native_arguments(sys.argv[1:], env)
    session = Session(ROOT, env)
    starter = ROOT / 'out/bb-start.exe'
    runtime = ROOT / 'out/bb-probe.exe'
    code = 1
    try:
        session.set_phase('preparation')
        if not starter.is_file() or not runtime.is_file():
            raise RuntimeError('Build the Techmo native engine first (tools/build-native.ps1).')
        identity = session.record_runtime(runtime)
        with open(env['BB_LOG_FILE'], 'a', encoding='utf-8', buffering=1) as log:
            print(f'Techmo experiment-v2, {datetime.now().isoformat(timespec="seconds")}', file=log)
            print(f'Runtime EXE SHA-256: {identity.get("sha256", "unavailable")}', file=log)
            print('Native start: ' + str(starter), file=log)
            session.set_phase('runtime')
            process = subprocess.Popen([str(starter), *arguments], cwd=ROOT, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            # The native starter and probe append to this file. Stream it to the
            # existing Game output view while preserving the structured session artifacts.
            with open(env['BB_LOG_FILE'], encoding='utf-8', errors='replace') as reader:
                while True:
                    chunk = reader.read()
                    if chunk:
                        print(chunk, end='', flush=True)
                    if process.poll() is not None:
                        print(reader.read(), end='', flush=True)
                        break
                    time.sleep(0.1)
            code = process.wait()
            print(f'bb-start exited with code {code}', file=log)
        return code
    finally:
        session.finish(code)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f'Techmo experiment-v2: {error}', file=sys.stderr)
        raise SystemExit(1)
