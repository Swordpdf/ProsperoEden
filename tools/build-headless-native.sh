#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ $# == 0 || ( $# == 1 && ( "$1" == --devices || "$1" == --graphics || "$1" == --gpu-probe ) ) ]]
devices=OFF
graphics=OFF
probe=OFF
if [[ ${1:-} == --devices ]]; then devices=ON; fi
if [[ ${1:-} == --graphics ]]; then graphics=ON; fi
if [[ ${1:-} == --gpu-probe ]]; then graphics=ON; probe=ON; fi
jobs=${EDEN_BUILD_JOBS:-$(nproc)}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid EDEN_BUILD_JOBS: $jobs" >&2; exit 1; }
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
command -v ninja >/dev/null
cache=$(command -v ccache)
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
bash tools/build-core-fixture.sh
scratch=$(cat .local/headless-cache)
[[ "$scratch" == "${XDG_CACHE_HOME:-$HOME/.cache}"/ps5-eden-headless.* && "$(cat "$scratch/owner")" == "$root" ]]
# Cache the read-only native dependency trees beside the upstream sources.
# Repeated header checks across /mnt/c took minutes before compilation began.
if [[ ! -f "$scratch/sdk/.complete" ]]; then
    mkdir -p "$scratch/sdk"
    cp -a "$root/../ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk/target" "$scratch/sdk/"
    touch "$scratch/sdk/.complete"
fi
if [[ ! -f "$scratch/cdeps/.complete" ]]; then
    mkdir -p "$scratch/cdeps"
    cp -a "${EDEN_CDEPS_SOURCE:-$root/.deps/pacbrew-0.40.2/opt/ps5-payload-sdk/target/user/homebrew}/." "$scratch/cdeps/"
    touch "$scratch/cdeps/.complete"
fi
export PS5_PAYLOAD_SDK="$scratch/sdk"
export EDEN_NATIVE_C_DEPS="$scratch/cdeps"
export PKG_CONFIG_LIBDIR="$scratch/empty-pkgconfig"
export PKG_CONFIG_PATH=
mkdir -p "$PKG_CONFIG_LIBDIR"
cdeps="$EDEN_NATIVE_C_DEPS"
test -f "$cdeps/lib/libcrypto.a"
ffmpeg="$scratch/ffmpeg-native/install"
test -f "$ffmpeg/lib/libavcodec.a" || { echo 'Run tools/build-headless-ffmpeg.sh first.' >&2; exit 1; }
cmake -S "$scratch/source" -B "$scratch/native-local" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/headless/ps5.cmake" -DPS5_NATIVE=ON -DEDEN_DEVICE_FRONTEND="$devices" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF \
    -DEDEN_PS5_VULKAN="${EDEN_PS5_VULKAN:-OFF}" \
    -DEDEN_VULKAN_DRIVER="${EDEN_VULKAN_DRIVER:-CUSTOM}" \
    -DEDEN_DEV_VULKAN="${EDEN_DEV_VULKAN:-OFF}" \
    -DCMAKE_C_COMPILER_LAUNCHER="$cache" -DCMAKE_CXX_COMPILER_LAUNCHER="$cache" \
    -DEDEN_DEV_ROM_ID="${EDEN_DEV_ROM_ID:-}" \
    -DEDEN_DEV_PROFILE="${EDEN_DEV_PROFILE:-OFF}" \
    -DEDEN_DEV_PROFILE_TITLE="${EDEN_DEV_PROFILE_TITLE:-$(cat "$root/.local/dev-profile-title" 2>/dev/null || true)}" \
    -DEDEN_DEV_WAIT_CALLERS="${EDEN_DEV_WAIT_CALLERS:-OFF}" \
    -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=ON \
    -DCMAKE_PROJECT_yuzu_INCLUDE="$root/headless/inject.cmake" \
    -DEDEN_GPU_PROBE="$probe" -DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_OPENGL="$graphics" -DEDEN_PS5_OPENGL="$graphics" -DENABLE_CUBEB=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DYUZU_CRASH_DUMPS=OFF \
    -U 'FFmpeg_*' -DYUZU_USE_EXTERNAL_FFMPEG=OFF -DFFMPEG_DIR="$ffmpeg" \
    -DOPENSSL_INCLUDE_DIR="$cdeps/include" -DOPENSSL_SSL_LIBRARY="$cdeps/lib/libssl.a" \
    -DOPENSSL_CRYPTO_LIBRARY="$cdeps/lib/libcrypto.a" -DOpenSSL_FORCE_SYSTEM=ON \
    -DZLIB_INCLUDE_DIR="$cdeps/include" -DZLIB_LIBRARY="$cdeps/lib/libz.a" \
    -DENABLE_WERROR=OFF -Dzstd_FORCE_BUNDLED=ON -DBoost_FORCE_BUNDLED=ON -Dfmt_FORCE_BUNDLED=ON \
    -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON -DDYNARMIC_IGNORE_ASSERTS=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF
python3 -B "$root/headless/check_slab_lifetime.py" \
    "$scratch/native-local/headless/include/core/hle/kernel/slab_helpers.h" \
    "$scratch/source/src/core/hle/kernel/slab_helpers.h"
echo "Building Eden with $jobs parallel jobs"
cmake --build "$scratch/native-local" --target eden-headless core -j "$jobs"
# In every build: an object that saw Eden's own table header would write a table the rest keeps sparse.
python3 -B "$root/tools/check-sparse-header.py" "$scratch/native-local"
python3 -B "$root/tools/check-multiplayer-headers.py" "$scratch/native-local"
# Source checks of the release configuration (development builds opt out: EDEN_SKIP_SOURCE_CHECKS=1).
[[ ${EDEN_SKIP_SOURCE_CHECKS:-0} == 1 ]] && exit 0
python3 -B "$root/headless/check_audio_shutdown.py" "$scratch/native-local/headless/core.cpp" "$scratch/source/src/core/core.cpp"
python3 -B "$root/tools/check-load-failure.py"
python3 -B "$root/tools/check-nso-memory.py"
python3 -B "$root/tools/check-performance.py"
python3 -B "$root/tools/check-performance-settings.py"
python3 -B "$root/tools/check-startup-performance.py"
python3 -B "$root/tools/check-worker-affinity.py"
python3 -B "$root/tools/check-tsc-fallback.py"
python3 -B "$root/tools/check-jit-protection.py"
python3 -B "$root/tools/check-jit-allocator.py"
python3 -B "$root/tools/check-jit-patch-lookup.py"
python3 -B "$root/tools/check-jit-assert.py"
python3 -B "$root/tools/check-crash-report.py"
if [[ "$probe" == OFF ]]; then
    python3 -B "$root/tools/check-exclusive-monitor.py"
else
    # The graphics-only entry returns before Core construction; its JIT is dead-stripped.
    python3 -B "$root/tools/check-gpu-probe.py" --self-test
    python3 -B "$root/tools/check-native-queue-probe.py"
fi
if [[ "$graphics" == ON ]]; then
    python3 -B "$root/tools/check-native-gpu-thread.py"
    python3 -B "$root/tools/check-gpu-producer-stop.py"
    python3 -B "$root/tools/check-gpu-sync-stop.py"
    python3 -B "$root/tools/check-nvdrv-process-lifetime.py"
    python3 -B "$root/tools/check-integer-buffer-clear.py"
    python3 -B "$root/tools/check-game-capture-shutdown.py"
fi

python3 -B tools/check-hud.py
python3 -B tools/check-game-frame-summary.py
