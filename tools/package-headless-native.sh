#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ $# == 0 || ( $# == 1 && ( "$1" == --integration || "$1" == --game ) ) ]]
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(cat "$root/.local/headless-cache")
[[ "$(cat "$scratch/owner")" == "$root" ]]
template="$root/../ps5-native-app-boilerplate"
builder="$root/build/host/ps5-native-tool"
out="$root/build/headless-native"
app="${EDEN_PACKAGE_DIR:-$root/dist/headless/PPSA99008}"
# A separate clean staging directory lets driver candidates omit private game
# assets without deleting or copying the existing development installation.
if [[ -n ${EDEN_PACKAGE_DIR:-} ]]; then
    [[ $(realpath -m "$app") == "$(realpath -m "$root")"/build/*/PPSA99008 ]] || exit 2
    [[ ${1:-} != --game ]] || exit 2
fi
test -x "$builder"
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
mkdir -p "$out" "$app/sce_sys" "$app/sce_module"
cp "$scratch/native-local/bin/eden-headless" "$out/llvm-pie.elf"
cp "$scratch/native-local/bin/eden-headless.map" "$out/link.map"
cp "$scratch/native-local/CMakeCache.txt" "$out/CMakeCache.txt"
stub_flags=(--stub-dir "$scratch/sdk/target/lib")
if grep -qx 'EDEN_PS5_OPENGL:BOOL=ON' "$out/CMakeCache.txt"; then
    gl46="$root/.deps/ps5-opengl-sdk-1.0.2/sdk"
    stub_flags=()
    for stub in "$scratch/sdk/target/lib/"*.so; do
        case "${stub##*/}" in libSceAgc.so|libSceAgcDriver.so) continue ;; esac
        stub_flags+=(--stub "$stub")
    done
    driver_stub="$gl46/lib/libSceAgcDriver.so"
    if grep -qx 'EDEN_PS5_VULKAN:BOOL=ON' "$out/CMakeCache.txt"; then
        driver_stub="$root/build/stubs/libSceAgcDriver.so"
    fi
    stub_flags+=(--stub "$gl46/lib/libSceAgc.so" --stub "$driver_stub")
fi
# The PS5 keyboard's libSceCommonDialog import (tools/build-common-dialog-stub.sh).
stub_flags+=(--stub "$root/build/stubs/libSceCommonDialog.so")
"$builder" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" \
    "${stub_flags[@]}" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$builder" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$template/runtime/libc.prx" "$app/sce_module/libc.prx"
rm -f "$app/sandbox-elevator.elf"
cp "$root/build/lapy-owned-helper/lapy.elf" "$app/lapy.elf"
cp "$root/build/lapy-owned-helper/lapy-manifest.json" "$app/lapy-manifest.json"
mkdir -p "$app/licenses"
cp "$root/build/lapy-owned-helper/LICENSE.Lapy" "$app/licenses/Lapy-MIT.txt"
# What the console's home screen shows for the title is in sce_sys/, as it goes into the package
# (the source pictures beside it stay in the repository).
# The self-update helper (headless/self_update_helper, the boilerplate's) is an ordinary payload:
# the app sends it to the console's payload loader to replace the app's files once it has closed.
# It does file work with the rights every payload has and never touches the kernel.
make -s -C "$root/headless/self_update_helper" PS5_PAYLOAD_SDK="$template/.deps/native/ps5-payload-sdk" \
    OUTPUT="$root/build/self-update/self-updater.elf"
python3 "$root/tools/validate-loader-elf.py" "$root/build/self-update/self-updater.elf"
cp "$root/build/self-update/self-updater.elf" "$app/self-updater.elf"
cp "$root/sce_sys/"{param.json,icon0.png,pic0.dds,pic1.dds,snd0.at9} "$app/sce_sys/"
rm -rf "$app/ui"
cp -a "$root/headless/prosperoeden/ui" "$app/ui"
# The switches the runner reads from the app folder (headless/main.cpp). Only a development
# package gets them: the profile title otherwise takes its timed input replay instead of the
# controller, and a release build never parses the file.
if [[ ${EDEN_DEV_PROFILE:-} == ON && -f "$root/headless/dev-settings.txt" ]]; then
    cp "$root/headless/dev-settings.txt" "$app/dev-settings.txt"
fi
# The exact-title upstream Lapy helper is verified before it reaches this package.
python3 - "$root" "$scratch" "$app" "${1:-}" <<'PY'
import json, pathlib, re, runpy, shutil, sys
root, scratch, app = map(pathlib.Path, sys.argv[1:4])
game = sys.argv[4] == '--game'
integration = sys.argv[4] in ('--integration', '--game')
if game:
    assert (app / 'game-assets.json').is_file()
    assert (app / 'assets/keys/prod.keys').is_file()
value = json.loads((app / 'sce_sys/param.json').read_text())
assert (value['titleId'], value['conceptId']) == ('PPSA99008', '99008')
assert value['contentId'] == 'UP9000-PPSA99008_00-PROSPEROEDEN0001'
assert value['localizedParameters']['en-US']['titleName'] == 'ProsperoEden'
assert value['pubtools']['loudnessSnd0'] == '-28.00'
# The app version has one home, sce_sys/param.json: the program was built with the same value.
assert re.fullmatch(r'\d{2}\.\d{3}\.\d{3}', value['contentVersion']), value['contentVersion']
built = re.search(r'EDEN_APP_VERSION "([0-9.]+)"',
    (scratch / 'native-local/headless/version/app_version.h').read_text()).group(1)
assert built == value['contentVersion'], f'the program says {built}, param.json {value["contentVersion"]}'
# The console gives a 120 Hz output (Settings > Video, headless/display_refresh.h) only to a title
# that declares it. Declaring it changes nothing by itself: the output stays at 60 Hz until asked.
# With it goes bit 18, a variable refresh rate as system software 6.02 already knows it: bit 19
# (the newer way to say so) is refused there as an unknown parameter, and the title is then
# listed without a variable refresh rate at all.
assert int(value['attribute3']) & 0x40040 == 0x40040, 'param.json no longer declares the 120 Hz output'
# Bit 22: PS5 Pro memory mode, a 13,504 MiB direct memory pool instead of 12,288 MiB on a Pro.
assert int(value['attribute3']) & 0x400000, 'param.json no longer asks for PS5 Pro memory mode'
# The flexible memory pool: what it is smaller than the system's 448 MiB goes to the direct pool,
# which is the one games run out of. A game session was measured to use 203 MiB of it.
assert value['kernel']['flexibleMemorySize'] == 320 << 20, 'param.json no longer sets the flexible memory pool'
runpy.run_path(str(root / 'tools/load_alignment.py'))['check_load_alignment']((app / 'eboot.bin').read_bytes())
profile = (root / 'build/headless-native/CMakeCache.txt').read_text()
assert sum(profile.count('EDEN_DEVICE_FRONTEND:BOOL=' + v) for v in ('ON', 'OFF')) == 1
devices = 'EDEN_DEVICE_FRONTEND:BOOL=ON' in profile
graphics = 'EDEN_PS5_OPENGL:BOOL=ON' in profile
assert not (integration and devices)
assert not game or graphics
fixture_name = 'core-integration.nro' if integration else 'core-devices.nro' if devices else 'core-homebrew.nro'
shutil.copy2(root / 'build/fixture' / fixture_name, app / 'core-homebrew.nro')
(root / 'build/headless-native/frontend.json').write_text(json.dumps({
    'devices': devices,
    'gpu_probe': 'EDEN_GPU_PROBE:BOOL=ON' in profile,
    'renderer': 'opengl-4.6-compatibility' if graphics else 'null', 'guest_fixture': 'retail-game' if game else 'cpu-state-memory-atomics-fp-simd-services' if integration else 'timer-sync-service-storage-hid-audio-renderer-voice-mix-src-high' if devices else 'timer-sync-service-storage',
    'vulkan': 'EDEN_PS5_VULKAN:BOOL=ON' in profile,
    'vulkan_driver': 'RADV' if 'EDEN_VULKAN_DRIVER:STRING=RADV' in profile else 'CUSTOM',
    'development_backend': 'Vulkan' if 'EDEN_DEV_VULKAN:BOOL=ON' in profile else 'user-preference',
    'development_rom_id': next((line.split('=', 1)[1] for line in profile.splitlines()
                                if line.startswith('EDEN_DEV_ROM_ID:STRING=')), ''),
    'hardware_qualified': False,
}, indent=2) + '\n')
PY
cp "$root/launcher-runtime.json" "$app/launcher-runtime.json"
llvm-readobj-18 --dyn-symbols --needed-libs "$out/llvm-pie.elf" > "$out/imports.txt"
"$builder" self --inspect --file "$app/eboot.bin" > "$out/fself-inspection.txt"
sha256sum "$app/eboot.bin" "$app/core-homebrew.nro" "$app/sce_module/libc.prx" \
    "$app/sce_sys/param.json" "$app/sce_sys/icon0.png" "$app/sce_sys/pic0.dds" "$app/sce_sys/pic1.dds" \
    "$app/sce_sys/snd0.at9"
