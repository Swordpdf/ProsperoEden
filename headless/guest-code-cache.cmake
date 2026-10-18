# SPDX-License-Identifier: GPL-3.0-or-later
# Instruction-byte caches must follow invalidation of shared translated blocks. Keep page
# contents private to the translating thread; other cores invalidate through an atomic epoch.
function(eden_code_cache_replace variable before after)
    string(FIND "${${variable}}" "${before}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Pinned A64 instruction-byte cache changed")
    endif()
    string(REPLACE "${before}" "${after}" changed "${${variable}}")
    set(${variable} "${changed}" PARENT_SCOPE)
endfunction()

file(READ "${PROJECT_SOURCE_DIR}/src/core/arm/dynarmic/arm_dynarmic_64.h" code_cache_header)
eden_code_cache_replace(code_cache_header
    "        last_code_addr = u64(-1); //reset back, force refetch"
    "        code_cache_generation.fetch_add(1, std::memory_order_release);")
eden_code_cache_replace(code_cache_header
    "    u64 last_code_addr = u64(-1);"
    "    u64 last_code_addr = u64(-1);\n    std::atomic<u64> code_cache_generation{0};\n    u64 cached_code_generation = 0;")
set(code_cache_header_path "${PORT_BUILD_DIR}/include/core/arm/dynarmic/arm_dynarmic_64.h")
write_derived("${code_cache_header_path}" "${code_cache_header}")
# core already searches PORT_BUILD_DIR/include before upstream headers. Explicit dependencies
# also rebuild its two consumers when this override is first added to an existing build cache.
set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/k_process.cpp"
    "${PORT_BUILD_DIR}/k_process.cpp" "${PORT_BUILD_DIR}/arm_dynarmic_64.cpp"
    TARGET_DIRECTORY core APPEND PROPERTY OBJECT_DEPENDS "${code_cache_header_path}")

function(eden_guest_code_cache variable)
    set(text "${${variable}}")
    eden_code_cache_replace(text
        "#include \"core/arm/dynarmic/arm_dynarmic_64.h\""
        "#include \"core/arm/dynarmic/arm_dynarmic_64.h\"\n#include \"core/arm/debug.h\"")
    eden_code_cache_replace(text
        "    if (last_code_addr != aligned_vaddr) {"
        "    const u64 generation = code_cache_generation.load(std::memory_order_acquire);\n    if (cached_code_generation != generation || last_code_addr != aligned_vaddr) {")
    eden_code_cache_replace(text
        "        last_code_addr = aligned_vaddr;"
        "        last_code_addr = aligned_vaddr;\n        cached_code_generation = generation;")
    eden_code_cache_replace(text
        "    last_code_addr = u64(-1); //invalidate cached page"
        "    code_cache_generation.fetch_add(1, std::memory_order_release);")
    # This also covers the non-shared JIT fallback. The shared group already unlinks compiled
    # blocks on every member, but each member's cached instruction bytes need their own epoch.
    eden_code_cache_replace(text
        "        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);"
        "        ::Core::InvalidateInstructionCacheRange(m_process, cache_line_start, ICACHE_LINE_SIZE);")
    eden_code_cache_replace(text
        "    m_cb->last_code_addr = u64(-1);"
        "    m_cb->code_cache_generation.fetch_add(1, std::memory_order_release);")
    eden_code_cache_replace(text
        "void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {\n    m_jit->InvalidateCacheRange(addr, size);"
        "void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {\n    m_cb->code_cache_generation.fetch_add(1, std::memory_order_release);\n    m_jit->InvalidateCacheRange(addr, size);")
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()
