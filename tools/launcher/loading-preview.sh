#!/usr/bin/env bash
# ProsperoEden - Draw the game loading screen on a PC (Mesa surfaceless EGL) to PNG files.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/launcher/loading-preview.sh [output dir] [width height [cover.tga]]
# The shader of the loading screen (headless/loading_scene.glsl with its lettering,
# loading_text.glsl) compiled as the console's OpenGL renderer compiles it, at a few moments of a
# game's start: each step, shaders being built, a slow step, with reduced motion, and for a game
# without a cover. The cover is the TGA given (the launcher keeps them in its covers folder) or
# one drawn by the tool.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${PROSPEROEDEN_LAUNCHER_BUILD:-"$HOME/.cache/prosperoeden-launcher"}
mkdir -p "$build"
"${HOST_CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -DGL_GLEXT_PROTOTYPES=1 -I"$root/tools/launcher/stb" -I"$root/headless" \
    "$root/tools/launcher/loading_preview.cpp" -lEGL -lGL -o "$build/loading_preview" 2> >(grep -v stb_image_write >&2 || true)
output=${1:-"$root/build/loading-preview"}
mkdir -p "$output"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe "$build/loading_preview" \
    "$root/headless" "$output" "${@:2}"
