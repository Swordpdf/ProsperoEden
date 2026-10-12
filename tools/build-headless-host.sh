#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p .local build/headless-host build/fixture
bash tools/build-core-fixture.sh
# Thousands of dependency files and compiler checks are slow across /mnt/c.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    mkdir -p "$cache_parent"
    scratch=$(mktemp -d "$cache_parent/ps5-eden-headless.XXXXXX")
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$scratch" == "$cache_parent"/ps5-eden-headless.* && "$(cat "$scratch/owner")" == "$root" ]]
eden="$scratch/source"
# As in prepare-build.sh: a tree unpacked from another archive is another Eden revision.
if [[ ! -f "$eden/CMakeLists.txt" || $(cat "$eden/.unpacked-from" 2>/dev/null) != eden-67bada77.tar.gz ]]; then
    python3 - <<'PY'
import hashlib, json, pathlib
p = pathlib.Path('.deps/eden-67bada77.tar.gz')
assert hashlib.sha256(p.read_bytes()).hexdigest() == json.loads(pathlib.Path('UPSTREAM.json').read_text())['archives'][p.name]
PY
    if [[ -d $eden/.cache ]]; then rm -rf "$scratch/source-cache" && mv "$eden/.cache" "$scratch/source-cache"; fi
    rm -rf "$eden"
    mkdir -p "$eden"
    tar -xzf .deps/eden-67bada77.tar.gz --strip-components=1 -C "$eden"
    if [[ -d $scratch/source-cache ]]; then mv "$scratch/source-cache" "$eden/.cache"; fi
    printf '%s\n' eden-67bada77.tar.gz > "$eden/.unpacked-from"
fi
printf '%s\n' '67bada77f8a43a90da2e94e89b8e7da73c256989' > "$eden/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$eden/GIT-REFSPEC"
# FFmpeg links configure probes through the compiler driver, not raw ld.
# Keep the derivative in our cache and regenerate its Makefile when it changes.
python3 - "$eden" "$scratch/build" <<'PY'
import pathlib, sys
source = pathlib.Path('.deps/mirror-67bada77f8a43a90da2e94e89b8e7da73c256989/externals/ffmpeg/CMakeLists.txt')
text = source.read_text()
for old, new in (
    ('--ld=${CMAKE_LINKER}', '--ld=${CMAKE_C_COMPILER}'),
    ('elseif (NOT (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES CMAKE_SYSTEM_PROCESSOR\n    AND CMAKE_HOST_SYSTEM_NAME MATCHES CMAKE_SYSTEM_NAME))', 'elseif (CMAKE_CROSSCOMPILING)'),
    ('set(FFmpeg_MAKE_ARGS -j${SYSTEM_THREADS})', 'set(FFmpeg_MAKE_ARGS -j6)'),
):
    assert text.count(old) == 1, old
    text = text.replace(old, new)
target = pathlib.Path(sys.argv[1]) / 'externals/ffmpeg/CMakeLists.txt'
if target.read_text() != text:
    target.write_text(text)
    (pathlib.Path(sys.argv[2]) / '_deps/ffmpeg-build/Makefile').unlink(missing_ok=True)
PY
cmake -S "$eden" -B "$scratch/build" -G Ninja \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DCMAKE_EXE_LINKER_FLAGS= -DEDEN_HOST_ASAN=OFF \
    -DCMAKE_PROJECT_yuzu_INCLUDE="$root/headless/inject.cmake" \
    -DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_OPENGL=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DYUZU_CRASH_DUMPS=OFF \
    -DYUZU_USE_EXTERNAL_FFMPEG=ON -DENABLE_WERROR=OFF -Dzstd_FORCE_BUNDLED=ON \
    -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON -DDYNARMIC_IGNORE_ASSERTS=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF
python3 -B "$root/headless/check_slab_lifetime.py" \
    "$scratch/build/headless/include/core/hle/kernel/slab_helpers.h" \
    "$scratch/source/src/core/hle/kernel/slab_helpers.h"
cmake --build "$scratch/build" --target eden-headless eden-romfs-check eden-devices-check eden-scalar-check eden-memory-check eden-ryujinx-check eden-mods-check eden-shader-cache-check eden-settings-check eden-patch-library-check eden-update-check eden-update-files-check eden-profiles-check eden-save-sync-check -j 6
python3 -B "$root/tools/check-sparse-header.py" "$scratch/build"
python3 -B "$root/tools/check-multiplayer-headers.py" "$scratch/build"
python3 -B "$root/tools/check-heap-growth.py"
python3 -B "$root/tools/check-crash-report.py"
python3 -B "$root/tools/check-stop-limit.py"
python3 -B "$root/tools/check-image-pages.py"
python3 -B "$root/tools/check-astc-policy.py"
python3 -B "$root/tools/check-guest-clock.py"
python3 -B "$root/tools/check-gc-scan.py"
"$scratch/build/bin/eden-scalar-check"
"$scratch/build/bin/eden-memory-check"
python3 -B "$root/tools/check-decoder-startup.py"
python3 -B "$root/headless/check_audio_shutdown.py" "$scratch/build/headless/core.cpp" "$scratch/source/src/core/core.cpp"
"$scratch/build/bin/eden-romfs-check"
"$scratch/build/bin/eden-ryujinx-check"
"$scratch/build/bin/eden-patch-library-check"
"$scratch/build/bin/eden-update-check"
"$scratch/build/bin/eden-update-files-check"
"$scratch/build/bin/eden-profiles-check"
"$scratch/build/bin/eden-save-sync-check"
"$scratch/build/bin/eden-mods-check"
"$scratch/build/bin/eden-shader-cache-check"
"$scratch/build/bin/eden-settings-check"
python3 -B "$root/tools/check-remote.py"
# The same against real RomM servers in Docker (only with ROMM_CHECK=1, and Docker).
python3 -B "$root/tools/check-romm.py"
bash tools/check-headless-devices.sh
cp "$scratch/build/bin/eden-headless" build/headless-host/eden-headless
cp "$scratch/build/compile_commands.json" build/headless-host/compile_commands.json
