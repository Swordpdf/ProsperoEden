#!/usr/bin/env python3
"""Freeze or verify the separate G7 package without changing G6's receipt."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys

from check import fixture

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get('EDEN_PACKAGE_DIR', ROOT / 'dist/headless/PPSA99008')).resolve()
if 'EDEN_PACKAGE_DIR' in os.environ:
    assert APP.is_relative_to(ROOT/'build') and APP.name == 'PPSA99008'
OUT = ROOT / 'build/headless-native'
BASE_REQUIRED = {'eboot.bin', 'core-homebrew.nro', 'sce_module/libc.prx',
            'sce_sys/param.json', 'sce_sys/icon0.png', 'sce_sys/pic0.dds', 'sce_sys/pic1.dds',
            'sce_sys/snd0.at9', 'lapy.elf', 'lapy-manifest.json', 'licenses/Lapy-MIT.txt',
            'self-updater.elf'}
REQUIRED = set(BASE_REQUIRED)
REQUIRED.update(p.relative_to(APP).as_posix() for p in (APP / 'ui').rglob('*') if p.is_file())
RECEIPT = ROOT / 'HEADLESS_CANDIDATE.json'
# The pinned OpenGL SDK release (tools/deps.json): digest of its manifest.sha256.
GL_SDK_MANIFEST_SHA256 = 'f4b91f672be037fbac3f82494f1225deaf4c227a03f37ac3ffa56abb213b943f'
LAPY_PROTOCOL_SHA256 = 'bb02c4aa814eaba7a7a423a31b29ff41f786212c2953678cf29434e85fa0f869'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check():
    frontend = json.loads((OUT / 'frontend.json').read_text())
    assert frontend.get('gpu_probe', False) == (b'EDEN_GPU_PROBE_PASS cases=36' in (OUT / 'llvm-pie.elf').read_bytes()), 'GPU probe receipt/binary mismatch'
    if frontend['guest_fixture'] == 'retail-game':
        assets = json.loads((APP / 'game-assets.json').read_text())
        assert 'assets/keys/prod.keys' in assets
        for name, expected in assets.items():
            assert name.startswith('assets/')
            assert not Path(name).is_absolute() and '..' not in Path(name).parts
            assert digest(APP / name) == expected, name
        REQUIRED.update(assets)
        REQUIRED.add('game-assets.json')
    assert {p.relative_to(APP).as_posix() for p in APP.rglob('*') if p.is_file()} == REQUIRED
    assert all(not (APP / p).is_symlink() and (APP / p).stat().st_size for p in REQUIRED)
    param = json.loads((APP / 'sce_sys/param.json').read_text())
    assert param['titleId'] == 'PPSA99008' and param['contentId'].endswith('PROSPEROEDEN0001')
    assert param['localizedParameters']['en-US']['titleName'] == 'ProsperoEden'
    assert param['pubtools']['loudnessSnd0'] == '-28.00'
    lapy = json.loads((APP / 'lapy-manifest.json').read_text())
    assert lapy['schema'] == 'lapy-owned-build/1'
    assert lapy['target_title'] == 'PPSA99008' and lapy['mode'] == 'elf-helper'
    assert lapy['max_requests'] == 1 and not lapy['service']
    assert lapy['features'] == ['root_layout_probe_retry']
    assert lapy['protocol_sha256'] == LAPY_PROTOCOL_SHA256
    assert lapy['elf_sha256'] == digest(APP / 'lapy.elf')
    assert (APP / 'lapy.elf').read_bytes()[:6] == b'\x7fELF\x02\x01'
    sound = (APP / 'sce_sys/snd0.at9').read_bytes()
    assert len(sound) <= 2 * 1024 * 1024 and sound[:4] == b'RIFF' and sound[8:12] == b'WAVE'
    fixture((APP / 'core-homebrew.nro').read_bytes())
    runpy.run_path(str(ROOT / 'tools/load_alignment.py'))['check_load_alignment']((APP / 'eboot.bin').read_bytes())
    imports = (OUT / 'imports.txt').read_text()
    needed = sorted(imports.split('NeededLibraries [', 1)[1].split(']', 1)[0].split())
    # libSceCommonDialog, libSceImeDialog and libSceSysmodule: the PS5 keyboard for a game's text
    # entry (headless/system_keyboard.cpp); RADV loads system modules as well.
    expected = ['libSceAudioOut.sprx', 'libSceCommonDialog.sprx', 'libSceImeDialog.sprx', 'libSceLibcInternal.sprx', 'libSceNet.sprx',
                'libScePad.sprx', 'libSceSysmodule.sprx', 'libSceUserService.sprx', 'libkernel.sprx']
    if json.loads((OUT / 'frontend.json').read_text())['renderer'] == 'opengl-4.6-compatibility':
        assert b'[ps5-batch-summary] config gpu-present=1 multidraw=1 deferred=1 ' in (OUT / 'llvm-pie.elf').read_bytes(), 'SDK batching disabled or missing compiled receipt'
        expected += ['libSceAgc.prx', 'libSceAgcDriver.prx', 'libSceSystemService.sprx',
                     'libSceVideoOut.sprx']
    if frontend.get('gpu_probe'):
        expected.remove('libSceNet.sprx')  # Guest networking is dead-stripped from this entry.
    assert needed == sorted(expected), needed
    for symbol in imports.split('Symbol {')[1:]:
        assert not ('Binding: Weak' in symbol and 'Section: Undefined' in symbol), symbol
    assert 'integrity: valid' in (OUT / 'fself-inspection.txt').read_text()
    return needed


if __name__ == '__main__':
    needed = check()
    if sys.argv[1:] == ['--freeze']:
        paths = [APP / p for p in sorted(REQUIRED)]
        paths += [OUT / p for p in ('llvm-pie.elf', 'eboot.elf', 'imports.txt', 'fself-inspection.txt',
                                   'link.map', 'CMakeCache.txt', 'frontend.json')]
        paths += [ROOT / p for p in ('build/host/ps5-native-tool', 'UPSTREAM.json')]
        paths += sorted(p for folder in ('headless', 'tools', 'src', 'fixtures')
                        for p in (ROOT / folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
        paths += [ROOT / p for p in ('tools/load_alignment.py', 'third_party/ps5_pad.hpp',
            'third_party/native_audio.hpp', 'third_party/app_heap.c',
            '../ps5-opengl/tools/Assert-Ps5ForegroundIdle.ps1',
            '../../docs/ps5-homebrew-dev-protocol/scripts/send-controller.sh',
            '../../docs/ps5-homebrew-dev-protocol/scripts/controllers/launch.c',
            '../../docs/ps5-homebrew-dev-protocol/scripts/controllers/close.c')
            if not p.startswith('../') or (ROOT / p).exists()]  # console tooling of a development layout
        frontend = json.loads((OUT / 'frontend.json').read_text())
        if frontend['renderer'] == 'opengl-4.6-compatibility':
            sdk = ROOT / '.deps/ps5-opengl-sdk-1.0.0/sdk'
            manifest = sdk / 'manifest.sha256'
            assert digest(manifest) == GL_SDK_MANIFEST_SHA256
            paths.append(manifest)
            for line in manifest.read_text().splitlines():
                expected, name = line.split('  ', 1)
                assert digest(sdk / name) == expected, name
                paths.append(sdk / name)
        if frontend.get('vulkan'):
            if frontend.get('vulkan_driver', 'CUSTOM') == 'RADV':
                paths += [ROOT / 'build/radv-isolated' / name for name in
                          ('libvulkan_radeon.ps5.a', 'manifest.json', 'symbols.map')]
                paths += [ROOT / '../mihawk-vulkan-review/.deps/native/radv-release' / name
                          for name in ('PROVENANCE.txt', 'EDEN_WSI_SHA256')]
                paths += [ROOT / '../mihawk-vulkan-review/.deps/native/ps5-payload-sdk/target/lib/libps5platform.a']
            else:
                paths += [ROOT / 'build/vulkan-isolated' / name for name in
                          ('libps5vk.a', 'libpsbc.a', 'manifest.json')]
            paths.append(ROOT / 'build/stubs/libSceAgcDriver.so')
        legacy_receipt = ROOT / 'CANDIDATE.json'
        old = json.loads(legacy_receipt.read_text()) if legacy_receipt.exists() else {'files': {}}
        previous = {p: old['files']['dist/PPSA99121/' + p]
                    for p in BASE_REQUIRED - {'core-homebrew.nro'}
                    if 'dist/PPSA99121/' + p in old['files']}
        if RECEIPT.exists():
            prior = json.loads(RECEIPT.read_text())
            previous = ({p: prior['files']['dist/headless/PPSA99008/' + p] for p in prior['package_sizes']}
                        if prior['hardware_run'] else prior['previous_package'])
        retail_launch = frontend['guest_fixture'] == 'retail-game' or bool(frontend.get('development_rom_id'))
        value = dict(gate='G7', title_id='PPSA99008', firmware='6.02', hardware_run=False,
                     classification='offline-validated', needed_libraries=needed,
                     data_heap_bytes=3072 * 1024**2, guest_backing_bytes=4 * 1024**3,
                     observation_seconds=120 if retail_launch else 60,
                     guest_sessions=1 if retail_launch else 3,
                     frontend=json.loads((OUT / 'frontend.json').read_text()),
                     files={p.relative_to(ROOT).as_posix(): digest(p) for p in paths},
                     package_sizes={p: (APP / p).stat().st_size for p in sorted(REQUIRED)},
                     previous_package=previous,
                     tools={p: subprocess.check_output([p, '--version'], text=True).splitlines()[0]
                            for p in ('clang++-18', 'llvm-readobj-18', 'cmake', 'ninja')})
        if frontend.get('gpu_probe'):
            value.update(observation_seconds=120, guest_sessions=0)
        RECEIPT.write_text(json.dumps(value, indent=2) + '\n')
    elif sys.argv[1:] == ['--check']:
        print('Native package inventory, fixture, alignment and imports PASS')
        sys.exit(0)
    else:
        assert sys.argv[1:] == ['--verify']
        value = json.loads(RECEIPT.read_text())
        assert value['needed_libraries'] == needed
        for name, expected in value['files'].items():
            assert digest(ROOT / name) == expected, name
    print('G7 native package inventory, fixture, alignment, imports and hashes PASS')
