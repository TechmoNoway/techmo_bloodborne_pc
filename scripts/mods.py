#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile

# Windows: symbolic links need administrator rights or developer mode. Directories become
# junctions and files hard links instead (a copy when the mod is on another volume).
WINDOWS = os.name == 'nt'

# Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself.
GAME_FOLDERS = {'action', 'chr', 'event', 'facegen', 'font', 'map', 'menu', 'movie', 'msg', 'mtd',
                'obj', 'other', 'param', 'paramdef', 'parts', 'remo', 'script', 'sfx', 'shader',
                'sound'}


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names)."""
    folder = Path(folder)
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


def content_root(folder, depth=0):
    """(root, prefix): where a mod's files are and the game path they go to, or None.

    Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/CUSA03173/dvdroot_ps4,
    one wrapper folder around any of these (an archive extracted into a folder of its name), and
    the game's folders without dvdroot_ps4 (<mod>/chr, <mod>/parts, ...)."""
    folder = Path(folder)
    if is_link(folder):
        raise ValueError(f'Mod links are unsupported: {folder}')
    if depth > 16:
        raise ValueError('Mod layout is nested too deeply')
    if not folder.is_dir():
        return None
    for wrapper in ('', 'app0', 'CUSA03173'):
        base = child(folder, wrapper) if wrapper else folder
        dvdroot = child(base, 'dvdroot_ps4')
        if is_link(base) or is_link(dvdroot):
            raise ValueError(f'Mod links are unsupported: {base}')
        if dvdroot.is_dir():
            return base, ''
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if folders and all(e.name.casefold() in GAME_FOLDERS for e in folders):
        return folder, 'dvdroot_ps4'
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0], depth + 1)
    return None


def discover(root):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    found = []
    for entry in root.iterdir():
        if not entry.is_dir() and not is_link(entry):
            continue
        try:
            recognized = content_root(entry) is not None
        except (OSError, ValueError):
            # Keep invalid entries selectable so a disabled bad mod cannot prevent
            # unrelated mods from loading. Active entries still fail in mod_files.
            recognized = True
        if recognized:
            found.append(entry.name)
    return sorted(found, key=lambda name: (name.casefold(), name))


def load_config(config):
    if not config or not Path(config).is_file():
        return {}
    if Path(config).stat().st_size > 1024 * 1024:
        raise ValueError('Mod configuration is too large')
    settings = json.loads(Path(config).read_text(encoding='utf-8-sig'))
    if not isinstance(settings, dict):
        raise ValueError('Mod configuration must be an object')
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    for key in ('legacy_enabled', 'direct_enabled'):
        if key in settings and not isinstance(settings[key], bool):
            raise ValueError(f'{key} must be a boolean')
    return settings


def selected(root, config):
    available = discover(root)
    settings = load_config(config)
    disabled = set(settings.get('disabled', []))
    order = settings.get('order', [])
    # New folders are enabled automatically and appended in alphabetical order.
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder):
    """(game path, source) of every file a mod replaces or adds under dvdroot_ps4."""
    layout = content_root(folder)
    if not layout:
        raise ValueError(f'{folder}: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder')
    root, prefix = layout
    root = root.resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in [*folders, *files]:
            if is_link(directory / name):
                raise ValueError(f'Mod links are unsupported: {directory / name}')
        for name in sorted(files):
            source = directory / name
            relative = Path(prefix) / source.relative_to(root) if prefix else source.relative_to(root)
            # Readme/metadata stay outside the mounted game. This loader handles assets;
            # executable patches use the existing patch compiler, with address validation.
            if relative.parts[0].casefold() != 'dvdroot_ps4':
                if relative.parts[0].casefold() in ('eboot.bin', 'sce_module', 'sce_sys'):
                    raise ValueError(f'Executable/system replacement is unsupported: {source}')
                continue
            if not source.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative, source


def is_link(path):
    return path.is_symlink() or (WINDOWS and path.is_junction())


