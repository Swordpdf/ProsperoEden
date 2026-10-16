#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a launcher update ZIP and HTTPS feed; does not upload or publish anything."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
from urllib.parse import urlsplit
import zipfile


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}', value):
        raise ValueError('Version must have three numeric components')
    return tuple(map(int, value.split('.')))


def https(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or parsed.fragment or any(ord(c) <= 32 or c == '\\' for c in value):
        raise ValueError('A public HTTPS URL without credentials is required')
    return value


def package(folder, output, url, previous=None, notes='', min_app=None):
    if output.resolve().is_relative_to(folder.resolve()):
        raise ValueError('Output must be outside the launcher folder')
    if folder.is_symlink():
        raise ValueError('Pack root must not be a symbolic link')
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
    identifier = manifest['id']
    if not re.fullmatch(r'[a-z0-9-]{1,48}', identifier) or identifier == 'builtin' or manifest['schema'] != 1:
        raise ValueError('Invalid launcher ID or manifest schema')
    version(manifest['version']); https(manifest['updates']['feed']); https(url)
    if manifest['host_contract'] != 'launcher-v1.0' or manifest['api']['major'] != 1:
        raise ValueError('Unsupported launcher runtime')
    if len(notes.encode()) > 8192:
        raise ValueError('Release notes exceed 8192 UTF-8 bytes')
    if min_app:
        version(min_app)
    files = []
    total = 0
    for path in sorted(folder.rglob('*')):
        relative = path.relative_to(folder).as_posix()
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode) or not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
            raise ValueError(f'Links and special files are forbidden: {relative}')
        if len((identifier + '/' + relative).encode()) > 240 or len(path.relative_to(folder).parts) > 20 or any(':' in part or any(ord(c) < 32 or ord(c) == 127 or c == '\\' for c in part) for part in path.relative_to(folder).parts):
            raise ValueError(f'Invalid resource path: {relative}')
        if path.is_file():
            size = path.stat().st_size
            if size > 64 * 1024**2:
                raise ValueError(f'Resource exceeds 64 MiB: {relative}')
            total += size
            files.append((path, relative))
    if not files or len(files) > 4096 or total > 256 * 1024**2:
        raise ValueError('Pack exceeds file count or expanded size limits')
    names = {relative for _, relative in files}
    for relative in [manifest['entry'], manifest['font'], *manifest['assets'].values()]:
        if relative not in names:
            raise ValueError(f'Missing resource: {relative}')
    # Deliberately emit only publisher-owned files; never add app data or UI state.
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"{identifier}-{manifest['version']}.zip"
    archive_tmp = archive.with_suffix('.zip.tmp')
    try:
        with zipfile.ZipFile(archive_tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
            for path, relative in files:
                zipped.write(path, f'{identifier}/{relative}')
        if archive_tmp.stat().st_size > 128 * 1024**2:
            raise ValueError('ZIP exceeds 128 MiB')
        archive_tmp.replace(archive)
    finally:
        archive_tmp.unlink(missing_ok=True)
    release = {key: manifest[key] for key in ['version', 'host_contract', 'api']}
    release.update(required_capabilities=manifest.get('required_capabilities', []), state_schema=manifest.get('state_schema', 1),
                   url=url, sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), bytes=archive.stat().st_size, notes=notes)
    if min_app:
        release['min_app'] = min_app
    feed = {'schema': 1, 'id': identifier, 'releases': []}
    if previous:
        feed = json.loads(previous.read_text(encoding='utf-8'))
        if feed['schema'] != 1 or feed['id'] != identifier:
            raise ValueError('Previous feed belongs to a different launcher')
    feed['releases'] = [item for item in feed['releases'] if version(item['version']) != version(manifest['version'])]
    feed['releases'].append(release)
    feed['releases'].sort(key=lambda item: version(item['version']), reverse=True)
    if len(feed['releases']) > 64:
        raise ValueError('Feed exceeds 64 releases; remove obsolete entries explicitly')
    encoded = json.dumps(feed, indent=2, ensure_ascii=False) + '\n'
    if len(encoded.encode()) > 256 * 1024:
        raise ValueError('Feed exceeds 256 KiB')
    (output / 'launcher-updates.json').write_text(encoded, encoding='utf-8')
    return archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--url', required=True, help='Permanent HTTPS URL of this versioned ZIP asset')
    parser.add_argument('--previous', type=Path, help='Previous feed, to retain older compatible releases')
    parser.add_argument('--notes', type=Path, help='UTF-8 release notes')
    parser.add_argument('--min-app', help='Optional minimum ProsperoEden version')
    args = parser.parse_args()
    try:
        print(package(args.pack, args.output, args.url, args.previous,
                      args.notes.read_text(encoding='utf-8') if args.notes else '', args.min_app))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'{error}\n')
