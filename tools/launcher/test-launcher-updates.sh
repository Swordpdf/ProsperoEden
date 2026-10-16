#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only tests; requires C++20, Python 3, nlohmann/json, libcurl and OpenSSL headers.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${PROSPEROEDEN_UPDATE_TEST_BUILD:-"$HOME/.cache/prosperoeden-launcher-update-tests"}
cxx=${HOST_CXX:-c++}
cc=${HOST_CC:-cc}
mkdir -p "$build/obj"
objects=()
source_dir="$root/headless/prosperoeden"
source "$root/tools/launcher/update-deps.sh"
for source in "$root"/third_party/lua/*.c; do
    name=${source##*/}
    case $name in lua.c|luac.c|linit.c|liolib.c|loslib.c|loadlib.c|ldblib.c) continue;; esac
    object="$build/obj/lua_$name.o"
    if [[ ! -e $object || $source -nt $object || $root/third_party/lua/runtime_api.hpp -nt $object ]]; then
        "$cxx" -x c++ -std=c++20 -O2 -w -include "$root/third_party/lua/runtime_api.hpp" \
            -I"$root/third_party/lua" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -I"$source_dir" -I"$root/third_party" \
    -I"$root/third_party/miniz" -I"$root/third_party/lua" \
    "$source_dir/pe/ui/launcher_updates.cpp" "$root/tools/launcher/test-launcher-updates.cpp" \
    "${objects[@]}" -lcrypto -lcurl -pthread -o "$build/test-updates"
python3 -B "$root/tools/launcher/test-launcher-updates.py" "$build/test-updates"
for name in update_check self_update_sha256; do
    "$cc" -std=c11 -O2 -DUPDATE_CHECK_NO_NETWORK -I"$root/headless/update_check" \
        -c "$root/headless/update_check/$name.c" -o "$build/obj/compat_$name.o"
done
"$cxx" -std=c++20 -O2 -ffunction-sections -fdata-sections \
    -I"$root/headless/self_update_helper" -I"$root/headless/update_check" \
    "$root/tools/launcher/test-launcher-update-compat.cpp" \
    "$root/headless/self_update_helper/updater.cpp" "$root/headless/self_update_helper/files.cpp" \
    "$build/obj/compat_update_check.o" "$build/obj/compat_self_update_sha256.o" \
    -Wl,--gc-sections -pthread -o "$build/test-compat"
case_directory=$(mktemp -d "$build/compat.XXXXXX")
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$case_directory"' EXIT
"$build/test-compat" "$case_directory"

"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -I"$source_dir" -I"$root/third_party" \
    "$root/tools/launcher/test-runtime-catalog.cpp" -o "$build/test-catalog"
"$build/test-catalog"