def inspect_mods(game, root):
    """Read-only catalog for the launcher, using the runtime's layout and file rules."""
    root = Path(root)
    if is_link(root):
        raise ValueError(f'Mod links are unsupported: {root}')
    rows = []
    total_files = 0

    def inspect(folder, kind, name=None):
        nonlocal total_files
        row = dict(name=name or folder.name, path=str(folder.resolve()), kind=kind,
                   files=[], error='', detail='')
        try:
            if not content_root(folder):
                row['error'] = 'layout'
            else:
                files = []
                for relative, _ in mod_files(folder):
                    total_files += 1
                    if total_files > 200_000:
                        raise ValueError('Mod catalog exceeds 200000 files')
                    files.append(relative.as_posix())
                row['files'] = files
                if not files:
                    row['error'] = 'empty'
        except (OSError, ValueError) as error:
            row.update(error='invalid', detail=str(error))
        rows.append(row)

    legacy = Path(str(Path(game).resolve()) + '-mods')
    if legacy.exists():
        inspect(legacy, 'legacy')
    direct = child(root, 'dvdroot_ps4').is_dir()
    if direct:
        inspect(root, 'direct', 'mods/dvdroot_ps4')
    elif root.is_dir():
        entries = sorted(root.iterdir(), key=lambda entry: (entry.name.casefold(), entry.name))
        if len(entries) > 10_000:
            raise ValueError('Mod catalog exceeds 10000 entries')
        for entry in entries:
            if entry.name.startswith('.') and not entry.is_dir():
                continue
            if entry.is_dir() or is_link(entry):
                inspect(entry, 'named')
            elif entry.suffix.casefold() in ('.zip', '.rar', '.7z', '.dll', '.exe', '.pkg', '.xml'):
                rows.append(dict(name=entry.name, path=str(entry.resolve()), kind='unsupported',
                                 files=[], error='archive' if entry.suffix.casefold() in ('.zip', '.rar', '.7z')
                                 else 'type', detail=''))
    return dict(mods=rows, direct=direct)


def link(path, target):
    """`path` refers to `target` (a directory or a file) without copying the game."""
    if not WINDOWS:
        path.symlink_to(target, target_is_directory=target.is_dir())
    elif target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(path))
    else:
        try:
            os.link(target, path)
        except OSError:
            shutil.copy2(target, path)


def unlink(path):
    """Removes a link made by `link` (never what it refers to)."""
    if WINDOWS and path.is_junction():
        os.rmdir(path)
        return
    try:
        path.unlink()
    except PermissionError:
        if not WINDOWS or is_link(path):
            raise
        os.chmod(path, stat.S_IWRITE)  # a read-only copy (hard links share the attribute)
        path.unlink()


def remove_overlay(path):
    """Deletes an overlay from build_overlay without following its links."""
    path = Path(path)
    if is_link(path) or not path.is_dir():
        if is_link(path) or path.exists():
            unlink(path)
        return
    for entry in path.iterdir():
        remove_overlay(entry)
    os.rmdir(path)


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if is_link(directory):
        target = directory.resolve(strict=True)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        unlink(directory)
        directory.mkdir()
        for entry in target.iterdir():
            link(directory / entry.name, entry)
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def build_overlay(game, out, mods):
    game = Path(game).resolve(strict=True)
    replacements = []
    owners = {}
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root):
            key = relative.as_posix().casefold()
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    if not replacements:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    overlay = Path(tempfile.mkdtemp(prefix='mod-game-', dir=out))
    try:
        for entry in game.iterdir():
            link(overlay / entry.name, entry)
        replaced = added = 0
        for relative, source in replacements:
            # Each component takes the game's spelling when it exists in another case.
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if destination.is_symlink():
                replaced += destination.resolve().is_relative_to(game)
                destination.unlink()
            elif WINDOWS and destination.is_file():
                # A hard link or copy: every file in the overlay is one of ours.
                replaced += 1
                unlink(destination)
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            link(destination, source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        return overlay
    except BaseException:
        remove_overlay(overlay)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    parser.add_argument('--inspect', action='store_true', help='print a read-only JSON catalog')
    args = parser.parse_args()
    if args.inspect:
        print(json.dumps(inspect_mods(args.game, args.mods_dir)))
        return
    if args.out is None:
        parser.error('--out is required when loading mods')
    layers = []
    if args.enabled == '1':
        if is_link(args.mods_dir):
            raise ValueError(f'Mod links are unsupported: {args.mods_dir}')
        settings = load_config(args.config)
        # shadPS4's loose overlay convention, beside the original game.
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir() and settings.get('legacy_enabled', True):
            layers.append((legacy.name, legacy))
        # A directly selected loose overlay is also accepted.
        if child(args.mods_dir, 'dvdroot_ps4').is_dir():
            if settings.get('direct_enabled', True):
                layers.append((args.mods_dir.name, args.mods_dir))
        else:
            for name in selected(args.mods_dir, args.config):
                layers.append((name, args.mods_dir / name))
    print(build_overlay(args.game, args.out, layers))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)
