#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepares everything the PS5 build needs besides Eden itself (make prepare), after the pinned
# inputs are in place (make deps): this checkout's build cache, Eden's source tree, the Payload
# SDK, FFmpeg for the PS5, the native packaging tool, the runtime libc.prx, the libSceAgcDriver
# link stub and the isolated RADV driver. Each step is skipped when its result already exists.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
step() { echo "== $*"; }

# Thousands of small files: keep the build cache on a native Linux filesystem, one per checkout.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    scratch="$cache_parent/ps5-eden-headless.$(printf '%s' "$root" | sha256sum | cut -c1-12)"
    mkdir -p .local "$scratch"
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$(cat "$scratch/owner")" == "$root" ]] || { echo "Build cache $scratch belongs to another checkout" >&2; exit 1; }

eden="$scratch/source"
eden_archive=eden-67bada77.tar.gz
# A tree unpacked from another archive is the source of another Eden revision: it is unpacked
# again (Eden's package cache, downloads only, is kept). A tree left from before an update of
# the pin was built as if it were the new one, and what Eden had changed went unseen.
if [[ ! -f $eden/CMakeLists.txt || $(cat "$eden/.unpacked-from" 2>/dev/null) != "$eden_archive" ]]; then
    step "Eden source"
    if [[ -d $eden/.cache ]]; then rm -rf "$scratch/source-cache" && mv "$eden/.cache" "$scratch/source-cache"; fi
    rm -rf "$eden"
    mkdir -p "$eden"
    tar -xzf ".deps/$eden_archive" --strip-components=1 -C "$eden"
    if [[ -d $scratch/source-cache ]]; then mv "$scratch/source-cache" "$eden/.cache"; fi
    printf '%s\n' "$eden_archive" > "$eden/.unpacked-from"
fi
printf '%s\n' '67bada77f8a43a90da2e94e89b8e7da73c256989' > "$eden/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$eden/GIT-REFSPEC"
# Optional: seed Eden's package cache from another checkout's (CI reuses a development cache).
if [[ -n ${EDEN_CPM_CACHE_SEED:-} && ! -d $eden/.cache/cpm ]]; then
    step "Eden package cache from $EDEN_CPM_CACHE_SEED"
    mkdir -p "$eden/.cache"
    cp -a "$EDEN_CPM_CACHE_SEED" "$eden/.cache/cpm"
fi
# The FFmpeg source Eden pins, where Eden's package cache keeps it.
ffmpeg_source="$eden/.cache/cpm/ffmpeg/c7b5f1537d"
if [[ ! -f $ffmpeg_source/configure ]]; then
    step "FFmpeg source"
    mkdir -p "$(dirname "$ffmpeg_source")"
    cp -a .deps/ffmpeg-c7b5f1537d "$ffmpeg_source"
fi

if [[ ! -f $scratch/sdk/.complete ]]; then
    step "Payload SDK"
    mkdir -p "$scratch/sdk"
    cp -a ../ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk/target "$scratch/sdk/"
    touch "$scratch/sdk/.complete"
fi
if [[ ! -f $scratch/ffmpeg-native/install/lib/libavcodec.a ]]; then
    step "FFmpeg for the PS5"
    bash tools/build-headless-ffmpeg.sh
fi
step "Native packaging tool"
bash tools/build-native-tool.sh
# The app's runtime libc.prx: the boilerplate's clean-room build, checked against its recorded digest.
if [[ ! -f ../ps5-native-app-boilerplate/runtime/libc.prx ]]; then
    step "Runtime libc.prx"
    bash ../ps5-native-app-boilerplate/tools/rebuild-libc.sh
fi
step "libSceAgcDriver link stub"
bash tools/build-agc-driver-stub.sh
bash tools/build-common-dialog-stub.sh
# The driver's display code carries this repository's adaptation (tools/patch-radv-wsi.py): a
# driver built with another version of it is built again (only the changed file compiles).
# So is a driver built from another Mesa revision than tools/deps.json pins.
radv_release=../mihawk-vulkan-review/.deps/native/radv-release
mesa_pin=$(python3 -c 'import json; print(next(i["commit"] for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"))')
if [[ -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
    built=$(sed -n 's/^revision: //p' "$radv_release/PROVENANCE.txt" 2>/dev/null || true)
    if [[ $built != "$mesa_pin" ]]; then
        step "RADV: built from Mesa ${built:0:12}, pinned ${mesa_pin:0:12}"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-wsi.py --check "$radv_release/EDEN_WSI_SHA256"; then
        step "RADV: the display code's adaptation changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    fi
fi
if [[ ! -f build/radv-isolated/libvulkan_radeon.ps5.a ]]; then
    if [[ ! -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
        step "RADV (Mesa) for the PS5 - this takes a while"
        bash tools/build-radv-dependencies.sh
    fi
    step "Isolating RADV beside the OpenGL Mesa"
    python3 -B tools/isolate-radv.py
fi
step "Prepared: $scratch"
