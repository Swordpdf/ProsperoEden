#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template="$root/../ps5-native-app-boilerplate"
sdk="$PS5_PAYLOAD_SDK"
output=$1
shift
libraries=()
radv_archive=""
for argument in "$@"; do
    case "$argument" in
        */libvulkan_radeon.ps5.a) radv_archive="$argument" ;;
        # These functions are supplied by the native libc/kernel providers.
        -Xlinker|-pthread|-lpthread|-ldl|-lrt|-lm) ;;
        # Native titles load libkernel, not the web-process provider. Preserve
        # Eden's qualified sysconf/pthread imports when consuming the GL SDK.
        -lkernel_web) libraries+=("$sdk/target/lib/libkernel.so") ;;
        -Wl,*) IFS=, read -r -a flags <<< "${argument#-Wl,}"; libraries+=("${flags[@]}") ;;
        *) libraries+=("$argument") ;;
    esac
done
radv_link_flags=()
radv_link_inputs=()
tls_flags=(--defsym=__cxa_thread_atexit_impl=0)
if [[ -n $radv_archive ]]; then
    source "$root/tools/radv-link-eden.sh"
    eden_radv_link_recipe "$radv_archive"
    # RADV's platform provides the real thread-local destructor registration.
    tls_flags=()
fi
# Mesa's Vulkan entrypoint tables reference optional driver entrypoints as weak
# symbols (vk_entrypoints_gen --weak). An unimplemented one must resolve to 0,
# not become a dynamic import the native packager has no SDK stub for.
# lld 21+ no longer exports undefined weak symbols from executables by default
# (-z dynamic-undefined-weak opts back in); lld 18 still does, and its only switch
# for that is --no-dynamic-linker, which otherwise affects just PT_INTERP, absent
# from the PS5 link script anyway. Every one of these weak imports is a Mesa
# entrypoint-table reference that resolves to 0 either way.
# The SDK's dlfcn wrappers explicitly return unavailable when these optional
# weak hooks are null. This static frontend supplies no dynamic-loader hooks.
"$template/.deps/native/ps5-payload-sdk/bin/prospero-lld" \
    "${tls_flags[@]}" "${radv_link_flags[@]}" -L "$sdk/target/lib" \
    --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 \
    --defsym=__dlclose=0 --defsym=__dlerror=0 \
    -T "$template/tooling/native/ps5-pie.ld" -T "$root/tools/unwind.ld" \
    --eh-frame-hdr --gc-sections --version-script "$root/tools/app-symbols.map" -e _start \
    --no-dynamic-linker \
    --error-limit=0 -Map="$output.map" \
    --wrap=aligned_alloc --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free \
    --wrap=posix_memalign --wrap=malloc_usable_size \
    --wrap=sceKernelAllocateDirectMemory --wrap=sceKernelReleaseDirectMemory \
    -o "$output" --start-group "${libraries[@]}" "${radv_link_inputs[@]}" \
    "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" \
    --end-group --as-needed "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
    "$sdk/target/lib/libc.a" "$sdk/target/lib/libSceNet.so"
