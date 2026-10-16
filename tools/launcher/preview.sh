#!/usr/bin/env bash
# ProsperoEden - Draw the launcher's screens on a PC (Mesa surfaceless EGL) to PNG files.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/launcher/preview.sh [output dir] [width height]
#        tools/launcher/preview.sh --tour <video.mp4> [width height]   (needs ffmpeg)
# The same screens, shaders and font as on the console, with sample data.
# PE_LANG=<tag> shows them in that language (ui/lang/<tag>.po).
# PE_LOOK=large,contrast,calm shows them with those accessibility settings on.
# PE_SYSTEM_FONTS=<folder> names a folder with copies of the console's fonts
# (/preinst/common/font): Japanese, Korean, Chinese, Greek, Thai and Arabic text is drawn with
# them, as on the console. Without it such text shows as question marks.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source_dir="$root/headless/prosperoeden"
build=${PROSPEROEDEN_LAUNCHER_BUILD:-"$HOME/.cache/prosperoeden-launcher"}
cxx=${HOST_CXX:-c++}
mkdir -p "$build/obj"
python3 -B "$root/tools/deps.py" fetch harfbuzz ui-kit
harfbuzz=$(python3 -B "$root/tools/deps.py" path harfbuzz)
harfbuzz=${harfbuzz%/harfbuzz.cc}
# The UI kit's theme table: the launcher's themes start from its recipes (pe/ui/theme_kit.cpp).
kit=$(python3 -B "$root/tools/deps.py" path ui-kit)/src
# nlohmann/json (pe/ui/runtime_loading.hpp): the copy in the Eden source `make prepare` unpacks.
json=$(ls -d "$(cat "$root/.local/headless-cache" 2>/dev/null)"/source/.cache/cpm/nlohmann_json/*/single_include \
    2>/dev/null | head -1)
[[ -n $json ]] || { echo "nlohmann/json not found: run make prepare first" >&2; exit 1; }

sources=("$source_dir"/host/*.cpp "$source_dir"/pe/core/*.cpp "$source_dir"/pe/gfx/*.cpp "$source_dir"/pe/ui/*.cpp
    "$kit/ui/theme.cpp")
objects=()
pids=()
for source in "${sources[@]}"; do
    relative=${source#"$source_dir/"}
    object="$build/obj/${relative//\//_}.o"
    objects+=("$object")
    # Rebuild when the source or any launcher header is newer than the object.
    if [[ ! -e $object || $source -nt $object ]] ||
        [[ $relative != pe/gfx/harfbuzz.cpp && -n $(find "$source_dir/pe" "$source_dir/host" \( -name '*.hpp' -o -name '*.inc' \) -newer "$object" -print -quit) ]]; then
        # HarfBuzz is not ours to tidy: its warnings are off.
        warnings=(-Wall -Wextra)
        [[ $relative == pe/gfx/harfbuzz.cpp ]] && warnings=(-w)
        "$cxx" -std=c++20 -O2 "${warnings[@]}" -DGL_GLEXT_PROTOTYPES=1 -I"$source_dir" \
            -I"$source_dir/host" -I"$root/third_party/lua" -I"$root/tools/launcher/stb" -I"$root/third_party" -I"$root/third_party/miniz" -I"$harfbuzz" -I"$kit" -I"$json" \
            -c "$source" -o "$object" &
        pids+=($!)
        if [[ ${#pids[@]} -ge ${HOST_BUILD_JOBS:-2} ]]; then
            wait "${pids[0]}"
            pids=("${pids[@]:1}")
        fi
    fi
done
for pid in "${pids[@]}"; do wait "$pid"; done
for source in "$root"/third_party/lua/*.c; do
    name=${source##*/}
    case $name in lua.c|luac.c|linit.c|liolib.c|loslib.c|loadlib.c|ldblib.c) continue;; esac
    object="$build/obj/lua_${name}.o"
    if [[ ! -e $object || $source -nt $object ]]; then
        "$cxx" -x c++ -std=c++20 -O2 -w -include "$root/third_party/lua/runtime_api.hpp" -I"$root/third_party/lua" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
for source in "$source_dir"/pe/audio/*.cpp; do
    object="$build/obj/audio_${source##*/}.o"
    if [[ ! -e $object || $source -nt $object || -n $(find "$source_dir/pe/audio" -name '*.hpp' -newer "$object" -print -quit) ]]; then
        "$cxx" -std=c++20 -O2 -I"$source_dir" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
source "$root/tools/launcher/update-deps.sh"
"$cxx" "${objects[@]}" -lEGL -lGL -lm -lcrypto -lcurl -pthread -o "$build/pe_preview"

run() {
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 "$build/pe_preview" "$@"
}
if [[ ${1:-} == --tour ]]; then
    video=${2:?video file}
    width=${3:-1280}
    height=${4:-720}
    mkdir -p "$build/tour"
    run "$source_dir/ui" "$build/tour" --tour "$width" "$height" |
        ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s "${width}x${height}" -r 60 -i - \
            -c:v libx264 -pix_fmt yuv420p -crf 20 -movflags +faststart "$video"
    echo "$video"
else
    output=${1:-"$root/build/launcher-preview"}
    mkdir -p "$output"
    run "$source_dir/ui" "$output" "${@:2}"
fi
