"""Verify that a portable package carries the exact recorded native runtime."""
import argparse
import hashlib
import json
from pathlib import Path


REQUIRED_BINARIES = {
    'bb-probe.exe', 'bb-gpu-capabilities.exe',
    'bb-engine-assets.exe', 'bb-crash-reporter.exe',
    'bb-shader-lab.exe',
}


def verify(package_out):
    package_out = Path(package_out)
    manifest = json.loads((package_out / 'native-build.json').read_text(encoding='utf-8'))
    if manifest.get('schema_version') != 1:
        raise ValueError('Unsupported native-build.json schema')
    binaries = manifest['binaries']
    dlls = manifest['runtime_dlls']
    missing = REQUIRED_BINARIES - set(binaries)
    if missing:
        raise ValueError(f'Native manifest is missing binaries: {sorted(missing)}')
    if 'sdl3.dll' not in {name.lower() for name in dlls}:
        raise ValueError('Native manifest is missing SDL3.dll')
    files = {name: details['sha256'] for name, details in binaries.items()}
    files.update(dlls)
    for name, expected in files.items():
        if not name or '/' in name or '\\' in name or ':' in name or name in ('.', '..'):
            raise ValueError(f'Invalid native runtime filename: {name!r}')
        path = package_out / name
        if not path.is_file():
            raise ValueError(f'Package is missing native dependency: {name}')
        with path.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != expected:
            raise ValueError(f'Package native file differs from build manifest: {name}')
    return len(binaries), len(dlls)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package_out', type=Path)
    args = parser.parse_args()
    try:
        binaries, dlls = verify(args.package_out)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Native package verification failed: {error}\n')
    print(f'Native package verified: {binaries} executables, {dlls} runtime DLLs.')


if __name__ == '__main__':
    main()
