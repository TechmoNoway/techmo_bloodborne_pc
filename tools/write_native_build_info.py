"""Record native provenance and copy the toolchain DLL import closure into out/."""
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def source_identity(root):
    root = Path(root)
    identity = {}
    try:
        identity['app_version'] = ET.parse(root / 'apps/launcher/Techmo.Launcher.csproj').findtext('.//Version')
    except (OSError, ET.ParseError):
        pass
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    try:
        if Path(git('rev-parse', '--show-toplevel')).resolve() != root.resolve():
            raise ValueError('Parent repository does not identify these sources')
        identity.update(commit=git('rev-parse', 'HEAD'),
                        description=git('describe', '--tags', '--always', '--dirty'),
                        dirty=bool(git('status', '--porcelain', '--untracked-files=normal')))
    except (OSError, ValueError, subprocess.CalledProcessError):
        identity['git_available'] = False
    return identity


def main():
    lock = json.loads((ROOT / 'engine.lock.json').read_text(encoding='utf-8'))
    native = ROOT / 'engine/native'
    native_out = native / 'out'
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which('clang')).resolve()
    toolchain_bin = compiler.parent
    binaries = [OUT / name for name in ('bb-probe.exe', 'bb-gpu-capabilities.exe',
                                        'bb-engine-assets.exe', 'bb-start.exe', 'bb-shader-lab.exe')]
    for binary in binaries:
        shutil.copy2(native_out / binary.name, binary)
    for name in ('gpu-cache.id', 'gpu-cache.legacy.id'):
        if (native_out / name).exists():
            shutil.copy2(native_out / name, OUT / name)
    for folder in ('patches', 'data', 'licenses'):
        target = ROOT / folder if folder != 'licenses' else ROOT / 'licenses/native'
        if (native / folder).is_dir():
            shutil.copytree(native / folder, target, dirs_exist_ok=True)
    queued = list(binaries)
    copied = set()
    # System DLLs always stay supplied by Windows/the GPU driver.
    while queued:
        binary = queued.pop()
        imports = command('llvm-objdump', '-p', str(binary))
        for line in imports.splitlines():
            if 'DLL Name:' not in line:
                continue
            name = line.split('DLL Name:', 1)[1].strip()
            if name.lower() == 'vulkan-1.dll' or name.lower() in copied:
                continue
            source = toolchain_bin / name
            if source.is_file():
                target = OUT / name
                if not target.exists() or digest(source) != digest(target):
                    shutil.copy2(source, target)
                copied.add(name.lower())
                queued.append(target)
    source_digest = hashlib.sha256()
    for folder in ('apps/launcher', 'apps/lantern-backend', 'tools', 'scripts'):
        for path in sorted((ROOT / folder).rglob('*')):
            if not path.is_file() or any(part in ('__pycache__', 'build', 'bin', 'obj') for part in path.parts):
                continue
            relative = path.relative_to(ROOT).as_posix()
            source_digest.update(relative.encode() + b'\0' + bytes.fromhex(digest(path)))
    for name in ('engine/bbhost-changes.patch', 'engine/techmo-compat.patch', 'engine/win_compat.h', 'engine.lock.json', 'build.sh', 'run.bat'):
        source_digest.update(name.encode() + b'\0' + bytes.fromhex(digest(ROOT / name)))
    manifest = {
        'schema_version': 1, 'built_utc': datetime.now(timezone.utc).isoformat(),
        'source_identity': source_identity(ROOT),
        'engine_source': dict(lock, patch_sha256=digest(ROOT / lock['patch']),
                              techmo_input_compatibility_sha256=digest(ROOT / 'engine/techmo-compat.patch'),
                              native_base=command('git', '-C', str(native), 'rev-parse', 'HEAD')),
        'target': 'Windows x64', 'configuration': 'RelWithDebInfo',
        'compiler': command('clang', '--version'),
        'packages': {name: command('pkg-config', '--modversion', name) for name in ('sdl3', 'vulkan', 'libavcodec', 'libxxhash')},
        'source_sha256': source_digest.hexdigest(),
        'binaries': {path.name: {'sha256': digest(path), 'size_bytes': path.stat().st_size} for path in binaries},
        'runtime_dlls': {path.name: digest(path) for path in OUT.glob('*.dll') if path.name.lower() in copied},
        'symbols': 'DWARF debug information retained in native executables; keep these exact binaries for symbolization',
    }
    (OUT / 'native-build.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Native build recorded; {len(copied)} toolchain DLLs copied to out/.')


if __name__ == '__main__':
    main()
