#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by both desktop previews; the PS5 target already links these dependencies.
cc=${HOST_CC:-cc}
for name in miniz miniz_tdef miniz_tinfl miniz_zip; do
    source="$root/third_party/miniz/$name.c"
    object="$build/obj/launcher_update_$name.o"
    if [[ ! -e $object || $source -nt $object || -n $(find "$root/third_party/miniz" -name '*.h' -newer "$object" -print -quit) ]]; then
        "$cc" -O2 -I"$root/third_party/miniz" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
source="$root/headless/remote/http.c"
object="$build/obj/launcher_update_http.o"
if [[ ! -e $object || $source -nt $object || $root/headless/remote/http.h -nt $object ]]; then
    "$cc" -O2 -DREMOTE_HTTP_HOST=1 -c "$source" -o "$object"
fi
objects+=("$object")
