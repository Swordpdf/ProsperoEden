#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release build for CI (.github/workflows/release.yml): `make release` in this checkout.
# EDEN_DEV_CHECKOUT optionally names a development checkout whose inputs are reused instead of
# fetched: its sibling repositories and .deps inputs are linked in (never copied or modified),
# and its Eden package cache seeds this checkout's. See docs/BUILDING.md.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
if [[ -n ${EDEN_DEV_CHECKOUT:-} ]]; then
    dev=$(cd -- "$EDEN_DEV_CHECKOUT" && pwd)
    if [[ $dev != "$root" ]]; then
        for sibling in ps5-native-app-boilerplate mihawk-vulkan-review mihawk-mesa-review mihawk-sdk-review; do
            if [[ ! -e ../$sibling && -d $dev/../$sibling ]]; then
                ln -s "$(cd -- "$dev/../$sibling" && pwd)" "../$sibling"
            fi
        done
        mkdir -p .deps build
        for input in "$dev"/.deps/eden-67bada77.tar.gz "$dev"/.deps/mirror-* "$dev"/.deps/ffmpeg-* \
                     "$dev"/.deps/fmt-12.1.0 "$dev"/.deps/compiler-rt-18.1.8 "$dev"/.deps/pacbrew-* \
                     "$dev"/.deps/ps5-opengl-sdk-*; do
            [[ -e $input && ! -e .deps/${input##*/} ]] && ln -s "$input" ".deps/${input##*/}"
        done
        [[ -e build/radv-isolated || ! -d $dev/build/radv-isolated ]] || ln -s "$dev/build/radv-isolated" build/radv-isolated
        if [[ -f $dev/.local/headless-cache ]]; then
            export EDEN_CPM_CACHE_SEED="$(cat "$dev/.local/headless-cache")/source/.cache/cpm"
        fi
    fi
fi
make release
