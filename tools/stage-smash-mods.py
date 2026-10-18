#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage Skyline, ARCropolis and character packs for ProsperoEden, without changing an install."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import tempfile
import zipfile

TITLE = '01006A800016E000'
CONTENT = f'atmosphere/contents/{TITLE}/'
FRAMEWORK = f'mods/{TITLE}/Skyline'


def archive_files(path):
    """Read regular files only; reject ambiguous or escaping archive names before staging."""
    files = {}
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            name = entry.filename
            parts = PurePosixPath(name).parts
            if (not parts or name.startswith('/') or '\\' in name or ':' in name
                    or '..' in parts or stat.S_ISLNK(entry.external_attr >> 16)):
                raise ValueError(f'Unsafe archive entry: {name!r}')
            if entry.is_dir():
                continue
            mode = stat.S_IFMT(entry.external_attr >> 16)
            if mode not in (0, stat.S_IFREG):
                raise ValueError(f'Not a regular file: {name!r}')
            normalized = PurePosixPath(name).as_posix()
            if normalized in files:
                raise ValueError(f'Duplicate archive entry: {name!r}')
            files[normalized] = archive.read(entry)  # zipfile checks each entry's CRC
    return files


def stage(skyline, arcropolis, characters, output, plugins=()):
    if output.exists() or output.is_symlink():
        raise ValueError(f'Output already exists; choose a new staging directory: {output}')
    sky = archive_files(skyline)
    arc = archive_files(arcropolis)
    if not sky.get('exefs/subsdk9', b'').startswith(b'NSO0'):
        raise ValueError('Skyline ZIP must contain exefs/subsdk9 (NSO0)')
    if not sky.get('exefs/main.npdm', b'').startswith(b'META'):
        raise ValueError('Skyline ZIP must contain the Smash exefs/main.npdm (META)')
    plugin = CONTENT + 'romfs/skyline/plugins/libarcropolis.nro'
    if arc.get(plugin, b'')[0x10:0x14] != b'NRO0':
        raise ValueError('ARCropolis ZIP must contain its Smash libarcropolis.nro (NRO0)')
    # Releases such as v4.0.8 embed menu resources and ship only the plugin NRO.
    planned = {}

    def add(name, data):
        # A case-insensitive collision would depend on the target filesystem's behavior.
        key = name.casefold()
        if key in planned:
            raise ValueError(f'Conflicting destination: {name}')
        planned[key] = (name, data)

    for name, data in sky.items():
        if name.startswith('exefs/'):
            add(f'{FRAMEWORK}/{name}', data)
    for name, data in arc.items():
        if name.startswith(CONTENT + 'romfs/'):
            add(f'{FRAMEWORK}/{name[len(CONTENT):]}', data)
        elif name.startswith('ultimate/'):
            add(f'user/sdmc/{name}', data)
        else:
            raise ValueError(f'Unexpected ARCropolis release entry: {name}')
    for plugin_file in plugins:
        data = plugin_file.read_bytes()
        if plugin_file.suffix.lower() != '.nro' or data[0x10:0x14] != b'NRO0':
            raise ValueError(f'Plugin must be a Skyline NRO: {plugin_file}')
        add(f'{FRAMEWORK}/romfs/skyline/plugins/{plugin_file.name}', data)
    sources = [skyline, arcropolis]
    for character in characters:
        # Accept one extracted character pack, not a console SD-card distribution.
        if not character.is_dir() or character.is_symlink():
            raise ValueError(f'Character must be an extracted pack directory: {character}')
        name = character.name
        if name in ('.', '..') or ':' in name or '\\' in name:
            raise ValueError(f'Invalid character pack name: {name}')
        count = 0
        for file in sorted(character.rglob('*')):
            if file.is_symlink():
                raise ValueError(f'Character pack contains a symbolic link: {file}')
            if file.is_dir():
                continue
            if not file.is_file():
                raise ValueError(f'Character pack contains a special file: {file}')
            relative = file.relative_to(character).as_posix()
            if ':' in relative or '\\' in relative:
                raise ValueError(f'Invalid character path: {relative}')
            add(f'user/sdmc/ultimate/mods/{name}/{relative}', file.read_bytes())
            count += 1
        if not count:
            raise ValueError(f'Empty character pack: {character}')
    manifest = {
        'status': 'staged; gameplay compatibility requires console validation',
        'title_id': TITLE,
        'archives': [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                     for p in sources],
        'plugins': [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in plugins],
        'files': [{'path': name, 'sha256': hashlib.sha256(data).hexdigest()}
                  for name, data in sorted(planned.values())],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.smash-stage-', dir=output.parent) as temporary:
        root = Path(temporary) / 'data'
        root.mkdir()
        for name, data in planned.values():
            destination = root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        (root / 'smash-mods-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        # Refuse existing destinations again; never merge into or replace a user's installation.
        if output.exists() or output.is_symlink():
            raise ValueError(f'Output appeared during staging: {output}')
        root.rename(output)
    return len(planned)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skyline', required=True, type=Path, help='official skyline.zip')
    parser.add_argument('--arcropolis', required=True, type=Path, help='official ARCropolis release.zip')
    parser.add_argument('--character', action='append', type=Path, default=[],
                        help='extracted character pack root; repeat for more packs')
    parser.add_argument('--plugin', action='append', type=Path, default=[],
                        help='additional Skyline .nro plugin required by a pack; repeat as needed')
    parser.add_argument('--output', required=True, type=Path, help='new staging directory')
    args = parser.parse_args()
    try:
        count = stage(args.skyline, args.arcropolis, args.character, args.output, args.plugin)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Staging failed: {error}\n')
    print(f'Staged {count} files in {args.output}. Gameplay compatibility is unverified.')


if __name__ == '__main__':
    main()
