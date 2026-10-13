#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned build inputs of ProsperoEden (tools/deps.json).

  tools/deps.py status          list every input, where it lives and whether it matches its pin
  tools/deps.py fetch [NAME...] fetch what is missing (all inputs, or the named ones)
  tools/deps.py path NAME       print where an input lives
  tools/deps.py clean           remove the fetched inputs inside .deps (sibling checkouts stay)

Nothing that already exists is modified: an existing checkout stays at whatever revision it
has (status reports it), an existing file or folder is not downloaded again. Downloads are
cached in $PROSPEROEDEN_DEPS_CACHE (default ~/.cache/prosperoeden-deps) and verified against
their pinned SHA-256/SHA-512 before use; git inputs are fetched at their pinned commit.
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / 'tools/deps.json').read_text())
CACHE = pathlib.Path(os.environ.get('PROSPEROEDEN_DEPS_CACHE') or
                     pathlib.Path(os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache')
                     / 'prosperoeden-deps')


def fail(message):
    print(f'deps: {message}', file=sys.stderr)
    sys.exit(1)


def items(names=()):
    known = {item['name']: item for item in MANIFEST['items']}
    unknown = [name for name in names if name not in known]
    if unknown:
        fail(f'unknown input(s): {", ".join(unknown)} (known: {", ".join(known)})')
    return [known[name] for name in names] if names else list(known.values())


def location(item):
    key = {'git': 'path', 'files': 'dest'}.get(item['kind'])
    return (ROOT / item[key]).resolve() if key else (ROOT / item['creates']).resolve()


def present(item):
    marker = item.get('creates')
    return (ROOT / marker).exists() if marker else location(item).exists()


def digest(path, algorithm):
    hasher = hashlib.new(algorithm)
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            hasher.update(block)
    return hasher.hexdigest()


def pinned_hash(entry):
    for algorithm in ('sha256', 'sha512'):
        if algorithm in entry:
            return algorithm, entry[algorithm]
    fail(f'no pinned hash for {entry.get("url")}')


def download(url, algorithm, expected, name):
    """The cached, verified download of url (downloaded when missing or corrupt)."""
    CACHE.mkdir(parents=True, exist_ok=True)
    target = CACHE / name
    if target.exists() and digest(target, algorithm) == expected:
        return target
    partial = target.with_name(target.name + f'.partial-{os.getpid()}')
    for attempt in range(1, 4):
        try:
            print(f'deps: downloading {url}', flush=True)
            request = urllib.request.Request(url, headers={'User-Agent': 'prosperoeden-deps'})
            with urllib.request.urlopen(request, timeout=120) as response, open(partial, 'wb') as out:
                shutil.copyfileobj(response, out, 1 << 20)
            break
        except OSError as error:
            partial.unlink(missing_ok=True)
            if attempt == 3:
                fail(f'cannot download {url}: {error}')
            time.sleep(5 * attempt)
    actual = digest(partial, algorithm)
    if actual != expected:
        partial.unlink(missing_ok=True)
        fail(f'{url}: {algorithm} {actual} does not match the pinned {expected}')
    partial.replace(target)
    return target


def extract(archive, destination, strip, only=()):
    """Extracts into destination through a temporary folder beside it (no partial trees).
    `only` keeps the members whose path (after strip) is one of these files or lies in one of
    these folders."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix='.deps-', dir=destination.parent))
    try:
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as zipped:
                for member in zipped.infolist():
                    parts = pathlib.PurePosixPath(member.filename).parts[strip:]
                    if not parts:
                        continue
                    relative = pathlib.PurePosixPath(*parts)
                    if only and not any(str(relative) == keep or
                                        str(relative).startswith(keep.rstrip('/') + '/')
                                        for keep in only):
                        continue
                    target = staging.joinpath(*relative.parts)
                    if member.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with zipped.open(member) as source, open(target, 'wb') as output:
                        shutil.copyfileobj(source, output)
                    mode = member.external_attr >> 16
                    if mode:
                        target.chmod(mode)
        else:
            with tarfile.open(archive) as tar:
                members = []
                for member in tar.getmembers():
                    parts = pathlib.PurePosixPath(member.name).parts[strip:]
                    if not parts:
                        continue
                    member.name = str(pathlib.PurePosixPath(*parts))
                    if only and not any(member.name == keep or
                                        member.name.startswith(keep.rstrip('/') + '/')
                                        for keep in only):
                        continue
                    members.append(member)
                # Python 3.11.0-3.11.3 has no extraction filters; the archive's hash was checked.
                safe = {'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}
                tar.extractall(staging, members=members, **safe)
        destination.mkdir(parents=True, exist_ok=True)
        for entry in staging.iterdir():
            target = destination / entry.name
            if target.exists():
                continue  # never replace what is already there
            entry.rename(target)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def git(*args, cwd=None):
    subprocess.run(['git', *args], cwd=cwd, check=True)


def fetch_git(item):
    path = location(item)
    if not path.exists():
        print(f'deps: fetching {item["url"]} at {item["commit"][:12]} into {path}', flush=True)
        staging = path.with_name(path.name + f'.partial-{os.getpid()}')
        shutil.rmtree(staging, ignore_errors=True)
        git('init', '-q', str(staging))
        git('remote', 'add', 'origin', item['url'], cwd=staging)
        git('fetch', '-q', '--depth', '1', 'origin', item['commit'], cwd=staging)
        git('-c', 'advice.detachedHead=false', 'checkout', '-q', '--detach', 'FETCH_HEAD', cwd=staging)
        head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=staging, text=True).strip()
        if head != item['commit']:
            fail(f'{item["name"]}: fetched {head}, pinned {item["commit"]}')
        staging.rename(path)
    if 'setup' in item and not (path / item['setup_creates']).exists():
        print(f'deps: setting up {item["name"]}: {" ".join(item["setup"])}', flush=True)
        subprocess.run(item['setup'], cwd=path, check=True)


def fetch(item):
    kind = item['kind']
    if kind == 'git':
        fetch_git(item)
        return
    if present(item):
        return
    if kind == 'archive':
        algorithm, expected = pinned_hash(item)
        name = pathlib.PurePosixPath(item.get('save') or f'{item["name"]}-{expected[:12]}.tar.gz').name
        archive = download(item['url'], algorithm, expected, name)
        if 'save' in item:
            saved = ROOT / item['save']
            if not saved.exists():
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(archive, saved)
        extract(archive, ROOT / item['extract'], item.get('strip', 0), item.get('only', ()))
    elif kind == 'files':
        destination = ROOT / item['dest']
        destination.mkdir(parents=True, exist_ok=True)
        for entry in item['files']:
            target = destination / entry['name']
            if target.exists():
                continue
            algorithm, expected = pinned_hash(entry)
            # A file may sit in a folder of its input (src/ui/theme.cpp).
            target.parent.mkdir(parents=True, exist_ok=True)
            cached = f'{item["name"]}-{entry["name"].replace("/", "-")}'
            shutil.copyfile(download(entry['url'], algorithm, expected, cached), target)
    else:
        fail(f'{item["name"]}: unknown kind {kind}')
    if not present(item):
        fail(f'{item["name"]}: fetched, but {item["creates"]} is still missing')


def revision(path):
    try:
        return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=path, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, FileNotFoundError, NotADirectoryError):
        return None


def status():
    width = max(len(item['name']) for item in MANIFEST['items'])
    mismatched = 0
    for item in MANIFEST['items']:
        where = os.path.relpath(location(item), ROOT)
        if item['kind'] == 'git':
            path = location(item)
            if not path.exists():
                state = 'missing'
            else:
                head = revision(path)
                if head == item['commit']:
                    state = 'ok (pinned commit)'
                else:
                    mismatched += 1
                    state = f'present at {head[:12] if head else "no git checkout"}, pinned {item["commit"][:12]}'
                if 'setup' in item and not (path / item['setup_creates']).exists():
                    state += '; setup pending'
        else:
            state = 'ok' if present(item) else 'missing'
        print(f'{item["name"]:<{width}}  {state:<40}  {where}')
    if mismatched:
        print(f'\n{mismatched} checkout(s) differ from their pin; the build uses them as they are.')


def clean():
    """Removes the fetched inputs inside this repository's .deps; sibling checkouts stay."""
    deps = (ROOT / '.deps').resolve()
    for item in MANIFEST['items']:
        targets = []
        if item['kind'] == 'files':
            targets.append(ROOT / item['dest'])
        elif item['kind'] == 'archive':
            if 'save' in item:
                targets.append(ROOT / item['save'])
            extract = pathlib.PurePosixPath(item['extract'])
            if extract == pathlib.PurePosixPath('.deps'):
                top = pathlib.PurePosixPath(item['creates']).relative_to(extract).parts[0]
                targets.append(ROOT / extract / top)
            else:
                targets.append(ROOT / extract)
        for target in targets:
            target = target.resolve()
            if deps not in target.parents or not target.exists():
                continue
            print(f'deps: removing {os.path.relpath(target, ROOT)}')
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__.strip())
        return
    command, names = argv[0], argv[1:]
    if command == 'status':
        status()
    elif command == 'clean':
        clean()
    elif command == 'fetch':
        for item in items(names):
            fetch(item)
    elif command == 'path':
        if len(names) != 1:
            fail('usage: tools/deps.py path NAME')
        print(location(items(names)[0]))
    else:
        fail(f'unknown command {command} (status, fetch, path)')


if __name__ == '__main__':
    main(sys.argv[1:])
