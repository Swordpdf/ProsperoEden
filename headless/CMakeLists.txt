option(EDEN_PS5_VULKAN "Build the native Vulkan renderer alongside OpenGL" OFF)
set(EDEN_VULKAN_DRIVER "CUSTOM" CACHE STRING "Native Vulkan driver: CUSTOM or RADV")
set_property(CACHE EDEN_VULKAN_DRIVER PROPERTY STRINGS CUSTOM RADV)
if(NOT EDEN_VULKAN_DRIVER MATCHES "^(CUSTOM|RADV)$")
    message(FATAL_ERROR "EDEN_VULKAN_DRIVER must be CUSTOM or RADV")
endif()
set(PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PORT_BUILD_DIR "${CMAKE_BINARY_DIR}/headless")
# One compiled-code namespace per guest process for the A64 JITs (headless/dynarmic/jit_group.h).
include("${CMAKE_CURRENT_LIST_DIR}/dynarmic/shared-jit.cmake")
# Register-allocator bookkeeping visits only the host locations in use (compile time).
include("${CMAKE_CURRENT_LIST_DIR}/dynarmic/reg_alloc.cmake")
if(PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(EDEN_SHARED_JIT ON)
endif()
file(MAKE_DIRECTORY "${PORT_BUILD_DIR}")
file(MAKE_DIRECTORY "${PORT_BUILD_DIR}/recovery/common")
file(REMOVE "${PORT_BUILD_DIR}/include/common/slot_vector.h")
set(SLOT_VECTOR_INPUT "${PROJECT_SOURCE_DIR}/src/common/slot_vector.h")
set(SLOT_VECTOR_OUTPUT "${PORT_BUILD_DIR}/recovery/common/slot_vector.h")
include("${CMAKE_CURRENT_LIST_DIR}/slot_vector_recovery.cmake")
# A distinct public include path also invalidates Ninja's cached dependencies:
# adding a shadow header under an existing include path alone would not.
target_include_directories(common BEFORE PUBLIC "${PORT_BUILD_DIR}/recovery")
# A consumer's own source include path precedes inherited includes. All current
# SlotVector users live in video_core, so place its override before that path too.
target_include_directories(video_core BEFORE PRIVATE "${PORT_BUILD_DIR}/recovery")
file(GLOB overlay_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${overlay_inputs})
function(write_derived path content)
    # Preserve timestamps when an overlay is unchanged; otherwise every
    # configure needlessly recompiles hundreds of dependent source files.
    file(WRITE "${path}.in" "${content}")
    configure_file("${path}.in" "${path}" COPYONLY)
endfunction()
set(MULTIPLAYER_SOURCE "${PROJECT_SOURCE_DIR}")
set(MULTIPLAYER_OUTPUT "${PORT_BUILD_DIR}/multiplayer")
include("${EDEN_PORT_DIR}/multiplayer_packets.cmake")
# Keep successful JIT invariant checks inline; preserve upstream failure handling.
# Use a private include root so other core/renderer targets retain their headers.
file(READ "${PROJECT_SOURCE_DIR}/src/common/assert.h" jit_assert_header)
file(READ "${CMAKE_CURRENT_LIST_DIR}/jit_assert.inc" jit_assert_override)
write_derived("${PORT_BUILD_DIR}/jit-checks/common/assert.h"
    "${jit_assert_header}\n${jit_assert_override}")
target_include_directories(dynarmic BEFORE PRIVATE "${PORT_BUILD_DIR}/jit-checks")
# Let the existing compiler inline across the JIT's translation units. Keep the
# rest of the native application on its qualified non-LTO build path.
target_compile_options(dynarmic PRIVATE -flto=thin)
if(NOT PS5_NATIVE)
    target_link_options(dynarmic INTERFACE -fuse-ld=lld-18)
endif()

# Clang 18 does not accept the upstream GCC TLS-dialect flag. Use the same
# compiler-rt emulated TLS configuration already qualified by G6 on PS5.
# Linux must match its system libstdc++'s native TLS ABI.
foreach(eden_target common core audio_core video_core hid_core network input_common frontend_common shader_recompiler gpu_logging dynarmic)
    if(TARGET ${eden_target})
        get_target_property(options ${eden_target} COMPILE_OPTIONS)
        if(options)
            list(FILTER options EXCLUDE REGEX "mtls-dialect")
            set_property(TARGET ${eden_target} PROPERTY COMPILE_OPTIONS "${options}")
        endif()
        if(PS5_NATIVE)
            target_compile_options(${eden_target} PRIVATE -femulated-tls)
        endif()
    endif()
endforeach()
if(PS5_NATIVE)
    set(COMPILER_RT "${PORT_ROOT}/.deps/compiler-rt-18.1.8")
    file(READ "${COMPILER_RT}/emutls.c" emutls)
    string(REPLACE "#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 0" "#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 1" emutls "${emutls}")
    write_derived("${PORT_BUILD_DIR}/emutls.c" "${emutls}")
    target_sources(common PRIVATE "${PORT_BUILD_DIR}/emutls.c")
    target_include_directories(common PRIVATE "${COMPILER_RT}")
endif()
# ReShade's effect compiler takes alloca from <malloc.h>, which the Payload SDK replaces by an
# #error. Its own copy of that header, for this target alone.
if(PS5_NATIVE AND TARGET reshadefx)
    write_derived("${PORT_BUILD_DIR}/reshadefx-compat/malloc.h"
        "#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca __builtin_alloca\n#endif\n")
    target_include_directories(reshadefx BEFORE PRIVATE "${PORT_BUILD_DIR}/reshadefx-compat")
endif()
# Use the already-qualified no-arena allocation contract in the full core.
get_target_property(common_sources common SOURCES)
list(REMOVE_ITEM common_sources host_memory.cpp sparse_large_vector.cpp)
set_property(TARGET common PROPERTY SOURCES "${common_sources}")
target_sources(common PRIVATE "${PORT_ROOT}/src/host_memory.cpp" "${PORT_ROOT}/src/memory_pages.cpp")

# Eden's large tables (the guest page table, the GPU's address tables) stay sparse on the PS5:
# their vector reserves its range with AllocateSparsePages and announces a first write to
# CommitSparsePage (src/memory_pages.cpp) where upstream maps anonymous memory and calls
# mprotect. A dense 39-bit page table alone was 1 GiB of the 12 GiB the CPU shares with the GPU.
file(READ "${PROJECT_SOURCE_DIR}/src/common/sparse_large_vector.h" sparse_header)
function(eden_sparse_replace old new count)
    string(REPLACE "${old}" "" without "${sparse_header}")
    string(LENGTH "${sparse_header}" before)
    string(LENGTH "${without}" after)
    string(LENGTH "${old}" each)
    math(EXPR found "(${before} - ${after}) / ${each}")
    if(NOT found EQUAL count)
        message(FATAL_ERROR "Pinned sparse_large_vector.h changed: ${found} of '${old}', expected ${count}")
    endif()
    string(REPLACE "${old}" "${new}" changed "${sparse_header}")
    set(sparse_header "${changed}" PARENT_SCOPE)
endfunction()
eden_sparse_replace("AllocateMemoryPages(alloc_size)" "AllocateSparsePages(alloc_size)" 2)
eden_sparse_replace("FreeMemoryPages(base_ptr, alloc_size)" "FreeSparsePages(base_ptr, alloc_size)" 2)
# Eden defines the host page size, its bit count and its mask as three inline variables, the last
# two computed from the first. Each inline variable has an initializer of its own, and on the
# PS5 they do not run in the order of the header: the bit count and the mask were computed from
# a page size that was still zero, and every committed page then set a bit far outside the
# vector's bitmap, in whatever the heap kept next to it. Each one asks the system itself.
eden_sparse_replace("inline const u64 HostPageBits = std::countr_zero(HostPageSize);"
    "inline const u64 HostPageBits = std::countr_zero(static_cast<u64>(sysconf(_SC_PAGESIZE)));" 1)
eden_sparse_replace("inline const u64 HostPageMask = ~(HostPageSize - 1);"
    "inline const u64 HostPageMask = ~(static_cast<u64>(sysconf(_SC_PAGESIZE)) - 1);" 1)
# Eden tests the result; CommitSparsePage has none (it ends the app when memory runs out).
eden_sparse_replace("mprotect(reinterpret_cast<void*>(page), HostPageSize, PROT_READ | PROT_WRITE)"
    "(CommitSparsePage(page), 0)" 1)
eden_sparse_replace("void FreeMemoryPages(void* base, std::size_t size) noexcept;"
    "void FreeMemoryPages(void* base, std::size_t size) noexcept;\nvoid* AllocateSparsePages(std::size_t size) noexcept;\nvoid FreeSparsePages(void* base, std::size_t size) noexcept;\nvoid CommitSparsePage(std::uintptr_t page) noexcept;" 1)
write_derived("${PORT_BUILD_DIR}/sparse/common/sparse_large_vector.h" "${sparse_header}")

# An abandoned fiber stack is not unwound. Empty the by-value weak reference
# before switching; jump_fcontext returns the live destination supplied by the
# resumer, under that fiber's guard, just as the initial entry trampoline does.
file(READ "${PROJECT_SOURCE_DIR}/src/common/fiber.cpp" fiber_source)
string(REPLACE "    to.impl->previous_fiber = weak_from.lock();"
    "    to.impl->previous_fiber = weak_from.lock();\n    ASSERT(to.impl->previous_fiber);\n    weak_from.reset();" fiber_source "${fiber_source}")
string(REPLACE "    // \"from\" might no longer be valid if the thread was killed\n    if (auto from = weak_from.lock()) {"
    "    if (auto* from = static_cast<Fiber*>(transfer.data)) {" fiber_source "${fiber_source}")
write_derived("${PORT_BUILD_DIR}/fiber.cpp" "${fiber_source}")
get_target_property(common_sources common SOURCES)
list(REMOVE_ITEM common_sources fiber.cpp)
list(APPEND common_sources "${PORT_BUILD_DIR}/fiber.cpp")
set_property(TARGET common PROPERTY SOURCES "${common_sources}")

get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "exception_handler_posix\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
# A32 fastmem: src/host_memory.cpp aliases 32-bit guest memory into a 4 GiB window at
# the PS5's 16 KiB page size; this handler sends faulting JIT accesses to their
# page-table fallback. Without it the generic handler keeps fastmem off.
option(EDEN_A32_FASTMEM "A32 fastmem window with JIT fault recovery" ON)
if(EDEN_A32_FASTMEM AND (PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    target_sources(dynarmic PRIVATE "${EDEN_PORT_DIR}/fastmem_handler.cpp")
else()
    target_sources(dynarmic PRIVATE "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/exception_handler_generic.cpp")
endif()

  # Page-local W^X: constants stay RW/NX; old code stays RX unless patched.
  # Apply the same derivative to Linux host correctness/pressure tests.
if(PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
set(code_relative "dynarmic/backend/x64/block_of_code")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${code_relative}.h" code_header)
string(REPLACE "    bool prelude_complete = false;"
      "    size_t protection_end = 0;\n    size_t executable_end = 0;\n    size_t write_begin = 0;\n    bool writing = true;\n    std::vector<size_t> patch_pages;\n    bool prelude_complete = false;" code_header "${code_header}")
  write_derived("${PORT_BUILD_DIR}/include/${code_relative}.h" "#include <vector>\n${code_header}")
target_include_directories(dynarmic BEFORE PUBLIC "${PORT_BUILD_DIR}/include")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${code_relative}.cpp" code_source)
include("${EDEN_PORT_DIR}/jit-alias.cmake")
    string(REPLACE "#include \"dynarmic/backend/x64/block_of_code.h\""
        "#include \"dynarmic/backend/x64/block_of_code.h\"\n#include \"${EDEN_PORT_DIR}/jit-allocator.h\"" code_source "${code_source}")
    string(REPLACE "        , nullptr)\n    , constant_pool"
        "        , EdenJitAllocator())\n    , constant_pool" code_source "${code_source}")
if(PS5_NATIVE)
    string(PREPEND code_source "#define EDEN_JIT_ALIAS_NATIVE 1\n")
    string(REPLACE "    GenRunCode(rcp);"
        "    std::printf(\"EDEN_JIT_FEATURES mask=%x cache_bytes=%zu\\n\", unsigned(GetHostFeatures()), total_code_size);\n    GenRunCode(rcp);" code_source "${code_source}")
endif()
  # ConstantPool has completed construction before this body. Isolate its last
  # page from executable prelude, without accessing fields during construction.
  string(REPLACE "    EnableWriting();\n    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);"
      "    align(sysconf(_SC_PAGESIZE));\n    write_begin = getSize();\n    executable_end = write_begin;\n    if (hasWritableAlias() && mprotect(const_cast<u8*>(getCode()), write_begin, PROT_READ) != 0) std::abort();\n    EnableWriting();\n    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);" code_source "${code_source}")
  # Keep generated helper routines RX even while guest blocks are being patched.
  string(REPLACE "    prelude_complete = true;"
      "    align(sysconf(_SC_PAGESIZE));\n    prelude_complete = true;" code_source "${code_source}")
  file(READ "${EDEN_PORT_DIR}/jit-permissions.inc" permission_methods)
  string(FIND "${code_source}" "void BlockOfCode::EnableWriting()" permission_start)
  string(FIND "${code_source}" "size_t BlockOfCode::SpaceRemaining()" permission_end)
  if(permission_start LESS 0 OR permission_end LESS permission_start)
      message(FATAL_ERROR "Pinned JIT permission methods changed")
  endif()
  string(SUBSTRING "${code_source}" 0 ${permission_start} code_prefix)
  string(SUBSTRING "${code_source}" ${permission_end} -1 code_suffix)
  set(code_source "${code_prefix}${permission_methods}\n${code_suffix}")
  string(REPLACE "    setSize(required_size);" [=[    ASSERT(writing);
    ASSERT(!prelude_complete || required_size >= size_t(reinterpret_cast<const u8*>(code_begin) - getCode()));
    protection_end = std::max(protection_end, getSize());
    // Backward cursor moves are EmitX64::Patch sites, at most 23 bytes.
    static const size_t page = sysconf(_SC_PAGESIZE);
    const size_t patch_end = std::min(write_begin, required_size + 23);
    size_t first_new = patch_end, last_new = patch_end;
    for (size_t offset = required_size & ~(page - 1); !hasWritableAlias() && offset < patch_end; offset += page) {
        if (std::find(patch_pages.begin(), patch_pages.end(), offset) == patch_pages.end()) {
            patch_pages.push_back(offset);
            first_new = std::min(first_new, offset);
            last_new = offset + page;
        }
    }
    if (last_new > first_new) ProtectMemory(getCode() + first_new, last_new - first_new, false);
    setSize(required_size);
    protection_end = std::max(protection_end, getSize());]=] code_source "${code_source}")
  string(REPLACE "    size_t current_size = getCurr<const u8*>()"
      "    ASSERT(size <= 23);\n    size_t current_size = getCurr<const u8*>()" code_source "${code_source}")
string(REPLACE "    mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode);"
    "    const size_t span = (size + (iaddr - roundAddr) + pageSize - 1) & ~(pageSize - 1);\n    if (mprotect(reinterpret_cast<void*>(roundAddr), span, mode) != 0) std::abort();" code_source "${code_source}")
  write_derived("${PORT_BUILD_DIR}/block_of_code.cpp" "#include <algorithm>\n#include <cstdio>\n#include <cstdlib>\n#include <unistd.h>\n${code_source}")
get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "(^|/)block_of_code\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/block_of_code.cpp")
foreach(bits 32 64)
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/a${bits}_emit_x64.cpp" emitter)
    if(bits EQUAL 64)
        file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64.h" emitter_header)
        string(REPLACE "Emit(IR::Block& block) noexcept" "Emit(IR::Block& block, bool publish = true) noexcept" emitter_header "${emitter_header}")
        eden_shared_jit_emitter_header()
        write_derived("${PORT_BUILD_DIR}/include/dynarmic/backend/x64/a64_emit_x64.h" "${emitter_header}")
        string(REPLACE "A64EmitX64::Emit(IR::Block& block) noexcept" "A64EmitX64::Emit(IR::Block& block, bool publish) noexcept" emitter "${emitter}")
        string(REPLACE "    code.DisableWriting();\n    shared_labels.clear();" "    if (publish) code.DisableWriting();\n    shared_labels.clear();" emitter "${emitter}")
    endif()
    set(lookup_transitions "        code.DisableWriting();\n        (*fast_dispatch_table_lookup)(location.Value()) = {};\n        code.EnableWriting();")
    string(FIND "${emitter}" "${lookup_transitions}" lookup_offset)
    if(lookup_offset LESS 0)
        message(FATAL_ERROR "Pinned JIT fast-dispatch invalidation changed")
    endif()
    string(REPLACE "${lookup_transitions}"
        "        // Prelude occupies separate RX pages throughout guest-code patching.\n        (*fast_dispatch_table_lookup)(location.Value()) = {};" emitter "${emitter}")
    if(bits EQUAL 64)
        eden_shared_jit_emitter()
    endif()
    write_derived("${PORT_BUILD_DIR}/a${bits}_emit_x64.cpp" "${emitter}")
    get_target_property(jit_sources dynarmic SOURCES)
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)a${bits}_emit_x64\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/a${bits}_emit_x64.cpp")
endforeach()
endif()

# Compiled-block code ranges by 4 KiB guest page (headless/dynarmic/block_range_information.*):
# upstream's interval map of hash sets split intervals and copied sets on every compilation that
# overlaps earlier blocks, ~8% of A64 compile time on the console.
file(READ "${CMAKE_CURRENT_LIST_DIR}/dynarmic/block_range_information.h" range_header)
write_derived("${PORT_BUILD_DIR}/include/dynarmic/backend/block_range_information.h" "${range_header}")
get_target_property(range_sources dynarmic SOURCES)
list(FILTER range_sources EXCLUDE REGEX "(^|/)block_range_information\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${range_sources}")
target_sources(dynarmic PRIVATE "${CMAKE_CURRENT_LIST_DIR}/dynarmic/block_range_information.cpp")
# The override header shadows upstream's only by include order, which build tools do not track:
# a definition on the target recompiles every dynarmic object that saw the upstream layout.
target_compile_definitions(dynarmic PRIVATE EDEN_BLOCK_RANGE_PAGES=1)
if(EDEN_SHARED_JIT)
    eden_shared_jit_sources()
endif()
if(PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    eden_reg_alloc_sources()
endif()

# Patch lookup must not create a large empty record for every compiled block.
# Keep the upstream implementation and change only its absent-target path.
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp" emitter)
set(patch_lookup "    const PatchInformation& patch_info = patch_information[target_desc];")
string(FIND "${emitter}" "${patch_lookup}" patch_lookup_offset)
if(patch_lookup_offset LESS 0)
    message(FATAL_ERROR "Pinned JIT patch lookup changed")
endif()
string(REPLACE "${patch_lookup}"
    "    const auto patch = patch_information.find(target_desc);\n    if (patch == patch_information.end()) return;\n    const PatchInformation& patch_info = patch->second;" emitter "${emitter}")
if(PS5_NATIVE)
    # PS5 has no perf-map writer. Its no-op still evaluates the formatted name.
    string(REPLACE "    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));"
        "" emitter "${emitter}")
endif()
write_derived("${PORT_BUILD_DIR}/emit_x64.cpp" "${emitter}")
get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "(^|/)emit_x64\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/emit_x64.cpp")

# Scalar loads use S/D register tracking; scalar arithmetic must use the same
# widths so GetSetElimination can retain their values instead of spilling Q.
set(translator_relative "dynarmic/frontend/A64/translate")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${translator_relative}/impl/impl.cpp" scalar_source)
set(scalar_read "    return ir.VectorGetElement(bitsize, ir.GetQ(vec), 0);")
set(scalar_write "    ir.SetQ(vec, ir.ZeroExtendToQuad(value));")
foreach(anchor IN ITEMS scalar_read scalar_write)
    string(FIND "${scalar_source}" "${${anchor}}" offset)
    if(offset LESS 0)
        message(FATAL_ERROR "Pinned scalar register helper changed")
    endif()
endforeach()
string(REPLACE "${scalar_read}"
    "    return ir.VectorGetElement(bitsize, bitsize == 32 || bitsize == 64 ? V(bitsize, vec) : ir.GetQ(vec), 0);" scalar_source "${scalar_source}")
string(REPLACE "${scalar_write}" [=[    if (bitsize == 32 || bitsize == 64) {
        V(bitsize, vec, ir.ZeroExtendToQuad(value));
    } else {
        ir.SetQ(vec, ir.ZeroExtendToQuad(value));
    }]=] scalar_source "${scalar_source}")
write_derived("${PORT_BUILD_DIR}/scalar_impl.cpp" "${scalar_source}")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${translator_relative}/a64_translate.cpp" translator)
string(REPLACE "${translator_relative}/impl/impl.cpp" "${PORT_BUILD_DIR}/scalar_impl.cpp" translator "${translator}")
write_derived("${PORT_BUILD_DIR}/a64_translate.cpp" "${translator}")
get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "(^|/)a64_translate\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/a64_translate.cpp")

# Decoder tables: upstream fills 4096 lists and, for each list, parses the bit string of every
# instruction pattern again (~640 for A64, ~260 for A32): about half a second in the first block
# a game compiles, on the guest core that asked for it. Parse every pattern once, then fill the
# lists from that, in the same order (tools/check-decoder-startup.py).
foreach(decoder IN ITEMS "A64/decoder/a64" "A32/decoder/arm")
    set(decoder_source "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/frontend/${decoder}")
    file(READ "${decoder_source}.h" decoder_header)
    get_filename_component(decoder_name "${decoder}" NAME)
    set(decoder_loop "        for (size_t i = 0; i < t.size(); ++i) {\n#define INST")
    set(decoder_match "        if ((i & make_fast_index(mask)) == make_fast_index(expect)) { \\\n            t[i].emplace_back(")
    set(decoder_end "#include \"./${decoder_name}.inc\"\n#undef INST\n        }\n        return t;")
    foreach(anchor IN ITEMS decoder_loop decoder_match decoder_end)
        string(FIND "${decoder_header}" "${${anchor}}" offset)
        if(offset LESS 0)
            message(FATAL_ERROR "Pinned ${decoder_name} decoder table changed (${anchor})")
        endif()
    endforeach()
    string(REPLACE "${decoder_loop}" "        std::vector<Handler> all;\n        {\n#define INST" decoder_header "${decoder_header}")
    string(REPLACE "${decoder_match}" "        { \\\n            all.emplace_back(" decoder_header "${decoder_header}")
    string(REPLACE "${decoder_end}" "#include \"${decoder_source}.inc\"\n#undef INST\n        }\n        for (size_t i = 0; i < t.size(); ++i)\n            for (auto const& e : all)\n                if ((i & make_fast_index(e.mask)) == make_fast_index(e.expect))\n                    t[i].push_back(e);\n        return t;" decoder_header "${decoder_header}")
    # The other use of the pattern list in the header (the instruction's name).
    string(REPLACE "#include \"./${decoder_name}.inc\"" "#include \"${decoder_source}.inc\"" decoder_header "${decoder_header}")
    write_derived("${PORT_BUILD_DIR}/include/dynarmic/frontend/${decoder}.h" "${decoder_header}")
endforeach()
# The override headers shadow upstream's only by include order: a definition on the target
# recompiles every dynarmic object that saw the upstream ones.
target_compile_definitions(dynarmic PRIVATE EDEN_DECODER_ONE_PARSE=1)

# Cancel low-lane extraction of a scalar that was just zero-extended to Q.
# Preserve other lanes, arbitrary vectors and all floating-point operations.
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/ir/opt_passes.cpp" optimizer)
set(fold_anchor "        case Op::LeastSignificantWord:\n            FoldLeastSignificantWord(inst);\n            break;")
string(FIND "${optimizer}" "${fold_anchor}" fold_offset)
if(fold_offset LESS 0)
    message(FATAL_ERROR "Pinned scalar folding boundary changed")
endif()
file(READ "${EDEN_PORT_DIR}/scalar-fold.inc" scalar_fold)
string(REPLACE "${fold_anchor}" "${fold_anchor}\n${scalar_fold}" optimizer "${optimizer}")
write_derived("${PORT_BUILD_DIR}/opt_passes.cpp" "${optimizer}")
get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "(^|/)opt_passes\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/opt_passes.cpp")

# Avoid a 64-bit mask load for Eden's packed pointer in the checked page-table path. Apply to
# both A32 and A64 emitters.
set(memory_relative "dynarmic/backend/x64/emit_x64_memory.h")
file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${memory_relative}" memory_emitter)
set(pointer_mask "    // mask away attributes\n    if (ctx.conf.page_table_pointer_mask == 0) {")
# The A32 and the A64 lookup end their own sign extension differently; each closes the else.
set(pointer_end_a32 "        code.sar(page, 63 - int(*ctx.conf.page_table_sign_extension));\n    }\n")
set(pointer_end_a64 "        code.sar(page, *ctx.conf.page_table_sign_extension);\n    }\n")
foreach(anchor IN ITEMS pointer_mask pointer_end_a32 pointer_end_a64)
    string(REPLACE "${${anchor}}" "" without "${memory_emitter}")
    string(LENGTH "${memory_emitter}" before)
    string(LENGTH "${without}" after)
    string(LENGTH "${${anchor}}" each)
    math(EXPR found "(${before} - ${after}) / ${each}")
    if(anchor STREQUAL "pointer_mask")
        set(expected 2)
    else()
        set(expected 1)
    endif()
    if(NOT found EQUAL expected)
        message(FATAL_ERROR "Pinned page-table pointer decoding changed: ${found} of ${anchor}, expected ${expected}")
    endif()
endforeach()
string(REPLACE "${pointer_mask}" [=[    // Eden packs pointer bits 12..55. Sign extension removes the upper
    // attributes first, leaving a page mask that fits an x86 immediate.
    if (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL &&
        ctx.conf.page_table_sign_extension == 8) {
        code.shl(page, 8);
        code.sar(page, 8);
        code.and_(page, -4096);
    } else {
    // mask away attributes
    if (ctx.conf.page_table_pointer_mask == 0) {]=] memory_emitter "${memory_emitter}")
foreach(anchor IN ITEMS pointer_end_a32 pointer_end_a64)
    string(REPLACE "${${anchor}}" "${${anchor}}    }\n" memory_emitter "${memory_emitter}")
endforeach()
# The packed-pointer decoder above uses only immediates. Like upstream's
# unmasked absolute-offset path, it can reuse the index register for the page.
# Relative offsets and other pointer formats still need two scratch registers.
set(reuse_index "ctx.conf.absolute_offset_page_table && ctx.conf.page_table_pointer_mask == 0 ? page")
string(FIND "${memory_emitter}" "${reuse_index}" reuse_offset)
if(reuse_offset LESS 0)
    message(FATAL_ERROR "Pinned page-table scratch allocation changed")
endif()
string(REPLACE "${reuse_index}"
    "ctx.conf.absolute_offset_page_table && (ctx.conf.page_table_pointer_mask == 0 || (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL && ctx.conf.page_table_sign_extension == 8)) ? page"
    memory_emitter "${memory_emitter}")
# Eden's mark is bit zero: use a normal test/branch instead of extracting carry.
# Keep the upstream bit-test path for every other configured mark position.
set(mark_test "        code.bt(page, *ctx.conf.page_table_marked_bit);\n        code.jc(abort, code.T_NEAR);")
string(FIND "${memory_emitter}" "${mark_test}" mark_offset)
if(mark_offset LESS 0)
    message(FATAL_ERROR "Pinned page-table mark check changed")
endif()
string(REPLACE "${mark_test}" [=[        if (*ctx.conf.page_table_marked_bit == 0) {
            code.test(page.cvt32(), 1);
            code.jnz(abort, code.T_NEAR);
        } else {
            code.bt(page, *ctx.conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }]=] memory_emitter "${memory_emitter}")
# Retain the ordinary page-table lookup on window misses. Reserve scratch
    # registers before branching: allocator spills must run on both paths.
    set(lookup_args "Xbyak::Label& abort, Xbyak::Reg64 vaddr)")
    string(REPLACE "${lookup_args};"
        "Xbyak::Label& abort, Xbyak::Reg64 vaddr, std::optional<std::pair<Xbyak::Reg64, Xbyak::Reg64>> scratch = std::nullopt);"
        memory_emitter "${memory_emitter}")
    string(REPLACE "${lookup_args} {"
        "Xbyak::Label& abort, Xbyak::Reg64 vaddr, std::optional<std::pair<Xbyak::Reg64, Xbyak::Reg64>> scratch) {"
        memory_emitter "${memory_emitter}")
    string(REPLACE "const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);"
        "const Xbyak::Reg64 page = scratch ? scratch->first : ctx.reg_alloc.ScratchGpr(code);"
        memory_emitter "${memory_emitter}")
    string(REPLACE "const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table"
        "const Xbyak::Reg64 tmp = scratch ? scratch->second : ctx.conf.absolute_offset_page_table"
        memory_emitter "${memory_emitter}")
    set(memory_inc "dynarmic/backend/x64/emit_x64_memory.cpp.inc")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/${memory_inc}" memory_source)
    # Exclusive operations already allocated these two registers before their
    # monitor/early-exit branches. Never introduce a conditional allocator spill.
    string(FIND "${memory_source}" "void AxxEmitX64::EmitExclusiveReadMemoryInline" exclusive_start)
    string(SUBSTRING "${memory_source}" 0 ${exclusive_start} memory_prefix)
    string(SUBSTRING "${memory_source}" ${exclusive_start} -1 memory_exclusive)
    # Keep upstream's monitor and compare/exchange implementation, but allow
    # checked page-table addresses when direct fastmem is unavailable.
    string(REPLACE "ASSERT(conf.global_monitor && conf.fastmem_pointer);"
        "ASSERT(conf.global_monitor && (conf.fastmem_pointer || conf.page_table));"
        memory_exclusive "${memory_exclusive}")
    string(REPLACE "if (!exception_handler.SupportsFastmem())"
        "if (!conf.page_table && !exception_handler.SupportsFastmem())"
        memory_exclusive "${memory_exclusive}")
    string(REPLACE "if (fastmem_marker) {" "if (fastmem_marker || conf.page_table) {"
        memory_exclusive "${memory_exclusive}")
    string(REPLACE "bool require_abort_handling = false;" "bool require_abort_handling = !fastmem_marker;"
        memory_exclusive "${memory_exclusive}")
    foreach(arguments IN ITEMS "" ", tmp")
        set(old "= EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling${arguments});")
        string(FIND "${memory_exclusive}" "${old}" site)
        if(site LESS 0)
            message(FATAL_ERROR "Pinned exclusive lookup changed")
        endif()
        string(REPLACE "${old}"
            "= fastmem_marker ? EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling${arguments}) : EmitExclusiveVAddrLookup(code, ctx, bitsize, *abort, vaddr, tmp2, tmp);"
            memory_exclusive "${memory_exclusive}")
    endforeach()
    string(REPLACE "fastmem_patch_info.emplace(" "if (fastmem_marker) fastmem_patch_info.emplace("
        memory_exclusive "${memory_exclusive}")
    set(memory_source "${memory_prefix}${memory_exclusive}")
    include("${EDEN_PORT_DIR}/scalar-memory.cmake")
    if(EDEN_A32_FASTMEM)
        include("${EDEN_PORT_DIR}/checked-fastmem.cmake")
    endif()
    if(EDEN_SHARED_JIT)
        eden_shared_jit_memory_inc()
    endif()
    write_derived("${PORT_BUILD_DIR}/include/${memory_inc}" "${memory_source}")
set(exclusive_lookup [=[template<typename EmitContext>
Xbyak::RegExp EmitExclusiveVAddrLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize,
        Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page, Xbyak::Reg64 tmp) {
    if (bitsize > 8) {
        code.test(vaddr, u32(bitsize / 8 - 1));
        code.jnz(abort, code.T_NEAR);
    }
    // Natural alignment excludes page crossings, including CMPXCHG16B's
    // stricter alignment requirement. Skip the duplicate generic check.
    return EmitVAddrLookup(code, ctx, 8, abort, vaddr, std::make_optional(std::pair{page, tmp}));
}

]=])
set(fastmem_declaration "template<typename EmitContext>\nXbyak::RegExp EmitFastmemVAddr")
string(REPLACE "${fastmem_declaration}" "${exclusive_lookup}${fastmem_declaration}"
    memory_emitter "${memory_emitter}")
if(EDEN_SHARED_JIT)
    eden_shared_jit_memory_header()
endif()
write_derived("${PORT_BUILD_DIR}/include/${memory_relative}" "${memory_emitter}")
# New include overlays do not invalidate objects whose depfile still names
# upstream headers. Derive both translation units so the first build cannot
# silently keep old objects; subsequent depfiles track the overlay normally.
get_target_property(jit_sources dynarmic SOURCES)
foreach(bits 32 64)
    set(memory_unit "a${bits}_emit_x64_memory.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/${memory_unit}" memory_source)
    if(EDEN_SHARED_JIT)
        eden_shared_jit_memory_unit(${bits})
    endif()
    write_derived("${PORT_BUILD_DIR}/${memory_unit}" "${memory_source}")
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)a${bits}_emit_x64_memory\\.cpp$")
    list(APPEND jit_sources "${PORT_BUILD_DIR}/${memory_unit}")
endforeach()
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")


if(PS5_NATIVE)
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/common/spin_lock_x64.cpp" spin_source)
    string(FIND "${spin_source}" "namespace {\nstruct SpinLockImpl" spin_offset)
    if(spin_offset LESS 0)
        message(FATAL_ERROR "Pinned JIT monitor lock implementation changed")
    endif()
    string(SUBSTRING "${spin_source}" 0 ${spin_offset} spin_source)
    file(READ "${EDEN_PORT_DIR}/spin-lock.inc" spin_methods)
    write_derived("${PORT_BUILD_DIR}/spin_lock_x64.cpp" "${spin_source}${spin_methods}\n} // namespace Dynarmic\n")
    get_target_property(jit_sources dynarmic SOURCES)
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)spin_lock_x64\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/spin_lock_x64.cpp")
endif()

# Diagnostics record actual automatic cache evacuation, not explicit clears.
    set(jit_interface "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/a64_interface.cpp")
    file(READ "${jit_interface}" jit_source)
    if(PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(compile_begin "        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };")
        set(compile_end "        return emitter.Emit(ir_block).entrypoint;")
        string(FIND "${jit_source}" "${compile_begin}" begin)
        string(FIND "${jit_source}" "${compile_end}" end)
        if(begin LESS 0 OR end LESS begin)
            message(FATAL_ERROR "Pinned JIT compilation boundary changed")
        endif()
        string(LENGTH "${compile_end}" end_length)
        math(EXPR end "${end} + ${end_length}")
        string(SUBSTRING "${jit_source}" 0 ${begin} prefix)
        string(SUBSTRING "${jit_source}" ${end} -1 suffix)
        file(READ "${EDEN_PORT_DIR}/jit-compile-batch.inc" compile_batch)
        set(jit_source "${prefix}${compile_batch}${suffix}")
    endif()
    set(pressure_site "            // Immediately evacuate cache")
    string(FIND "${jit_source}" "${pressure_site}" pressure_offset)
    if(pressure_offset LESS 0)
        message(FATAL_ERROR "Pinned JIT cache evacuation changed.")
    endif()
    set(pressure_log "            std::fprintf(stderr, \"EDEN_JIT_PRESSURE core=%u capacity=%u remaining=%zu\\n\", unsigned(conf.processor_id), unsigned(conf.code_cache_size), block_of_code.SpaceRemaining());")
    if(PS5_NATIVE)
        # Every whole-cache evacuation (first 200 per core, with the monotonic time the
        # performance reports use) and a counter for them (eden_jit_evacuation).
        set(pressure_log "            static thread_local unsigned pressure_count;\n            if (++pressure_count <= 200) std::printf(\"EDEN_JIT_PRESSURE core=%u capacity=%u remaining=%zu count=%u mono_ns=%lld\\n\", unsigned(conf.processor_id), unsigned(conf.code_cache_size), block_of_code.SpaceRemaining(), pressure_count, static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));\n            if (eden_jit_evacuation) eden_jit_evacuation(unsigned(conf.processor_id));")
    endif()
    string(REPLACE "${pressure_site}"
        "${pressure_log}\n${pressure_site}"
        jit_source "${jit_source}")
    if(PS5_NATIVE AND EDEN_DEV_PROFILE)
        # Development: time every block compilation (cache misses) per guest core.
        set(compile_site "        if (auto block = emitter.GetBasicBlock(descriptor))\n            return block->entrypoint;\n")
        string(FIND "${jit_source}" "${compile_site}" compile_site_at)
        if(compile_site_at LESS 0)
            message(FATAL_ERROR "Pinned JIT block lookup changed.")
        endif()
        string(REPLACE "${compile_site}" "${compile_site}        struct EdenCompileTimer {\n            unsigned core;\n            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();\n            ~EdenCompileTimer() {\n                if (eden_jit_compile)\n                    eden_jit_compile(core, static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));\n            }\n        } eden_compile_timer{unsigned(conf.processor_id)};\n" jit_source "${jit_source}")
        # Development: the compile phases of every block (eden_jit_phases).
        set(phase_old "            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});\n            Optimization::Optimize(ir_block, conf, polyfill_options);\n            const auto entrypoint = emitter.Emit(ir_block, false).entrypoint;")
        string(FIND "${jit_source}" "${phase_old}" phase_at)
        if(phase_at LESS 0)
            message(FATAL_ERROR "Pinned JIT compile batch changed.")
        endif()
        string(REPLACE "${phase_old}" "            const auto eden_t0 = std::chrono::steady_clock::now();\n            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});\n            const auto eden_t1 = std::chrono::steady_clock::now();\n            Optimization::Optimize(ir_block, conf, polyfill_options);\n            const auto eden_t2 = std::chrono::steady_clock::now();\n            const auto entrypoint = emitter.Emit(ir_block, false).entrypoint;\n            if (eden_jit_phases) {\n                const auto eden_ns = [](auto a, auto b) { return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); };\n                eden_jit_phases(unsigned(conf.processor_id), eden_ns(eden_t0, eden_t1), eden_ns(eden_t1, eden_t2), eden_ns(eden_t2, std::chrono::steady_clock::now()), descriptor.Value());\n            }" jit_source "${jit_source}")
        string(PREPEND jit_source "extern \"C\" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize, unsigned long long emit, unsigned long long location) __attribute__((weak));\n")
    endif()
    if(EDEN_SHARED_JIT)
        eden_shared_jit_interface()
    endif()
    write_derived("${PORT_BUILD_DIR}/a64_interface.cpp" "#include <chrono>\n#include <cstdio>\nextern \"C\" void eden_jit_evacuation(unsigned core) __attribute__((weak));\nextern \"C\" void eden_jit_compile(unsigned core, unsigned long long ns) __attribute__((weak));\n${jit_source}")
    get_target_property(jit_sources dynarmic SOURCES)
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)a64_interface\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/a64_interface.cpp")

if(PS5_NATIVE)
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp" a32_interface)
    string(REPLACE "            invalidate_entire_cache = true;"
        "            static thread_local unsigned pressure_count;\n            if (++pressure_count <= 3) std::printf(\"EDEN_JIT_PRESSURE bits=32 core=%u capacity=%u remaining=%zu\\n\", unsigned(conf.processor_id), unsigned(conf.code_cache_size), block_of_code.SpaceRemaining());\n            invalidate_entire_cache = true;"
        a32_interface "${a32_interface}")
    set(a32_emit "        return emitter.Emit(ir_block);\n    }")
    string(FIND "${a32_interface}" "${a32_emit}" a32_emit_at)
    if(a32_emit_at LESS 0)
        message(FATAL_ERROR "Pinned A32 block emission changed")
    endif()
    string(REPLACE "${a32_emit}"
        "        const auto emitted = emitter.Emit(ir_block);\n        if (eden_jit_block)\n            eden_jit_block(unsigned(conf.processor_id), descriptor.Value(), reinterpret_cast<const void*>(emitted.entrypoint), emitted.size);\n        return emitted;\n    }"
        a32_interface "${a32_interface}")
    write_derived("${PORT_BUILD_DIR}/a32_interface.cpp" "#include <cstdio>\nextern \"C\" void eden_jit_block(unsigned core, unsigned long long location, const void* entry, unsigned long long size) __attribute__((weak));\n${a32_interface}")
    get_target_property(jit_sources dynarmic SOURCES)
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)a32_interface\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/a32_interface.cpp")
endif()

# Carry both qualified JIT configuration changes into the real core wrappers.
# Generate local derivatives; the pinned dependency itself remains unchanged.
get_target_property(core_sources core SOURCES)
foreach(bits 32 64)
    set(relative "arm/dynarmic/arm_dynarmic_${bits}.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/${relative}" wrapper)
    string(REPLACE "Common::PageTable::SIGN_BIT" "std::countl_zero(Common::PageTable::ATTRIBUTE_MASK)" wrapper "${wrapper}")
    # Without fastmem every LDREX/STREX left JIT code through a callback under the
    # global monitor lock (~7,000 pairs/frame in a measured racing game). The exclusive
    # emitter patch below supports page-table addresses; eden-memory-check covers
    # it for A64 and A32 (atomic loops, 1-4 cores).
    option(EDEN_INLINE_EXCLUSIVES "Inline page-table exclusive accesses" ON)
    if(EDEN_INLINE_EXCLUSIVES)
        foreach(spacing "  " " ")
            string(REPLACE "config.fastmem_exclusive_access = config.fastmem_pointer${spacing}!= std::nullopt;"
                "config.fastmem_exclusive_access = true;" wrapper "${wrapper}")
        endforeach()
        string(FIND "${wrapper}" "config.fastmem_exclusive_access = true;" inline_exclusive_at)
        if(inline_exclusive_at LESS 0)
            message(FATAL_ERROR "Pinned exclusive-access configuration changed")
        endif()
    endif()
      string(REPLACE "std::uint32_t(512_MiB)" "std::uint32_t(16_MiB)" wrapper "${wrapper}")
      # Guest cores 0-2 share a 640 MiB code budget. A64: 256/192/192, since a large
      # open-world game's workers filled 64 MiB on cores 1/2 and evacuated whole caches
      # while streaming. A32 keeps the qualified 512/64/64.
      # Only the active architecture allocates these caches; core3/host stay 16 MiB.
      if(PS5_NATIVE)
          if(bits EQUAL 64)
              set(jit_cache_sizes "m_core_index == 0 ? 256_MiB : m_core_index < 3 ? 192_MiB : 16_MiB")
          else()
              set(jit_cache_sizes "m_core_index == 0 ? 512_MiB : m_core_index < 3 ? 64_MiB : 16_MiB")
          endif()
          string(REPLACE "std::uint32_t(16_MiB)" "std::uint32_t(${jit_cache_sizes})" wrapper "${wrapper}")
      endif()
    set(derived "${PORT_BUILD_DIR}/arm_dynarmic_${bits}.cpp")
    if(EDEN_DEV_PROFILE)
        string(PREPEND wrapper "#include \"${EDEN_PORT_DIR}/performance.h\"\n")
        if(bits EQUAL 64)
            set(sample_pc "m_jit->GetPC()")
            set(sample_fp "m_jit->GetFpcr()")
        else()
            set(sample_pc "m_jit->Regs()[15]")
            set(sample_fp "m_jit->Fpscr()")
        endif()
        string(REPLACE "    return TranslateHaltReason(m_jit->Run());"
            "    const auto halt = m_jit->Run();\n    ::Eden::Performance::SampleCpu(m_core_index, 0, ${sample_pc}, GetSvcNumber(), ${sample_fp});\n    return TranslateHaltReason(halt);"
            wrapper "${wrapper}")
        if(bits EQUAL 32)
            foreach(counted IN ITEMS "u64 DynarmicCallbacks32::MemoryRead(u32 vaddr, size_t size) {|reads"
                    "void DynarmicCallbacks32::MemoryWrite(Dynarmic::A32::VAddr vaddr, u64 value, size_t size) {|writes")
                string(REPLACE "|" ";" counted_parts "${counted}")
                list(GET counted_parts 0 counted_head)
                list(GET counted_parts 1 counted_name)
                string(FIND "${wrapper}" "${counted_head}" counted_at)
                if(counted_at LESS 0)
                    message(FATAL_ERROR "Pinned A32 callback changed: ${counted_head}")
                endif()
                string(REPLACE "${counted_head}"
                    "${counted_head}\n    ::Eden::Performance::CountJit(::Eden::Performance::jit_callbacks[m_parent.m_core_index].${counted_name});"
                    wrapper "${wrapper}")
            endforeach()
            string(REPLACE "bool DynarmicCallbacks32::MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) {"
                "bool DynarmicCallbacks32::MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) {\n    ::Eden::Performance::CountJit(::Eden::Performance::jit_callbacks[m_parent.m_core_index].exclusive_writes);"
                wrapper "${wrapper}")
            string(REPLACE "bool DynarmicCallbacks32::MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) {"
                "bool DynarmicCallbacks32::MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) {\n    ::Eden::Performance::CountJit(::Eden::Performance::jit_callbacks[m_parent.m_core_index].exclusive_writes);"
                wrapper "${wrapper}")
            string(REPLACE "bool DynarmicCallbacks32::MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) {"
                "bool DynarmicCallbacks32::MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) {\n    ::Eden::Performance::CountJit(::Eden::Performance::jit_callbacks[m_parent.m_core_index].exclusive_writes);"
                wrapper "${wrapper}")
            string(REPLACE "bool DynarmicCallbacks32::MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) {"
                "bool DynarmicCallbacks32::MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) {\n    ::Eden::Performance::CountJit(::Eden::Performance::jit_callbacks[m_parent.m_core_index].exclusive_writes);"
                wrapper "${wrapper}")
        endif()
        string(REPLACE "    m_jit.emplace(config);"
            "    std::printf(\"EDEN_DEV_JIT bits=${bits} core=%zu cache=%u\\n\", m_core_index, config.code_cache_size);\n    m_jit.emplace(config);"
            wrapper "${wrapper}")
        if(bits EQUAL 64)
            # Guest calls through null pointers (seen in a boot race):
            # dump the guest code around the return chain for offline disassembly.
            set(no_execute "        LOG_CRITICAL(Core_ARM, \"Cannot execute instruction at unmapped address {:#016x}\", pc);\n")
            string(FIND "${wrapper}" "${no_execute}" no_execute_at)
            if(no_execute_at LESS 0)
                message(FATAL_ERROR "Pinned no-execute fault handler changed")
            endif()
            string(REPLACE "${no_execute}" "${no_execute}        ::Eden::CrashSite::Dump(m_memory, *m_parent.m_jit);\n        ::Eden::CrashSite::DumpThreads(\"fault\", *m_process);\n"
                wrapper "${wrapper}")
            string(PREPEND wrapper "#include \"${EDEN_PORT_DIR}/crash_site.h\"\n")
            # dev-settings watch=: writes into the watched range reach these callbacks (its
            # pages are marked debug) and are reported with the guest thread (watch.h).
            set(watch_context "m_memory, *m_parent.m_jit, Kernel::GetCurrentThread(m_parent.m_system.Kernel()), m_parent.m_core_index")
            foreach(write IN ITEMS "MemoryWrite(Dynarmic::A64::VAddr vaddr, u64 value, std::size_t size)|size|value"
                    "MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value)|16|value[0]"
                    "MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected)|1|value"
                    "MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected)|2|value"
                    "MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected)|4|value"
                    "MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected)|8|value"
                    "MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected)|16|value[0]")
                string(REPLACE "|" ";" write_parts "${write}")
                list(GET write_parts 0 write_signature)
                list(GET write_parts 1 write_size)
                list(GET write_parts 2 write_value)
                set(write_head "DynarmicCallbacks64::${write_signature} {\n")
                string(FIND "${wrapper}" "${write_head}" write_at)
                if(write_at LESS 0)
                    message(FATAL_ERROR "Pinned A64 write callback changed: ${write_signature}")
                endif()
                string(REPLACE "${write_head}"
                    "${write_head}    if (::Eden::Watch::Hit(vaddr, ${write_size})) ::Eden::Watch::Report(vaddr, ${write_size}, u64(${write_value}), ${watch_context});\n"
                    wrapper "${wrapper}")
            endforeach()
            string(PREPEND wrapper "#include \"${EDEN_PORT_DIR}/watch.h\"\n#include \"core/hle/kernel/k_thread.h\"\n#include \"core/hle/kernel/kernel.h\"\n")
        endif()
    endif()
    write_derived("${derived}" "#include <bit>\n${wrapper}")
    list(REMOVE_ITEM core_sources "${relative}")
    list(APPEND core_sources "${derived}")
endforeach()
# A prefetch abort (guest jump to an unmapped address) parks the thread for a debugger; with
# none attached, record it so the frontend ends the session instead of showing a black screen
# forever (headless/guest_fault.h).
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/physical_core.cpp" physical_core_source)
set(prefetch_old "            } else {\n                interface->LogBacktrace(process);\n            }\n            thread->RequestSuspend(kernel, SuspendType::Debug);")
string(FIND "${physical_core_source}" "${prefetch_old}" prefetch_at)
if(prefetch_at LESS 0)
    message(FATAL_ERROR "Pinned prefetch-abort handling changed")
endif()
# Guest core idle: count each wait and how long it lasted, and spin on the
# interrupt flag for a bounded time before sleeping on the condition variable, because waking a
# sleeping host thread puts the host scheduler's latency on the guest's critical path.
set(idle_old "void PhysicalCore::Idle() {\n    std::unique_lock lk{m_guard};\n    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });\n}")
string(FIND "${physical_core_source}" "${idle_old}" idle_at)
if(idle_at LESS 0)
    message(FATAL_ERROR "Pinned PhysicalCore::Idle changed")
endif()
string(REPLACE "${idle_old}" [=[void PhysicalCore::Idle() {
    const long long start = ::Eden::Performance::NowNs();
    const unsigned spins = ::Eden::Performance::idle_spin_iterations.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < spins && !__atomic_load_n(&m_is_interrupted, __ATOMIC_ACQUIRE); ++i)
        __builtin_ia32_pause();
    bool slept;
    {
        std::unique_lock lk{m_guard};
        slept = !m_is_interrupted;
        m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
    }
    ::Eden::Performance::CountIdle(m_core_index, ::Eden::Performance::NowNs() - start, slept);
}]=] physical_core_source "${physical_core_source}")
string(REPLACE "${prefetch_old}"
    "            } else {\n                interface->LogBacktrace(process);\n                if (prefetch_abort)\n                    ::Eden::RecordGuestFault(\"guest thread \" + Core::GetThreadName(thread).value_or(\"?\") +\n                                             \" jumped to an unmapped address\");\n            }\n            thread->RequestSuspend(kernel, SuspendType::Debug);"
    physical_core_source "${physical_core_source}")
write_derived("${PORT_BUILD_DIR}/physical_core.cpp"
    "#include \"${EDEN_PORT_DIR}/guest_fault.h\"\n#include \"${EDEN_PORT_DIR}/performance.h\"\n#include \"core/arm/debug.h\"\n${physical_core_source}")
list(REMOVE_ITEM core_sources "hle/kernel/physical_core.cpp")
list(APPEND core_sources "${PORT_BUILD_DIR}/physical_core.cpp")
if(EDEN_DEV_PROFILE)
    # Guest-visible waits: syncpoint event registration to signal (GPU-pipeline latency
    # the game waits on) and buffer-queue dequeue waits (presentation back-pressure).
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/nvdrv/devices/nvhost_ctrl.cpp" ctrl_source)
    set(ctrl_old "    event.wait_handle =\n        host1x_syncpoint_manager.RegisterHostAction(fence_id, target_value, [this, slot]() {\n            auto& event_ = events[slot];")
    string(FIND "${ctrl_source}" "${ctrl_old}" ctrl_at)
    if(ctrl_at LESS 0)
        message(FATAL_ERROR "Pinned nvhost_ctrl event wait changed")
    endif()
    string(REPLACE "${ctrl_old}" "    const auto wait_start = ::Eden::Performance::NowNs();\n    event.wait_handle =\n        host1x_syncpoint_manager.RegisterHostAction(fence_id, target_value, [this, slot, wait_start]() {\n            ::Eden::Performance::AddSince(::Eden::Performance::guest_sync_wait, wait_start);\n            auto& event_ = events[slot];" ctrl_source "${ctrl_source}")
    write_derived("${PORT_BUILD_DIR}/nvhost_ctrl.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${ctrl_source}")
    list(REMOVE_ITEM core_sources "hle/service/nvdrv/devices/nvhost_ctrl.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/nvhost_ctrl.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/nvnflinger/buffer_queue_producer.cpp" producer_source)
    set(producer_old "        Status status = WaitForFreeSlotThenRelock(async, &found, &return_flags, lock);")
    string(FIND "${producer_source}" "${producer_old}" producer_at)
    if(producer_at LESS 0)
        message(FATAL_ERROR "Pinned buffer queue dequeue changed")
    endif()
    string(REPLACE "${producer_old}" "        const auto dequeue_start = ::Eden::Performance::NowNs();\n${producer_old}\n        ::Eden::Performance::AddSince(::Eden::Performance::guest_dequeue_wait, dequeue_start);" producer_source "${producer_source}")
    write_derived("${PORT_BUILD_DIR}/buffer_queue_producer.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${producer_source}")
    list(REMOVE_ITEM core_sources "hle/service/nvnflinger/buffer_queue_producer.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/buffer_queue_producer.cpp")
    # Guest asset streaming: time and bytes of fsp-srv IFile/IStorage backend reads.
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/filesystem/fsp/fs_i_file.cpp" fs_file_source)
    set(fs_file_old "    R_RETURN(\n        backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(), size));")
    string(FIND "${fs_file_source}" "${fs_file_old}" fs_file_at)
    if(fs_file_at LESS 0)
        message(FATAL_ERROR "Pinned IFile::Read changed")
    endif()
    string(REPLACE "${fs_file_old}" "    const auto fs_start = ::Eden::Performance::NowNs();\n    const Result fs_result =\n        backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(), size);\n    ::Eden::Performance::AddSince(::Eden::Performance::guest_fs_file, fs_start);\n    ::Eden::Performance::guest_fs_file_bytes.fetch_add(static_cast<unsigned long long>(*out_size.Get()), std::memory_order_relaxed);\n    // Few per boot (save data): every read with its result.\n    LOG_WARNING(Service_FS, \"EDEN_FS_FILE_READ offset={:#x} size={} read={} result={:#x} ns={}\", offset, size, *out_size.Get(), fs_result.raw, ::Eden::Performance::NowNs() - fs_start);\n    R_RETURN(fs_result);" fs_file_source "${fs_file_source}")
    write_derived("${PORT_BUILD_DIR}/fs_i_file.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${fs_file_source}")
    list(REMOVE_ITEM core_sources "hle/service/filesystem/fsp/fs_i_file.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/fs_i_file.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/filesystem/fsp/fs_i_storage.cpp" fs_storage_source)
    set(fs_storage_old "    backend->Read(out_bytes.data(), length, offset);")
    string(FIND "${fs_storage_source}" "${fs_storage_old}" fs_storage_at)
    if(fs_storage_at LESS 0)
        message(FATAL_ERROR "Pinned IStorage::Read changed")
    endif()
    # The pinned handler ignores the backend's byte count and reports success; report any
    # short storage read (the guest would get stale buffer bytes).
    string(REPLACE "${fs_storage_old}" "    const auto fs_start = ::Eden::Performance::NowNs();\n    const auto fs_read = backend->Read(out_bytes.data(), length, offset);\n    if (fs_read != static_cast<std::size_t>(length))\n        LOG_ERROR(Service_FS, \"EDEN_FS_SHORT_STORAGE offset={:#x} length={} read={} size={}\", offset, length, fs_read, backend->GetSize());\n    ::Eden::Performance::AddSince(::Eden::Performance::guest_fs_storage, fs_start);\n    ::Eden::Performance::guest_fs_storage_bytes.fetch_add(static_cast<unsigned long long>(length), std::memory_order_relaxed);" fs_storage_source "${fs_storage_source}")
    # Open/entry results for the few named-path requests (save data, BCAT, SD).
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/filesystem/fsp/fs_i_filesystem.cpp" fs_system_source)
    # (Statements carry semicolons, so no CMake lists here; quoted arguments keep them.)
    foreach(call_kind IN ITEMS file directory entry)
        if(call_kind STREQUAL "file")
            set(call_expression "backend->OpenFile(&vfs_file, FileSys::Path(path->str),\n                            static_cast<FileSys::OpenMode>(mode))")
        elseif(call_kind STREQUAL "directory")
            set(call_expression "backend->OpenDirectory(&vfs_dir, FileSys::Path(path->str),\n                                 static_cast<FileSys::OpenDirectoryMode>(mode))")
        else()
            set(call_expression "backend->GetEntryType(&vfs_entry_type, FileSys::Path(path->str))")
        endif()
        set(call_old "    R_TRY(${call_expression});")
        string(FIND "${fs_system_source}" "${call_old}" call_at)
        if(call_at LESS 0)
            message(FATAL_ERROR "Pinned IFileSystem ${call_kind} request changed")
        endif()
        string(REPLACE "${call_old}" "    const Result fs_open_result = ${call_expression};\n    LOG_WARNING(Service_FS, \"EDEN_FS_OPEN kind=${call_kind} path={} result={:#x}\", path->str, fs_open_result.raw);\n    R_TRY(fs_open_result);"
            fs_system_source "${fs_system_source}")
    endforeach()
    write_derived("${PORT_BUILD_DIR}/fs_i_filesystem.cpp" "${fs_system_source}")
    list(REMOVE_ITEM core_sources "hle/service/filesystem/fsp/fs_i_filesystem.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/fs_i_filesystem.cpp")
    write_derived("${PORT_BUILD_DIR}/fs_i_storage.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${fs_storage_source}")
    list(REMOVE_ITEM core_sources "hle/service/filesystem/fsp/fs_i_storage.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/fs_i_storage.cpp")
    # HLE service requests: handling time per service command, and the guest's
    # svcSendSyncRequest latency (queueing behind other requests included).
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/service.cpp" hle_source)
    foreach(hle_call "        InvokeRequest(ctx);" "            InvokeRequestTipc(ctx);")
        string(FIND "${hle_source}" "${hle_call}" hle_at)
        if(hle_at LESS 0)
            message(FATAL_ERROR "Pinned HLE request dispatch changed")
        endif()
        string(REPLACE "${hle_call}" "        {\n            const auto hle_start = ::Eden::Performance::NowNs();\n${hle_call}\n            ::Eden::Performance::RecordHle(service_name, ctx.GetCommand(), ::Eden::Performance::NowNs() - hle_start);\n        }" hle_source "${hle_source}")
    endforeach()
    # Named-path filesystem requests (save data): the requesting guest thread and its
    # return chain, from its context saved at svcSendSyncRequest (boot-race probe).
    string(REPLACE "            ::Eden::Performance::RecordHle(service_name, ctx.GetCommand(), ::Eden::Performance::NowNs() - hle_start);\n"
        "            ::Eden::Performance::RecordHle(service_name, ctx.GetCommand(), ::Eden::Performance::NowNs() - hle_start);\n            if (::Eden::Performance::trace_fs_callers.load(std::memory_order_relaxed) && std::string_view{service_name} == \"IFileSystem\") {\n                std::string frames;\n                for (const auto& entry : Core::GetBacktrace(&ctx.GetThread()))\n                    frames += fmt::format(\" {}+{:X}\", entry.module, entry.offset);\n                LOG_WARNING(Service_FS, \"EDEN_FS_CALLER command={} thread={} name={} frames={}\", ctx.GetCommand(),\n                            ctx.GetThread().GetThreadId(), Core::GetThreadName(&ctx.GetThread()).value_or(\"-\"), frames);\n            }\n"
        hle_source "${hle_source}")
    write_derived("${PORT_BUILD_DIR}/hle_service.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n#include \"core/arm/debug.h\"\n${hle_source}")
    list(REMOVE_ITEM core_sources "hle/service/service.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/hle_service.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/svc/svc_ipc.cpp" ipc_source)
    set(ipc_old "    R_RETURN(session->SendSyncRequest(kernel, message, buffer_size));")
    string(FIND "${ipc_source}" "${ipc_old}" ipc_at)
    if(ipc_at LESS 0)
        message(FATAL_ERROR "Pinned SendSyncRequest changed")
    endif()
    string(REPLACE "${ipc_old}" "    const auto ipc_start = ::Eden::Performance::NowNs();\n    const Result ipc_result = session->SendSyncRequest(kernel, message, buffer_size);\n    ::Eden::Performance::AddSince(::Eden::Performance::guest_ipc_wait, ipc_start);\n    R_RETURN(ipc_result);" ipc_source "${ipc_source}")
    write_derived("${PORT_BUILD_DIR}/svc_ipc.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${ipc_source}")
    list(REMOVE_ITEM core_sources "hle/kernel/svc/svc_ipc.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/svc_ipc.cpp")
endif()
if(EDEN_A32_FASTMEM)
    # The window spans 4 GiB: only 32-bit applications get it as their fastmem arena.
    file(READ "${PROJECT_SOURCE_DIR}/src/core/memory.cpp" arena_source)
    set(arena_old "        if (process.IsApplication() && Settings::IsFastmemEnabled()) {")
    string(FIND "${arena_source}" "${arena_old}" arena_at)
    if(arena_at LESS 0)
        message(FATAL_ERROR "Pinned fastmem arena selection changed")
    endif()
    string(REPLACE "${arena_old}" "        if (process.IsApplication() && !process.Is64Bit() && Settings::IsFastmemEnabled()) {"
        arena_source "${arena_source}")
    write_derived("${PORT_BUILD_DIR}/memory.cpp" "${arena_source}")
    list(REMOVE_ITEM core_sources "memory.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/memory.cpp")
    # Upstream places code at any 4 KiB offset below 16 MiB, which leaves three of four
    # layouts with every module page out of 16 KiB phase with its backing. Keep the same
    # range with 16 KiB steps so module chunks can alias.
    file(READ "${PROJECT_SOURCE_DIR}/src/core/loader/deconstructed_rom_directory.cpp" aslr_source)
    set(aslr_old "? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;")
    string(FIND "${aslr_source}" "${aslr_old}" aslr_at)
    if(aslr_at LESS 0)
        message(FATAL_ERROR "Pinned code ASLR offset changed")
    endif()
    string(REPLACE "${aslr_old}" "? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 14) & 0xffc000;"
        aslr_source "${aslr_source}")
    write_derived("${PORT_BUILD_DIR}/deconstructed_rom_directory.cpp" "${aslr_source}")
    list(REMOVE_ITEM core_sources "loader/deconstructed_rom_directory.cpp")
    list(APPEND core_sources "${PORT_BUILD_DIR}/deconstructed_rom_directory.cpp")
endif()
# GetPseudoDeviceId read the seed through the control data without checking that there is
# any (a game without control data, such as a homebrew NRO): seed 0 then, as for other
# NACP fields Eden defaults when the metadata is missing.
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/am/service/application_functions.cpp" app_functions_source)
set(app_functions_old "    auto const seed = res.first->raw.seed_for_pseudo_device_id;")
string(FIND "${app_functions_source}" "${app_functions_old}" app_functions_at)
if(app_functions_at LESS 0)
    message(FATAL_ERROR "Pinned GetPseudoDeviceId changed")
endif()
string(REPLACE "${app_functions_old}" "    auto const seed = res.first != nullptr ? res.first->raw.seed_for_pseudo_device_id : u64_le{0};" app_functions_source "${app_functions_source}")
write_derived("${PORT_BUILD_DIR}/application_functions.cpp" "${app_functions_source}")
list(REMOVE_ITEM core_sources "hle/service/am/service/application_functions.cpp")
list(APPEND core_sources "${PORT_BUILD_DIR}/application_functions.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")
# Core's NS query service owns this implementation from frontend_common.
target_sources(core PRIVATE "${PROJECT_SOURCE_DIR}/src/frontend_common/play_time_manager.cpp")

# Load() already resolves the ID into its per-load parameters. Reuse it for
# overrides instead of reading again into an uninitialized persistent field.
file(READ "${PROJECT_SOURCE_DIR}/src/core/core.cpp" system_source)
foreach(header game_settings.h launch_timestamp_cache.h)
    string(REPLACE "#include \"${header}\"" "#include \"core/${header}\"" system_source "${system_source}")
endforeach()
string(REPLACE "    u64 program_id;\n\n" "" system_source "${system_source}")
string(REPLACE "        if (app_loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {\n            LOG_ERROR(Core, \"Failed to find program id for ROM\");\n        }\n\n" "" system_source "${system_source}")
string(REPLACE "GameSettings::LoadOverrides(program_id," "GameSettings::LoadOverrides(params.program_id," system_source "${system_source}")
# Join callbacks before their service-owned targets can be destroyed.
set(old_audio_shutdown "        stop_event.request_stop();")
string(FIND "${system_source}" "${old_audio_shutdown}" audio_shutdown_offset)
if(audio_shutdown_offset LESS 0)
    message(FATAL_ERROR "Pinned service shutdown changed; review audio callback lifetime.")
endif()
string(REPLACE "${old_audio_shutdown}"
    "        if (audio_core) audio_core->Shutdown();\n${old_audio_shutdown}" system_source "${system_source}")
# The cheat engine's looping timing event reads HID through the service manager; it was
# destroyed after the services, and core timing runs again during their teardown
# (SyncPause(false)), so a game with cheats crashed on the HostTiming thread when stopped.
# Its destructor unschedules the event and waits for a callback in progress.
set(old_cheat_shutdown "        kernel.CloseServices();")
string(FIND "${system_source}" "${old_cheat_shutdown}" cheat_shutdown_offset)
if(cheat_shutdown_offset LESS 0)
    message(FATAL_ERROR "Pinned service shutdown changed; review cheat engine lifetime.")
endif()
string(REPLACE "${old_cheat_shutdown}"
    "        cheat_engine.reset();\n${old_cheat_shutdown}" system_source "${system_source}")
# Kernel intrusive lists refer into DeviceMemory even during failed Load unwind.
# Declare the backing first so reverse member destruction releases it last.
string(REPLACE "    std::optional<Core::DeviceMemory> device_memory;\n" "" system_source "${system_source}")
string(REPLACE "    Kernel::KernelCore kernel;"
    "    std::optional<Core::DeviceMemory> device_memory;\n    Kernel::KernelCore kernel;" system_source "${system_source}")
if(PS5_NATIVE)
    foreach(stage "gpu_core->NotifyShutdown();" "if (audio_core) audio_core->Shutdown();" "kernel.SuspendEmulation(true);" "kernel.CloseServices();" "kernel.ShutdownCores();" "services.reset();" "gpu_core.reset();" "cpu_manager.Shutdown();" "kernel.Shutdown();")
        string(REPLACE ";" "" stage_label "${stage}")
        string(REPLACE "${stage}" "${stage}\n        ::Eden::ReportStep(\"shutdown\", \"Completed ${stage_label}\");" system_source "${system_source}")
    endforeach()
    string(PREPEND system_source "#include \"${EDEN_PORT_DIR}/diagnostics.h\"\n")
endif()
# Failed Load still owns a Process wrapper whose destructor touches the kernel.
# Destroy it before kernel.Shutdown tears down its mutexes (both failure paths).
string(REPLACE "            ShutdownMainProcess();\n            return"
    "            process.reset();\n            ShutdownMainProcess();\n            return"
    system_source "${system_source}")
write_derived("${PORT_BUILD_DIR}/core.cpp" "${system_source}")
get_target_property(core_sources core SOURCES)
# The PS5 heap is bounded: avoid allocating/decompressing an executable merely
# to calculate its layout, and avoid vector growth duplicating a large BSS image.
if(PS5_NATIVE)
    file(READ "${PROJECT_SOURCE_DIR}/src/core/loader/nso.cpp" nso_source)
    set(old_nso_image "    Kernel::CodeSet codeset;\n    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);")
    set(new_nso_image [=[    const u64 initialized_size = module_start + u64(last_segment_it->location) + u64(last_segment_it->size);
    const u64 argument_size = should_pass_arguments && !Settings::values.program_args.GetValue().empty()
        ? NSO_ARGUMENT_DATA_ALLOCATION_SIZE : 0;
    const u64 final_size = (initialized_size + argument_size + u64(last_segment_it->bss_size) + 0xFFF) & ~u64(0xFFF);
    if (!final_size || final_size > UINT32_MAX) return std::nullopt;
    for (size_t i = 0; i < nso_header.segments.size(); ++i) {
        const auto& segment = nso_header.segments[i];
        const u64 stored_size = nso_header.IsSegmentCompressed(i)
            ? u64(nso_header.segments_compressed_size[i]) : u64(segment.size);
        if ((nso_header.IsSegmentCompressed(i) && (stored_size > INT32_MAX || u64(segment.size) > INT32_MAX)) ||
            u64(segment.offset) + stored_size > nso_file.GetSize() ||
            module_start + u64(segment.location) + u64(segment.size) > initialized_size)
            return std::nullopt;
    }
    // PS5 uses Dynarmic, not NCE: a layout query needs no executable bytes.
#ifdef HAS_NCE
#error Review PS5 NSO layout-only loading before enabling NCE
#endif
    if (!load_into_process) return load_base + final_size;
    std::fprintf(stderr, "[ProsperoEden] NSO %s image_bytes=%llu bss_bytes=%u\n",
                 nso_file.GetName().c_str(), static_cast<unsigned long long>(final_size),
                 static_cast<unsigned>(last_segment_it->bss_size));
    Kernel::CodeSet codeset;
    const auto name = nso_file.GetName();
    // A mod's patch changes code and data, which are here whole; only a dump wants the BSS too.
    const bool patched = pm && pm->HasNSOPatch(nso_header.build_id, name);
    const bool full_image = pm && Settings::values.dump_nso;
    // BSS belongs in guest memory, not a second almost-2-GiB heap buffer.
    codeset.memory.reserve(full_image ? final_size : initialized_size + argument_size);
    codeset.memory.resize(initialized_size);]=])
    string(FIND "${nso_source}" "${old_nso_image}" nso_offset)
    if(nso_offset LESS 0)
        message(FATAL_ERROR "Pinned NSO loader changed")
    endif()
    string(REPLACE "${old_nso_image}" "${new_nso_image}" nso_source "${nso_source}")
    string(REPLACE "    codeset.memory.resize(image_size);" "    if (full_image) codeset.memory.resize(image_size);" nso_source "${nso_source}")
    string(REPLACE "    const auto name = nso_file.GetName();\n    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {"
        "    if (patched || full_image) {" nso_source "${nso_source}")
    string(REPLACE "    // Load codeset for current process"
        "    // Initialize BSS and page padding directly, including reused guest pages.\n    if (codeset.memory.size() < image_size &&\n        !process.GetMemory().ZeroBlock(load_base + codeset.memory.size(), image_size - codeset.memory.size()))\n        return std::nullopt;\n\n    // Load codeset for current process" nso_source "${nso_source}")
    set(old_nso_decode [=[        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);]=])
    string(REPLACE "${old_nso_decode}" "" nso_source "${nso_source}")
    string(REPLACE "            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);"
        "            auto* destination = codeset.memory.data() + module_start + nso_header.segments[i].location;\n            if (!nso_header.IsSegmentCompressed(i)) {\n                if (nso_file.Read(destination, nso_header.segments[i].size, nso_header.segments[i].offset) != nso_header.segments[i].size) return std::nullopt;\n            } else if (nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset) != nso_header.segments_compressed_size[i]) return std::nullopt;"
        nso_source "${nso_source}")
    # A segment is decoded where it belongs (read there directly when it is not compressed), and
    # a segment that does not decode ends the load instead of the app.
    set(old_nso_segment [=[            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
]=])
    string(FIND "${nso_source}" "${old_nso_segment}" nso_segment_at)
    if(nso_segment_at LESS 0)
        message(FATAL_ERROR "Pinned NSO segment decoding changed")
    endif()
    string(REPLACE "${old_nso_segment}" [=[            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{destination, nso_header.segments[i].size},
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i]));
                    if (r <= 0) return std::nullopt;
                } else {
                    const int r = Common::Compression::DecompressDataLZ4(destination, nso_header.segments[i].size,
                        compressed_data.data(), nso_header.segments_compressed_size[i]);
                    if (r != int(nso_header.segments[i].size)) return std::nullopt;
                }
            }
]=] nso_source "${nso_source}")
    # Cheats address the game's main module. Eden registers a cheat list for every module that
    # has one, and the last registration wins: a cheat file without a build ID in its name
    # (mods/<title ID>/cheat_<name>.txt) matches every module, so its cheats were applied
    # against the last module loaded (sdk) and changed nothing in the game.
    set(old_nso_cheats [=[        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);]=])
    set(new_nso_cheats [=[        if (name == "main") {
            // The build ID a cheat file for this version of the game is named after.
            char build[17]{};
            for (int i = 0; i < 8; ++i)
                std::snprintf(build + i * 2, 3, "%02X", static_cast<unsigned>(nso_header.build_id[i]));
            std::fprintf(stderr, "[ProsperoEden] cheats: %zu entries for main (build ID %s) at %#llx (%llu bytes)\n",
                         cheats.size(), build, static_cast<unsigned long long>(load_base),
                         static_cast<unsigned long long>(image_size));
        }
        if (name == "main" && !cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);]=])
    string(FIND "${nso_source}" "${old_nso_cheats}" nso_cheats_offset)
    if(nso_cheats_offset LESS 0)
        message(FATAL_ERROR "Pinned NSO cheat registration changed")
    endif()
    string(REPLACE "${old_nso_cheats}" "${new_nso_cheats}" nso_source "${nso_source}")
    string(PREPEND nso_source "#include <cstdio>\n")
    write_derived("${PORT_BUILD_DIR}/nso.cpp" "${nso_source}")
    list(REMOVE_ITEM core_sources loader/nso.cpp)
    list(APPEND core_sources "${PORT_BUILD_DIR}/nso.cpp")
endif()
list(REMOVE_ITEM core_sources core.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/core.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# The builder's vectors own every context through serialization. Parent links
# only navigate that live tree; owning them creates parent/child cycles.
file(READ "${PROJECT_SOURCE_DIR}/src/core/file_sys/fsmitm_romfsbuild.cpp" romfs_source)
string(REPLACE "    std::shared_ptr<RomFSBuildDirectoryContext> parent;"
    "    RomFSBuildDirectoryContext* parent = nullptr;" romfs_source "${romfs_source}")
string(REPLACE "parent = std::move(parent_dir_ctx);" "parent = parent_dir_ctx.get();" romfs_source "${romfs_source}")
write_derived("${PORT_BUILD_DIR}/fsmitm_romfsbuild.cpp" "${romfs_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources file_sys/fsmitm_romfsbuild.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/fsmitm_romfsbuild.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# One change to a pinned Eden source; the build stops when the text it replaces is gone.
function(eden_patch source old new)
    string(FIND "${${source}}" "${old}" patch_at)
    if(patch_at LESS 0)
        message(FATAL_ERROR "Pinned Eden source changed; not found: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" patched "${${source}}")
    set(${source} "${patched}" PARENT_SCOPE)
endfunction()

# Mods (headless/mods.h). Eden's .pchtxt reader takes every line as a record or a flag: the
# credits at the end of a real patch file then wrote over the start of the game's code. Only
# records and known flags are read here (headless/mods_check.cpp checks all of it).
file(READ "${PROJECT_SOURCE_DIR}/src/core/file_sys/ips_layer.cpp" ips_source)
set(ips_escape_begin "static IPSwitchRecord EscapeStringSequences(std::string_view sv) {")
set(ips_escape_end "void IPSwitchCompiler::Parse(")
string(FIND "${ips_source}" "${ips_escape_begin}" ips_escape_at)
string(FIND "${ips_source}" "${ips_escape_end}" ips_parse_at)
if(ips_escape_at LESS 0 OR ips_parse_at LESS ips_escape_at)
    message(FATAL_ERROR "Pinned IPSwitch text parsing changed")
endif()
string(SUBSTRING "${ips_source}" 0 ${ips_escape_at} ips_before)
string(SUBSTRING "${ips_source}" ${ips_parse_at} -1 ips_after)
set(ips_escape [=[// A patch record is its offset in hex digits, blanks, then the value: an even number of hex digits or
// quoted text. Patch files also carry credits and links after their records; the reference tool
// skips those, and reading them as records wrote stray bytes over the start of the game's code.
static bool IsPatchRecord(std::string_view line) {
    std::size_t at = 0;
    while (at < line.size() && std::isxdigit(static_cast<unsigned char>(line[at]))) ++at;
    if (at == 0 || at > 16 || at == line.size() || (line[at] != ' ' && line[at] != '\t')) return false;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    if (at == line.size()) return false;
    if (line[at] == '"' || line[at] == '\'') return true;
    const std::size_t value = at;
    while (at < line.size() && std::isxdigit(static_cast<unsigned char>(line[at]))) ++at;
    return at == line.size() && (at - value) % 2 == 0;
}

// The flags a patch file may carry. An author's "@name" in the credits is not one: taken by its
// second letter alone it could end the file early, flip the byte order or replace the build ID.
static bool IsPatchFlag(std::string_view line) {
    static constexpr std::string_view kFlags[] = {"@nsobid-", "@stop", "@enabled", "@disabled", "@flag ",
                                                  "@little-endian", "@big-endian"};
    for (const std::string_view flag : kFlags) {
        bool same = line.size() >= flag.size();
        for (std::size_t i = 0; same && i < flag.size(); ++i)
            same = std::tolower(static_cast<unsigned char>(line[i])) == flag[i];
        if (same) return true;
    }
    return false;
}

static IPSwitchRecord EscapeStringSequences(std::string_view sv) {
    IPSwitchRecord r{};
    for (auto it = sv.cbegin(); it < sv.cend(); ) {
        if (*it == '\\' && it + 1 < sv.cend()) {
            switch (it[1]) {
            case 'a': r.data.push_back('\a'); break;
            case 'b': r.data.push_back('\b'); break;
            case 'e': r.data.push_back('\x1b'); break;
            case 'f': r.data.push_back('\f'); break;
            case 'n': r.data.push_back('\n'); break;
            case 'r': r.data.push_back('\r'); break;
            case 't': r.data.push_back('\t'); break;
            case 'v': r.data.push_back('\v'); break;
            default: r.data.push_back(static_cast<uint8_t>(it[1])); break;
            }
            it += 2;
        } else {
            r.data.push_back(static_cast<uint8_t>(*it));
            ++it;
        }
    }
    return r;
}

]=])
set(ips_source "${ips_before}${ips_escape}${ips_after}")
eden_patch(ips_source "        if (line.size() > 2 && line[0] == '@') {\n            switch (line[1]) {"
    "        if (line.size() > 2 && line[0] == '@') {\n            if (!IsPatchFlag(line)) return true;\n            switch (line[1]) {")
eden_patch(ips_source "        } else if (patches.empty()) {\n            LOG_WARNING(Loader, \"Invalid line not in a patch {}\", line);"
    "        } else if (!IsPatchRecord(line)) {\n            // Credits, links, anything that is not a record.\n        } else if (patches.empty()) {\n            LOG_WARNING(Loader, \"Invalid line not in a patch {}\", line);")
write_derived("${PORT_BUILD_DIR}/ips_layer.cpp" "${ips_source}")
# The patch manager: a missing mod root must never enter its sorting and dereference loops, and an
# .ips patch is named after the build ID in either case.
file(READ "${PROJECT_SOURCE_DIR}/src/core/file_sys/patch_manager.cpp" patch_source)
eden_patch(patch_source "std::vector<VirtualDir> patch_dirs = {sdmc_load_dir};"
    "std::vector<VirtualDir> patch_dirs;\n    if (sdmc_load_dir) patch_dirs.push_back(sdmc_load_dir);")
eden_patch(patch_source
    "    std::vector<VirtualDir> patch_dirs = load_dir->GetSubdirectories();\n    if (std::find(disabled.cbegin(), disabled.cend(), \"SDMC\") == disabled.cend()) {"
    "    std::vector<VirtualDir> patch_dirs;\n    if (load_dir != nullptr) patch_dirs = load_dir->GetSubdirectories();\n    if (sdmc_load_dir != nullptr && std::find(disabled.cbegin(), disabled.cend(), \"SDMC\") == disabled.cend()) {")
eden_patch(patch_source
    "const auto this_build_id = fmt::format(\"{:0<64}\", name.substr(0, name.find('.')));"
    "const auto this_build_id = fmt::format(\"{:0<64}\", Common::ToUpper(name.substr(0, name.find('.'))));")
# A game without a mods folder is the usual case here, not an error to log for each of its modules.
eden_patch(patch_source "        LOG_ERROR(Loader, \"Cannot load mods for invalid title_id={:016X}\", title_id);\n" "")
# Cheats chosen one by one (headless/cheats.h): each mod's cheat list, from its folder or from a
# loose cheat file, goes into the game's with the cheats that were not chosen switched off.
eden_patch(patch_source "std::copy(res->begin(), res->end(), std::back_inserter(out));"
    "Eden::Cheats::Append(out, *res, subdir->GetName());")
eden_patch(patch_source "std::copy(res.begin(), res.end(), std::back_inserter(out));"
    "Eden::Cheats::Append(out, res, name);")
string(PREPEND patch_source "#include \"${EDEN_PORT_DIR}/cheats.h\"\n")
write_derived("${PORT_BUILD_DIR}/patch_manager.cpp" "${patch_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources file_sys/ips_layer.cpp file_sys/patch_manager.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/ips_layer.cpp" "${PORT_BUILD_DIR}/patch_manager.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# The rate of the vsync clock a game runs on (60 Hz, or what an FPS patch asks for: 120, 240) is
# published for the renderer: frames a display has no refresh for are not presented
# (headless/display_refresh.h).
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/vi/conductor.cpp" conductor_source)
string(PREPEND conductor_source "#include \"${EDEN_PORT_DIR}/display_refresh.h\"\n")
eden_patch(conductor_source
    "    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);\n    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));"
    "    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);\n    ::Eden::Display::game_millihertz.store(static_cast<int>(effective_fps * 1000.f / speed_scale),\n                                           std::memory_order_relaxed);\n    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));")
write_derived("${PORT_BUILD_DIR}/vi_conductor.cpp" "${conductor_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources hle/service/vi/conductor.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/vi_conductor.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# Nvdrv sessions borrow process memory until their final handles are unmapped.
# Retain the process across that interval, including service shutdown ordering.
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/nvdrv/core/container.cpp" nvcontainer)
string(PREPEND nvcontainer "#include \"core/core.h\"\n#include <utility>\n")
string(REPLACE "    session.is_active = true;"
    "    process->Open(impl->host1x.System().Kernel()); // Paired with the final CloseSession after SMMU unregistration.\n    session.is_active = true;" nvcontainer "${nvcontainer}")
string(REPLACE "void Container::CloseSession(SessionId session_id) {\n    std::scoped_lock lk(impl->session_guard);"
    "void Container::CloseSession(SessionId session_id) {\n    std::unique_lock lk(impl->session_guard);" nvcontainer "${nvcontainer}")
string(REPLACE "    impl->id_pool.emplace_front(session_id.id);"
    "    impl->id_pool.emplace_front(session_id.id);\n    auto* process = std::exchange(session.process, nullptr);\n    lk.unlock(); // Process finalization may close other services.\n    process->Close(impl->host1x.System().Kernel());" nvcontainer "${nvcontainer}")
write_derived("${PORT_BUILD_DIR}/nvdrv_container.cpp" "${nvcontainer}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources hle/service/nvdrv/core/container.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/nvdrv_container.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")
# Finalize alone leaves both event references and its owner process open.
# Use the same paired close as the other HID event owners.
file(READ "${PROJECT_SOURCE_DIR}/src/hid_core/resource_manager.cpp" hid_source)
string(REPLACE "input_event->Finalize(system.Kernel());" "service_context.CloseEvent(input_event);" hid_source "${hid_source}")
write_derived("${PORT_BUILD_DIR}/resource_manager.cpp" "${hid_source}")
get_target_property(hid_sources hid_core SOURCES)
list(REMOVE_ITEM hid_sources resource_manager.cpp)
list(APPEND hid_sources "${PORT_BUILD_DIR}/resource_manager.cpp")
set_property(TARGET hid_core PROPERTY SOURCES "${hid_sources}")

# Clang 18 trips over libstdc++ range constraints for implicit vector-to-span
# conversions inside these generic lambdas. Use the equivalent pointer/count
# constructor for every variable-argument wrapper.
set(ioctl_header "core/hle/service/nvdrv/devices/ioctl_serialization.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${ioctl_header}" ioctl_source)
string(REPLACE "(fixed, inl_out," "(fixed, std::span<InlOutVarArg>{inl_out.data(), inl_out.size()}," ioctl_source "${ioctl_source}")
string(REPLACE "(var, std::forward" "(std::span<VarArg>{var.data(), var.size()}, std::forward" ioctl_source "${ioctl_source}")
string(REPLACE "(fixed, var," "(fixed, std::span<VarArg>{var.data(), var.size()}," ioctl_source "${ioctl_source}")
string(REPLACE "(fixed, inl_in," "(fixed, std::span<InlInVarArg>{inl_in.data(), inl_in.size()}," ioctl_source "${ioctl_source}")
write_derived("${PORT_BUILD_DIR}/include/${ioctl_header}" "${ioctl_source}")
target_include_directories(core BEFORE PRIVATE "${PORT_BUILD_DIR}/include")

# Slab allocation constructs C++ objects; returning a slot must release their
# host RAII members as well as the guest resources handled by Finalize().
set(slab_header "core/hle/kernel/k_slab_heap.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${slab_header}" slab_source)
string(REPLACE "    void Free(T* obj) {\n        BaseHeap::Free(obj);"
    "    void Free(T* obj) {\n        std::destroy_at(obj);\n        BaseHeap::Free(obj);" slab_source "${slab_source}")
# The slab heap's lock and a thread's context guard stay spin locks (headless/kernel_spin_lock.h).
set(slab_lock "    std::mutex m_lock;\n")
string(FIND "${slab_source}" "${slab_lock}" slab_lock_at)
if(slab_lock_at LESS 0)
    message(FATAL_ERROR "Pinned slab heap lock changed")
endif()
string(REPLACE "${slab_lock}" "    ::Eden::KernelSpinLock m_lock;\n" slab_source "${slab_source}")
string(PREPEND slab_source "#include \"${EDEN_PORT_DIR}/kernel_spin_lock.h\"\n")
write_derived("${PORT_BUILD_DIR}/include/${slab_header}" "${slab_source}")
set(thread_header "core/hle/kernel/k_thread.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${thread_header}" thread_source)
set(thread_guard "    std::mutex m_context_guard{};\n")
string(FIND "${thread_source}" "${thread_guard}" thread_guard_at)
if(thread_guard_at LESS 0)
    message(FATAL_ERROR "Pinned thread context guard changed")
endif()
string(REPLACE "${thread_guard}" "    ::Eden::KernelSpinLock m_context_guard{};\n" thread_source "${thread_source}")
string(PREPEND thread_source "#include \"${EDEN_PORT_DIR}/kernel_spin_lock.h\"\n")
write_derived("${PORT_BUILD_DIR}/include/${thread_header}" "${thread_source}")

# PostDestroy can release an owner process and enter the scheduler using the
# current dummy thread's mutex. Keep that thread alive through this cleanup.
set(slab_helpers "core/hle/kernel/slab_helpers.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${slab_helpers}" helpers_source)
string(REPLACE "        Free(kernel, static_cast<Derived*>(this));\n        if (is_initialized) {\n            Derived::PostDestroy(kernel, arg);\n        }"
    "        if (is_initialized) {\n            Derived::PostDestroy(kernel, arg);\n        }\n        Free(kernel, static_cast<Derived*>(this));" helpers_source "${helpers_source}")
write_derived("${PORT_BUILD_DIR}/include/${slab_helpers}" "${helpers_source}")

# Finish queued process/thread finalizers before shutting down their kernel
# dependencies or reusing slab storage for the next session.
set(worker_header "core/hle/kernel/k_worker_task_manager.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${worker_header}" worker_source)
string(REPLACE "    KWorkerTaskManager();" "    KWorkerTaskManager();\n    void WaitForRequests() { m_waiting_thread.WaitForRequests(); }" worker_source "${worker_source}")
write_derived("${PORT_BUILD_DIR}/include/${worker_header}" "${worker_source}")

# Keep service threads until their outer callbacks and C++ destructors finish.
# Finalization can wait for process/page-table light locks. Holding the outer
# scheduler lock postpones that wait until its stack-owned queue is already gone.
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/k_thread.cpp" thread_source)
set(old_finish "        // Acquire the scheduler lock.\n        KScopedSchedulerLock sl{kernel};\n\n        // Signal.\n        m_signaled = true;\n        KSynchronizationObject::NotifyAvailable(kernel);\n\n        // Close the thread.\n        this->Close(kernel);")
string(FIND "${thread_source}" "${old_finish}" finish_offset)
if(finish_offset LESS 0)
    message(FATAL_ERROR "Pinned thread termination changed; review finalization lock scope.")
endif()
string(REPLACE "${old_finish}" "        {\n            KScopedSchedulerLock sl{kernel};\n            m_signaled = true;\n            KSynchronizationObject::NotifyAvailable(kernel);\n        }\n\n        // Finalization may block; scheduling must be enabled here.\n        this->Close(kernel);" thread_source "${thread_source}")
write_derived("${PORT_BUILD_DIR}/k_thread.cpp" "${thread_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources hle/kernel/k_thread.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/k_thread.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# Game exit: the application's page table frees every physical page it maps (~1 s for a 4 GiB
# game) although the kernel shutdown that finalizes it recreates the memory manager at the next
# initialization. Skip those frees during a kernel shutdown; an in-game process exit (applets)
# still frees its pages.
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/k_page_table_base.cpp" page_table_source)
set(page_free_old [=[        // Get physical pages.
        KPageGroup pg(m_system.Kernel(), m_block_info_manager);
        this->MakePageGroup(pg, addr, size / PageSize);

        // Free the pages.
        pg.CloseAndReset(m_system.Kernel());
    };]=])
string(FIND "${page_table_source}" "${page_free_old}" page_free_at)
if(page_free_at LESS 0)
    message(FATAL_ERROR "Pinned page table finalization changed")
endif()
string(REPLACE "${page_free_old}" [=[        // The memory manager is recreated after a kernel shutdown.
        if (m_system.Kernel().IsShuttingDown()) {
            return;
        }

        // Get physical pages.
        KPageGroup pg(m_system.Kernel(), m_block_info_manager);
        this->MakePageGroup(pg, addr, size / PageSize);

        // Free the pages.
        pg.CloseAndReset(m_system.Kernel());
    };]=] page_table_source "${page_table_source}")
write_derived("${PORT_BUILD_DIR}/k_page_table_base.cpp" "${page_table_source}")
get_target_property(page_table_core_sources core SOURCES)
list(REMOVE_ITEM page_table_core_sources hle/kernel/k_page_table_base.cpp)
list(APPEND page_table_core_sources "${PORT_BUILD_DIR}/k_page_table_base.cpp")
set_property(TARGET core PROPERTY SOURCES "${page_table_core_sources}")

# A server's stopped flag only covers its inner request loop; stopping the CPU
# immediately after it fires can abandon the applet destructor mid-cleanup.
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/kernel.cpp" kernel_source)
string(REPLACE "    void Shutdown(KernelCore& kernel) {" "    void Shutdown(KernelCore& kernel) {\n        worker_task_manager.WaitForRequests();" kernel_source "${kernel_source}")
string(REPLACE "    ASSERT(R_SUCCEEDED(thread->Run(*this)));\n}"
    "    impl->service_threads.push_back(thread);\n    ASSERT(R_SUCCEEDED(thread->Run(*this)));\n}" kernel_source "${kernel_source}")
string(REPLACE "    std::mutex server_lock;" "    std::vector<KThread*> service_threads;\n    bool services_closing = false;\n    std::mutex server_lock;" kernel_source "${kernel_source}")
string(REPLACE "    void Initialize(KernelCore& kernel) {"
    "    void Initialize(KernelCore& kernel) {\n        services_closing = false;" kernel_source "${kernel_source}")
string(REPLACE "        std::scoped_lock lk{server_lock};\n        server_managers.clear();"
    "        decltype(server_managers) closing_managers;\n        {\n            std::scoped_lock lk{server_lock};\n            services_closing = true;\n            closing_managers.swap(server_managers);\n        }\n        for (auto& manager : closing_managers) manager->RequestStop();\n        closing_managers.clear();\n        for (auto* thread : service_threads) {\n            s32 index;\n            KSynchronizationObject* objects[] = {thread};\n            ASSERT(KSynchronizationObject::Wait(system.Kernel(), &index, objects, 1, -1).IsSuccess());\n            thread->Close(system.Kernel());\n        }\n        service_threads.clear();" kernel_source "${kernel_source}")
if(EDEN_DEV_PROFILE)
    # Development: time the service shutdown phases and every slow manager destruction, named by
    # the service process that ran it.
    string(PREPEND kernel_source "#include <algorithm>\n#include <chrono>\n#include <cstdio>\n#include <string>\n")
    string(REPLACE "    bool services_closing = false;" "    bool services_closing = false;\n    std::vector<std::string> server_names;" kernel_source "${kernel_source}")
    string(REPLACE "            impl->server_managers.emplace_back(std::move(server_manager));\n        }"
        "            impl->server_managers.emplace_back(std::move(server_manager));\n            auto* eden_process = GetCurrentProcessPointer(*this);\n            impl->server_names.emplace_back(eden_process ? eden_process->GetName() : \"?\");\n        }" kernel_source "${kernel_source}")
    string(REPLACE [=[        decltype(server_managers) closing_managers;
        {
            std::scoped_lock lk{server_lock};
            services_closing = true;
            closing_managers.swap(server_managers);
        }
        for (auto& manager : closing_managers) manager->RequestStop();
        closing_managers.clear();
        for (auto* thread : service_threads) {
            s32 index;
            KSynchronizationObject* objects[] = {thread};
            ASSERT(KSynchronizationObject::Wait(system.Kernel(), &index, objects, 1, -1).IsSuccess());
            thread->Close(system.Kernel());
        }
        service_threads.clear();]=] [=[        using EdenClock = std::chrono::steady_clock;
        const auto eden_ms = [](EdenClock::time_point from) {
            return std::chrono::duration<double, std::milli>(EdenClock::now() - from).count();
        };
        const auto eden_start = EdenClock::now();
        decltype(server_managers) closing_managers;
        decltype(server_names) closing_names;
        {
            std::scoped_lock lk{server_lock};
            services_closing = true;
            closing_managers.swap(server_managers);
            closing_names.swap(server_names);
        }
        const double eden_lock_ms = eden_ms(eden_start);
        const size_t eden_count = closing_managers.size();
        auto eden_phase = EdenClock::now();
        double eden_stop_worst = 0;
        for (auto& manager : closing_managers) {
            const auto eden_one = EdenClock::now();
            manager->RequestStop();
            eden_stop_worst = std::max(eden_stop_worst, eden_ms(eden_one));
        }
        const double eden_stop_ms = eden_ms(eden_phase);
        eden_phase = EdenClock::now();
        double eden_worst = 0;
        for (size_t i = 0; i < closing_managers.size(); ++i) {
            const auto eden_one = EdenClock::now();
            closing_managers[i].reset();
            const double ms = eden_ms(eden_one);
            eden_worst = std::max(eden_worst, ms);
            if (ms >= 20.0) {
                std::fprintf(stderr, "EDEN_SHUTDOWN_MANAGER index=%zu name=%s ms=%.1f\n", i,
                             i < closing_names.size() ? closing_names[i].c_str() : "?", ms);
            }
        }
        closing_managers.clear();
        const double eden_destroy_ms = eden_ms(eden_phase);
        eden_phase = EdenClock::now();
        double eden_wait_worst = 0;
        const size_t eden_threads = service_threads.size();
        for (auto* thread : service_threads) {
            const auto eden_one = EdenClock::now();
            s32 index;
            KSynchronizationObject* objects[] = {thread};
            ASSERT(KSynchronizationObject::Wait(system.Kernel(), &index, objects, 1, -1).IsSuccess());
            thread->Close(system.Kernel());
            eden_wait_worst = std::max(eden_wait_worst, eden_ms(eden_one));
        }
        service_threads.clear();
        std::fprintf(stderr, "EDEN_SHUTDOWN_SERVICES managers=%zu threads=%zu lock_ms=%.1f stop_ms=%.1f "
                     "worst_stop_ms=%.1f destroy_ms=%.1f worst_manager_ms=%.1f wait_ms=%.1f "
                     "worst_wait_ms=%.1f total_ms=%.1f\n", eden_count, eden_threads, eden_lock_ms,
                     eden_stop_ms, eden_stop_worst, eden_destroy_ms, eden_worst, eden_ms(eden_phase),
                     eden_wait_worst, eden_ms(eden_start));]=] kernel_source "${kernel_source}")
    # Development: time the kernel teardown steps after the services close (>= 20 ms each), and
    # the cost of an unbuffered stderr line on the console.
    function(eden_patch_kernel match replacement)
        string(FIND "${kernel_source}" "${match}" found_at)
        if(found_at LESS 0)
            message(FATAL_ERROR "Pinned kernel shutdown changed")
        endif()
        string(REPLACE "${match}" "${replacement}" patched "${kernel_source}")
        set(kernel_source "${patched}" PARENT_SCOPE)
    endfunction()
    eden_patch_kernel([=[    void Shutdown(KernelCore& kernel) {
        worker_task_manager.WaitForRequests();]=] [=[    void Shutdown(KernelCore& kernel) {
        auto eden_last = std::chrono::steady_clock::now();
        const auto eden_mark = [&eden_last](const char* step) {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - eden_last).count();
            if (ms >= 20.0) std::fprintf(stderr, "EDEN_SHUTDOWN_KERNEL step=%s ms=%.1f\n", step, ms);
            eden_last = std::chrono::steady_clock::now();
        };
        {
            const auto probe = std::chrono::steady_clock::now();
            for (int line = 0; line < 20; ++line) std::fprintf(stderr, "EDEN_SHUTDOWN_WRITE_PROBE line=%d\n", line);
            std::fprintf(stderr, "EDEN_SHUTDOWN_WRITE_PROBE lines=20 ms=%.1f\n",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe).count());
            eden_last = std::chrono::steady_clock::now();
        }
        worker_task_manager.WaitForRequests();
        eden_mark("worker_tasks");]=])
    eden_patch_kernel([=[        CloseServices();

        if (application_process) {
            application_process->Close(system.Kernel());
            application_process = nullptr;
        }
]=] [=[        CloseServices();
        eden_mark("close_services");

        if (application_process) {
            application_process->Close(system.Kernel());
            application_process = nullptr;
        }
        eden_mark("application_process");
]=])
    eden_patch_kernel([=[        CleanupObject(system_resource_limit);
]=] [=[        CleanupObject(system_resource_limit);
        eden_mark("persistent_objects");
]=])
    eden_patch_kernel([=[            schedulers[core_id].reset();
        }
]=] [=[            schedulers[core_id].reset();
        }
        eden_mark("schedulers");
]=])
    eden_patch_kernel([=[                registered_in_use_objects.clear();
            }
        }
]=] [=[                registered_in_use_objects.clear();
            }
        }
        eden_mark("in_use_objects");
]=])
    eden_patch_kernel([=[        global_object_list_container.reset();
]=] [=[        global_object_list_container.reset();
        eden_mark("object_container");
]=])
    eden_patch_kernel([=[        hardware_timer.reset();
]=] [=[        hardware_timer.reset();
        eden_mark("hardware_timer");
]=])
endif()
if(EDEN_DEV_PROFILE)
    # Development: time the application process teardown steps (>= 20 ms each).
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/kernel/k_process.cpp" process_source)
    string(PREPEND process_source "#include <chrono>\n#include <cstdio>\n")
    # First occurrence after KProcess::Finalize only: the comments recur in other functions.
    function(eden_patch_process match replacement)
        string(FIND "${process_source}" "void KProcess::Finalize(KernelCore& kernel) {" finalize_at)
        string(SUBSTRING "${process_source}" ${finalize_at} -1 tail)
        string(FIND "${tail}" "${match}" found_at)
        if(finalize_at LESS 0 OR found_at LESS 0)
            message(FATAL_ERROR "Pinned process finalization changed")
        endif()
        math(EXPR found_at "${finalize_at} + ${found_at}")
        string(LENGTH "${match}" match_length)
        math(EXPR after_at "${found_at} + ${match_length}")
        string(SUBSTRING "${process_source}" 0 ${found_at} head)
        string(SUBSTRING "${process_source}" ${after_at} -1 rest)
        set(process_source "${head}${replacement}${rest}" PARENT_SCOPE)
    endfunction()
    eden_patch_process([=[void KProcess::Finalize(KernelCore& kernel) {
    // Delete the process local region.
    this->DeleteThreadLocalRegion(kernel, m_plr_address);
]=] [=[void KProcess::Finalize(KernelCore& kernel) {
    auto eden_last = std::chrono::steady_clock::now();
    const auto eden_mark = [&eden_last](const char* step) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - eden_last).count();
        if (ms >= 20.0) std::fprintf(stderr, "EDEN_SHUTDOWN_PROCESS step=%s ms=%.1f\n", step, ms);
        eden_last = std::chrono::steady_clock::now();
    };
    // Delete the process local region.
    this->DeleteThreadLocalRegion(kernel, m_plr_address);
    eden_mark("local_region");
]=])
    eden_patch_process([=[    m_page_table.Finalize();
]=] [=[    eden_mark("used_memory_size");
    m_page_table.Finalize();
    eden_mark("page_table");
]=])
    eden_patch_process([=[    // Free all shared memory infos.
]=] [=[    eden_mark("system_resource");
    // Free all shared memory infos.
]=])
    eden_patch_process([=[    // Our thread local page list must be empty at this point.
]=] [=[    eden_mark("shared_memory");
    // Our thread local page list must be empty at this point.
]=])
    eden_patch_process([=[    // Clear expensive resources, as the destructor is not called for guest objects.
    for (auto& interface : m_arm_interfaces) {
        interface.reset();
    }
    m_exclusive_monitor.reset();
]=] [=[    eden_mark("resource_limit");
    // Clear expensive resources, as the destructor is not called for guest objects.
    for (auto& interface : m_arm_interfaces) {
        interface.reset();
    }
    eden_mark("arm_interfaces");
    m_exclusive_monitor.reset();
    eden_mark("exclusive_monitor");
]=])
    write_derived("${PORT_BUILD_DIR}/k_process.cpp" "${process_source}")
    get_target_property(process_core_sources core SOURCES)
    list(REMOVE_ITEM process_core_sources hle/kernel/k_process.cpp)
    list(APPEND process_core_sources "${PORT_BUILD_DIR}/k_process.cpp")
    set_property(TARGET core PROPERTY SOURCES "${process_core_sources}")
endif()
# Never wait for service fibers while holding their registration mutex. A late
# registration keeps caller ownership, stops, and runs its normal completion path.
string(REPLACE "        if (impl->is_shutting_down) {\n            return;\n        }\n\n        impl->server_managers.emplace_back(std::move(server_manager));"
    "        if (!impl->services_closing && !impl->is_shutting_down) {\n            impl->server_managers.emplace_back(std::move(server_manager));\n        }" kernel_source "${kernel_source}")
string(REPLACE "    manager->LoopProcess();" "    if (server_manager) manager->RequestStop();\n    manager->LoopProcess();" kernel_source "${kernel_source}")
set(server_header "core/hle/service/server_manager.h")
file(READ "${PROJECT_SOURCE_DIR}/src/${server_header}" server_header_source)
string(REPLACE "    Result LoopProcess();" "    void RequestStop();\n    Result LoopProcess();" server_header_source "${server_header_source}")
if(EDEN_DEV_PROFILE)
    string(REPLACE "    Mutex m_selection_mutex;" "    Mutex m_selection_mutex;\n    std::string m_eden_names;" server_header_source "${server_header_source}")
endif()
write_derived("${PORT_BUILD_DIR}/include/${server_header}" "${server_header_source}")
file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/server_manager.cpp" server_source)
string(REPLACE "ServerManager::~ServerManager() {\n    // Signal stop.\n    m_stop_source.request_stop();\n    m_wakeup_event->Signal(m_system.Kernel());"
    "void ServerManager::RequestStop() {\n    m_stop_source.request_stop();\n    m_wakeup_event->Signal(m_system.Kernel());\n}\n\nServerManager::~ServerManager() {\n    RequestStop();" server_source "${server_source}")
if(EDEN_DEV_PROFILE)
    # Development: split a slow manager destruction into its phases.
    string(PREPEND server_source "#include <chrono>\n#include <cstdio>\n")
    function(eden_patch_server match replacement)
        string(FIND "${server_source}" "${match}" found_at)
        if(found_at LESS 0)
            message(FATAL_ERROR "Pinned ServerManager destructor changed")
        endif()
        string(REPLACE "${match}" "${replacement}" patched "${server_source}")
        set(server_source "${patched}" PARENT_SCOPE)
    endfunction()
    eden_patch_server([=[    // Add the new server to sm: and get the moved server port.
    Kernel::KServerPort* server_port{};]=] [=[    // Add the new server to sm: and get the moved server port.
    Kernel::KServerPort* server_port{};
    m_eden_names += service_name + " ";]=])
    eden_patch_server([=[    // Create a new port.
    auto* port = Kernel::KPort::Create(m_system.Kernel());]=] [=[    // Create a new port.
    auto* port = Kernel::KPort::Create(m_system.Kernel());
    m_eden_names += service_name + " ";]=])
    eden_patch_server([=[ServerManager::~ServerManager() {
    RequestStop();]=] [=[ServerManager::~ServerManager() {
    RequestStop();
    const auto eden_t0 = std::chrono::steady_clock::now();]=])
    eden_patch_server([=[    m_stopped.Wait();
    m_threads.clear();]=] [=[    m_stopped.Wait();
    const auto eden_t1 = std::chrono::steady_clock::now();
    m_threads.clear();
    const auto eden_t2 = std::chrono::steady_clock::now();]=])
    eden_patch_server([=[    // Clean up sessions.]=] [=[    const auto eden_t3 = std::chrono::steady_clock::now();
    size_t eden_sessions = 0;
    // Clean up sessions.]=])
    eden_patch_server([=[        delete session;
    }

    // Close wakeup event.]=] [=[        delete session;
        ++eden_sessions;
    }
    const auto eden_t4 = std::chrono::steady_clock::now();

    // Close wakeup event.]=])
    eden_patch_server([=[        // Write event is owned by ServiceManager
    }
}]=] [=[        // Write event is owned by ServiceManager
    }
    const auto eden_t5 = std::chrono::steady_clock::now();
    const auto eden_span = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    if (eden_span(eden_t0, eden_t5) >= 10.0) {
        std::fprintf(stderr, "EDEN_SHUTDOWN_DTOR manager=%p services=%s stopped=%.1f threads=%.1f ports=%.1f sessions=%.1f n=%zu events=%.1f\n",
                     static_cast<void*>(this), m_eden_names.c_str(), eden_span(eden_t0, eden_t1), eden_span(eden_t1, eden_t2),
                     eden_span(eden_t2, eden_t3), eden_span(eden_t3, eden_t4), eden_sessions, eden_span(eden_t4, eden_t5));
    }
}]=])
endif()
write_derived("${PORT_BUILD_DIR}/server_manager.cpp" "${server_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources hle/service/server_manager.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/server_manager.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")
foreach(thread main_thread idle_thread)
    string(REPLACE "            KThread::Register(system.Kernel(), ${thread});"
        "            KThread::Register(system.Kernel(), ${thread});\n            system.Kernel().RegisterInUseObject(${thread});" kernel_source "${kernel_source}")
endforeach()
# Shutdown fibers never call ExitThread. Their original array-held reference
# keeps them alive until CPU joins; balance Run()'s additional reference here.
string(REPLACE "    for (auto* thread : impl->shutdown_threads)\n        void(thread->Run(*this));"
    "    for (auto* thread : impl->shutdown_threads) {\n        ASSERT(thread->Run(*this).IsSuccess());\n        thread->Close(*this);\n    }" kernel_source "${kernel_source}")
write_derived("${PORT_BUILD_DIR}/kernel.cpp" "${kernel_source}")
get_target_property(core_sources core SOURCES)
list(REMOVE_ITEM core_sources hle/kernel/kernel.cpp)
list(APPEND core_sources "${PORT_BUILD_DIR}/kernel.cpp")
set_property(TARGET core PROPERTY SOURCES "${core_sources}")

# Replace SDL audio with the optional native sink; the renderer stays null.
get_target_property(audio_sources audio_core SOURCES)
list(FILTER audio_sources EXCLUDE REGEX "sink/sdl3_sink\\.")
set_property(TARGET audio_core PROPERTY SOURCES "${audio_sources}")
get_target_property(audio_definitions audio_core COMPILE_DEFINITIONS)
list(REMOVE_ITEM audio_definitions HAVE_SDL3)
set_property(TARGET audio_core PROPERTY COMPILE_DEFINITIONS "${audio_definitions}")
get_target_property(audio_libraries audio_core LINK_LIBRARIES)
list(REMOVE_ITEM audio_libraries SDL3::SDL3)
set_property(TARGET audio_core PROPERTY LINK_LIBRARIES "${audio_libraries}")
get_target_property(audio_interface audio_core INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM audio_interface SDL3::SDL3 "$<LINK_ONLY:SDL3::SDL3>")
set_property(TARGET audio_core PROPERTY INTERFACE_LINK_LIBRARIES "${audio_interface}")

# Reuse the independently authored native ABI contracts from the research.
set(pad_contract "${PORT_ROOT}/third_party")
set(audio_contract "${PORT_ROOT}/third_party")
file(SHA256 "${pad_contract}/ps5_pad.hpp" pad_hash)
file(SHA256 "${audio_contract}/native_audio.hpp" audio_hash)
if(NOT pad_hash STREQUAL "6a9708fe7ab59afa69899e1981e84decf1492a97e8ebd08194f8e27841a00ee0" OR
   NOT audio_hash STREQUAL "54627379f641d608a9a5c6a96ebdfc543a0dbb8a1e843e0ce08cb6e3adb93528")
    message(FATAL_ERROR "Native device contracts changed; review the adapter ABI first.")
endif()
# Guest rumble: with no mapped buttons (this frontend binds the virtual_gamepad engine), send each
# player's left and right vibration to the PS5 controller engine (headless/pad.cpp).
file(READ "${PROJECT_SOURCE_DIR}/src/hid_core/frontend/emulated_controller.cpp" controller_source)
set(rumble_outputs "    output_params[LeftIndex] = left_joycon;\n    output_params[RightIndex] = right_joycon;\n")
string(FIND "${controller_source}" "${rumble_outputs}" rumble_outputs_at)
if(rumble_outputs_at LESS 0)
    message(FATAL_ERROR "Pinned controller output parameters changed")
endif()
string(REPLACE "${rumble_outputs}" "${rumble_outputs}    if (output_params[LeftIndex].Get(\"engine\", \"\").empty()) {\n        const auto rumble_port = std::to_string(Service::HID::NpadIdTypeToIndex(npad_id_type));\n        output_params[LeftIndex] = Common::ParamPackage{\"engine:virtual_gamepad,port:\" + rumble_port + \",pad:1\"};\n        output_params[RightIndex] = Common::ParamPackage{\"engine:virtual_gamepad,port:\" + rumble_port + \",pad:2\"};\n    }\n"
    controller_source "${controller_source}")
write_derived("${PORT_BUILD_DIR}/emulated_controller.cpp" "#include <string>\n${controller_source}")
get_target_property(controller_sources hid_core SOURCES)
list(FILTER controller_sources EXCLUDE REGEX "(^|/)emulated_controller\\.cpp$")
list(APPEND controller_sources "${PORT_BUILD_DIR}/emulated_controller.cpp")
set_property(TARGET hid_core PROPERTY SOURCES "${controller_sources}")
# A controller update reaches every applet resource that is assigned. One that is assigned but not
# activated yet has no controllers set up (NPad::Activate fills them in), and upstream calls
# through its null device. Buttons rarely changed in that window; the motion sensors change with
# every sample, so a game that registers a second applet resource crashed a few seconds after it
# started (seen on the console: page fault in NPad::ControllerUpdate from the pad thread).
file(READ "${PROJECT_SOURCE_DIR}/src/hid_core/resources/npad/npad.cpp" npad_source)
set(npad_update "        auto& controller = controller_data[aruid_index][controller_idx];\n        const auto is_connected = controller.device->IsConnected();")
string(FIND "${npad_source}" "${npad_update}" npad_update_at)
if(npad_update_at LESS 0)
    message(FATAL_ERROR "Pinned NPad controller update changed")
endif()
string(REPLACE "${npad_update}"
    "        auto& controller = controller_data[aruid_index][controller_idx];\n        if (controller.device == nullptr) {\n            continue;  // ProsperoEden: this applet resource is not activated yet\n        }\n        const auto is_connected = controller.device->IsConnected();"
    npad_source "${npad_source}")
write_derived("${PORT_BUILD_DIR}/npad.cpp" "${npad_source}")
get_target_property(npad_sources hid_core SOURCES)
list(FILTER npad_sources EXCLUDE REGEX "(^|/)npad\\.cpp$")
list(APPEND npad_sources "${PORT_BUILD_DIR}/npad.cpp")
set_property(TARGET hid_core PROPERTY SOURCES "${npad_sources}")
add_library(eden-pad STATIC "${EDEN_PORT_DIR}/pad.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/input_engine.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/input_poller.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/drivers/virtual_gamepad.cpp")
target_include_directories(eden-pad PUBLIC "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(eden-pad PUBLIC common)
target_compile_options(eden-pad PRIVATE -fno-rtti $<$<BOOL:${PS5_NATIVE}>:-femulated-tls>)
target_sources(audio_core PRIVATE "${EDEN_PORT_DIR}/audio.cpp")
target_include_directories(audio_core PRIVATE "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}")
file(READ "${PROJECT_SOURCE_DIR}/src/audio_core/sink/sink_details.cpp" sink_source)
string(REPLACE "    return GetOutputSinkDetails(sink_id).factory(device_id);"
    "    if (device_id == \"ps5\") return std::make_unique<Eden::AudioSink>();\n    return GetOutputSinkDetails(sink_id).factory(device_id);" sink_source "${sink_source}")
write_derived("${PORT_BUILD_DIR}/sink_details.cpp" "#include \"devices.h\"\n${sink_source}")
# Mirror mono PCM into both hardware channels in the existing upmix path.
file(READ "${PROJECT_SOURCE_DIR}/src/audio_core/sink/sink_stream.cpp" stream_source)
string(REPLACE "    if (type == StreamType::In)\n        return;"
    "    if (type == StreamType::In)\n        return;\n    static_assert(std::is_same_v<decltype(samples_buffer), Common::RingBuffer<s16, 0x10000>>);\n    if (buffer.frames * device_channels > 0x10000 - samples_buffer.Size())\n        throw std::overflow_error(\"Audio PCM queue capacity exceeded\");" stream_source "${stream_source}")
string(REPLACE "        samples_buffer.Push(new_samples);"
    "        if (system_channels == 1 && device_channels == 2) {\n            for (std::size_t i = 0; i < new_samples.size(); i += 2) new_samples[i + 1] = new_samples[i];\n        }\n        samples_buffer.Push(new_samples);" stream_source "${stream_source}")
# Refresh the held frame before a partial hardware block underruns.
string(REPLACE "    std::memcpy(last_frame.data(), &output_buffer[(frames_written - 1) * frame_size], frame_size_bytes);" "" stream_source "${stream_source}")
string(REPLACE "        samples_buffer.Pop(&output_buffer[frames_written * frame_size], frames_available * frame_size);"
    "        samples_buffer.Pop(&output_buffer[frames_written * frame_size], frames_available * frame_size);\n        if (frames_available) std::memcpy(last_frame.data(), &output_buffer[(frames_written + frames_available - 1) * frame_size], frame_size_bytes);" stream_source "${stream_source}")
write_derived("${PORT_BUILD_DIR}/sink_stream.cpp" "#include <stdexcept>\n${stream_source}")
# A bitfield expression yields 0/1, so explicitly restore the SRC flag's bit position.
file(READ "${PROJECT_SOURCE_DIR}/src/audio_core/renderer/voice/voice_info.cpp" voice_source)
set(old_voice_flag "flags |= u16(params.flags.IsVoicePitchAndSrcSkippedSupported);")
string(FIND "${voice_source}" "${old_voice_flag}" voice_flag_offset)
if(voice_flag_offset LESS 0)
    message(FATAL_ERROR "Pinned voice flag assignment changed; review the correction.")
endif()
string(REPLACE "${old_voice_flag}" "flags |= u16(params.flags.IsVoicePitchAndSrcSkippedSupported) << 1;" voice_source "${voice_source}")
write_derived("${PORT_BUILD_DIR}/voice_info.cpp" "${voice_source}")
get_target_property(audio_sources audio_core SOURCES)
list(REMOVE_ITEM audio_sources sink/sink_details.cpp sink/sink_stream.cpp renderer/voice/voice_info.cpp)
list(APPEND audio_sources "${PORT_BUILD_DIR}/sink_details.cpp" "${PORT_BUILD_DIR}/sink_stream.cpp" "${PORT_BUILD_DIR}/voice_info.cpp")
set_property(TARGET audio_core PROPERTY SOURCES "${audio_sources}")
if(PS5_NATIVE)
    target_link_libraries(eden-pad PUBLIC "${sdk}/target/lib/libScePad.so" "${sdk}/target/lib/libSceUserService.so")
    target_link_libraries(audio_core PUBLIC "${sdk}/target/lib/libSceAudioOut.so")
else()
    add_library(eden-device-mock STATIC "${EDEN_PORT_DIR}/mock_devices.cpp")
    target_include_directories(eden-device-mock PUBLIC "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}")
    target_include_directories(eden-device-mock PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-device-mock PRIVATE common)
    target_link_libraries(audio_core PUBLIC eden-device-mock)
endif()

file(READ "${PROJECT_SOURCE_DIR}/src/video_core/video_core.cpp" video_factory)
if(NOT EDEN_PS5_VULKAN)
string(REPLACE "#include \"video_core/renderer_vulkan/renderer_vulkan.h\"" "" video_factory "${video_factory}")
string(REPLACE "    case Settings::RendererBackend::Vulkan:\n        return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context));\n" "" video_factory "${video_factory}")
endif()
write_derived("${PORT_BUILD_DIR}/video_core.cpp" "${video_factory}")
get_target_property(video_sources video_core SOURCES)
if(NOT EDEN_PS5_VULKAN)
    list(FILTER video_sources EXCLUDE REGEX "^(renderer_vulkan|vulkan_common)/")
else()
    include("${EDEN_PORT_DIR}/vulkan.cmake")
endif()
list(REMOVE_ITEM video_sources video_core.cpp gpu.cpp)
list(APPEND video_sources "${PORT_BUILD_DIR}/video_core.cpp")
# Remove a derived header left by the isolated classic-uniform experiment.
file(REMOVE "${PORT_BUILD_DIR}/include/video_core/buffer_cache/buffer_cache_base.h")
# Join the worker before its renderer/context and scheduler are destroyed.
# C++ members are destroyed in reverse declaration order.
file(READ "${PROJECT_SOURCE_DIR}/src/video_core/gpu.cpp" gpu_source)
set(gpu_first_members ": system{system_}\n        , gpu_thread{system_}")
string(FIND "${gpu_source}" "${gpu_first_members}" gpu_first_members_at)
if(gpu_first_members_at LESS 0)
    message(FATAL_ERROR "Pinned GPU member order changed")
endif()
string(REPLACE "${gpu_first_members}" ": system{system_}" gpu_source "${gpu_source}")
string(REPLACE ", is_async{is_async_}" ", is_async{is_async_}\n        , gpu_thread{system_}" gpu_source "${gpu_source}")
string(REPLACE "    // Destruction of thread must be done before all (non trivial)\n    // previous members has been destroyed\n    VideoCommon::GPUThread::ThreadManager gpu_thread;\n" "" gpu_source "${gpu_source}")
string(REPLACE "    u64 pending_composite_fence{};" "    u64 pending_composite_fence{};\n    VideoCommon::GPUThread::ThreadManager gpu_thread;" gpu_source "${gpu_source}")
if(EDEN_PS5_OPENGL)
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_opengl/gl_buffer_cache.cpp" buffer_source)
    string(REPLACE "static_cast<GLsizeiptr>(size), GL_RED, GL_UNSIGNED_INT, &value);"
        "static_cast<GLsizeiptr>(size), GL_RED_INTEGER, GL_UNSIGNED_INT, &value);" buffer_source "${buffer_source}")
    write_derived("${PORT_BUILD_DIR}/gl_buffer_cache.cpp" "${buffer_source}")
    list(REMOVE_ITEM video_sources renderer_opengl/gl_buffer_cache.cpp)
    list(APPEND video_sources "${PORT_BUILD_DIR}/gl_buffer_cache.cpp")
    if(PS5_NATIVE)
        # PS5 permits reading an ended query while another query is active.
        # Retain the upstream NVIDIA workaround on all other builds.
        file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_opengl/gl_query_cache.cpp" query_source)
        set(query_slice "const bool slice_counter = WaitPending() && stream.IsEnabled();")
        string(FIND "${query_source}" "${query_slice}" query_slice_offset)
        if(query_slice_offset LESS 0)
            message(FATAL_ERROR "Pinned query slicing workaround changed")
        endif()
        string(REPLACE "${query_slice}" "const bool slice_counter = false; // PS5: do not split cached query reads."
            query_source "${query_source}")
        write_derived("${PORT_BUILD_DIR}/gl_query_cache.cpp" "${query_source}")
        list(REMOVE_ITEM video_sources renderer_opengl/gl_query_cache.cpp)
        list(APPEND video_sources "${PORT_BUILD_DIR}/gl_query_cache.cpp")
    endif()
    # Record actual guest draw submission, separately from the startup GL oracle.
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_opengl/gl_rasterizer.cpp" rasterizer_source)
    set(draw_end "        }\n    });\n}\n\nvoid RasterizerOpenGL::DrawIndirect()")
    string(FIND "${rasterizer_source}" "${draw_end}" draw_end_offset)
    if(draw_end_offset LESS 0)
        message(FATAL_ERROR "Pinned guest draw callback changed")
    endif()
    string(REPLACE "${draw_end}"
        "        }\n        static unsigned traced_draws{};\n        if (traced_draws < 3) {\n            ++traced_draws;\n            LOG_INFO(Render_OpenGL, \"EDEN_GUEST_GL_DRAW count={} error={}\", traced_draws, glGetError());\n        }\n    });\n}\n\nvoid RasterizerOpenGL::DrawIndirect()" rasterizer_source "${rasterizer_source}")
    if(EDEN_DEV_PROFILE)
        file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_opengl/gl_texture_cache.cpp" texture_source)
        set(copy_end "dst_origin.y, dst_origin.z, region.width, region.height, region.depth);")
        string(FIND "${texture_source}" "${copy_end}" copy_offset)
        if(copy_offset LESS 0)
            message(FATAL_ERROR "Pinned texture copy changed")
        endif()
        string(REPLACE "${copy_end}" "${copy_end}\n        static unsigned copy_errors = 0;\n        const GLenum copy_error = copy_errors < 8 ? glGetError() : GL_NO_ERROR;\n        if (copy_error && copy_errors++ < 8) {\n            GLint sw = 0, dw = 0;\n            glGetTextureLevelParameteriv(src_name, src_origin.level, GL_TEXTURE_WIDTH, &sw);\n            glGetTextureLevelParameteriv(dst_name, dst_origin.level, GL_TEXTURE_WIDTH, &dw);\n            std::printf(\"EDEN_DEV_COPY error=%x src=%u target=%x level=%d x=%d width=%d dst=%u target=%x level=%d x=%d width=%d extent=%dx%dx%d\\n\", copy_error, src_name, src_target, src_origin.level, src_origin.x, sw, dst_name, dst_target, dst_origin.level, dst_origin.x, dw, region.width, region.height, region.depth);\n        }" texture_source "${texture_source}")
        string(PREPEND texture_source "#include <cstdio>\n")
        write_derived("${PORT_BUILD_DIR}/gl_texture_cache.cpp" "${texture_source}")
        list(REMOVE_ITEM video_sources renderer_opengl/gl_texture_cache.cpp)
        list(APPEND video_sources "${PORT_BUILD_DIR}/gl_texture_cache.cpp")
        string(PREPEND rasterizer_source "#include \"${EDEN_PORT_DIR}/performance.h\"\n")
        string(PREPEND rasterizer_source "#include \"${EDEN_PORT_DIR}/dev_render_passes.inc\"\n")
        string(REPLACE "        static unsigned traced_draws{};"
            "        CaptureDevelopmentPass();\n        static unsigned traced_draws{};"
            rasterizer_source "${rasterizer_source}")
        string(REPLACE "void RasterizerOpenGL::Draw(bool is_indexed, u32 instance_count) {"
            "void RasterizerOpenGL::Draw(bool is_indexed, u32 instance_count) {\n    ::Eden::Performance::Timer draw_timer(::Eden::Performance::rasterizer_draw);"
            rasterizer_source "${rasterizer_source}")
    endif()
    write_derived("${PORT_BUILD_DIR}/gl_rasterizer.cpp" "${rasterizer_source}")
    list(REMOVE_ITEM video_sources renderer_opengl/gl_rasterizer.cpp)
    list(APPEND video_sources "${PORT_BUILD_DIR}/gl_rasterizer.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/renderer_opengl/renderer_opengl.cpp" renderer_source)
    write_derived("${PORT_BUILD_DIR}/renderer_opengl.cpp" "${renderer_source}")
    list(REMOVE_ITEM video_sources renderer_opengl/renderer_opengl.cpp)
    list(APPEND video_sources "${PORT_BUILD_DIR}/renderer_opengl.cpp")
    # Join the GPU worker before transferring its context to the teardown thread.
    if(PS5_NATIVE)
        set(MACRO_INPUT "${PROJECT_SOURCE_DIR}/src/video_core/macro.cpp")
        set(MACRO_OUTPUT "${PORT_BUILD_DIR}/macro_recovery.cpp")
        include("${EDEN_PORT_DIR}/macro_recovery.cmake")
        list(REMOVE_ITEM video_sources macro.cpp)
        list(APPEND video_sources "${MACRO_OUTPUT}")
    endif()
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/gpu_thread.h" gpu_thread_header)
    string(REPLACE "using CommandQueue = Common::SPSCQueue<CommandDataContainer>;"
        "using CommandQueue = Common::SPSCQueue<CommandDataContainer, 8>;"
        gpu_thread_header "${gpu_thread_header}")
    string(REPLACE "    ~ThreadManager();" "    ~ThreadManager();\n    void Stop() { thread.request_stop(); if (thread.joinable()) thread.join(); }" gpu_thread_header "${gpu_thread_header}")
    if(PS5_NATIVE)
        string(PREPEND gpu_thread_header "#include <pthread.h>\n")
        string(REPLACE "    void Stop() { thread.request_stop(); if (thread.joinable()) thread.join(); }"
            "    void Stop();" gpu_thread_header "${gpu_thread_header}")
        string(REPLACE "    std::jthread thread;"
            "    void RunWorker(VideoCore::RendererBase&, Core::Frontend::GraphicsContext&, Tegra::Control::Scheduler&);\n    pthread_t thread{};\n    std::stop_source stop_source;\n    bool thread_joinable{};" gpu_thread_header "${gpu_thread_header}")
        file(READ "${PROJECT_SOURCE_DIR}/src/video_core/gpu_thread.cpp" gpu_worker)
        set(worker_start "void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {\n    rasterizer = renderer.ReadRasterizer();\n    thread = std::jthread([&](std::stop_token stop_token) {")
        string(FIND "${gpu_worker}" "${worker_start}" worker_start_offset)
        if(worker_start_offset LESS 0)
            message(FATAL_ERROR "Pinned GPU worker entry changed")
        endif()
        string(REPLACE "${worker_start}"
            "void ThreadManager::RunWorker(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {\n    const auto stop_token = stop_source.get_token();" gpu_worker "${gpu_worker}")
        string(REPLACE "    });\n}" "}" gpu_worker "${gpu_worker}")
        string(REPLACE "thread.get_stop_token()" "stop_source.get_token()" gpu_worker "${gpu_worker}")
        string(REPLACE "ThreadManager::~ThreadManager() = default;" "ThreadManager::~ThreadManager() { Stop(); }" gpu_worker "${gpu_worker}")
        # Eden's own shutdown of its std::jthread worker: the worker here is a pthread, ended by Stop.
        set(worker_shutdown "void ThreadManager::NotifyShutdown() {\n    if (thread.joinable()) {\n        thread.request_stop();\n        thread.join();\n    }\n}")
        string(FIND "${gpu_worker}" "${worker_shutdown}" worker_shutdown_at)
        if(worker_shutdown_at LESS 0)
            message(FATAL_ERROR "Pinned GPU worker shutdown changed")
        endif()
        string(REPLACE "${worker_shutdown}" "void ThreadManager::NotifyShutdown() { Stop(); }" gpu_worker "${gpu_worker}")
        string(REPLACE "        CommandDataContainer next;" "        unsigned dispatches = 0;\n        CommandDataContainer next;" gpu_worker "${gpu_worker}")
        set(dispatch_call "                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));")
        string(REPLACE "${dispatch_call}"
            "                const auto count = ++dispatches;\n                if (count <= 3) LOG_INFO(Render_OpenGL, \"EDEN_GPU_DISPATCH_BEGIN count={}\", count);\n${dispatch_call}\n                if (count <= 3) LOG_INFO(Render_OpenGL, \"EDEN_GPU_DISPATCH_END count={}\", count);" gpu_worker "${gpu_worker}")
        # A full queue must not retain a producer after its consumer stops.
        string(REPLACE "    state.queue.EmplaceWait(std::move(command_data), fence, block);"
            "    while (!stop_source.stop_requested() &&\n           !state.queue.TryEmplace(std::move(command_data), fence, block))\n        std::this_thread::sleep_for(std::chrono::microseconds(100));\n    if (stop_source.stop_requested())\n        return state.signaled_fence.load(std::memory_order_relaxed);"
            gpu_worker "${gpu_worker}")
        file(READ "${EDEN_PORT_DIR}/native_gpu_thread.inc" native_gpu_lifecycle)
        string(REPLACE "#include \"diagnostics.h\"" "#include \"${EDEN_PORT_DIR}/diagnostics.h\""
            native_gpu_lifecycle "${native_gpu_lifecycle}")
        string(REPLACE "#include \"gpu_failure.h\"" "#include \"${EDEN_PORT_DIR}/gpu_failure.h\""
            native_gpu_lifecycle "${native_gpu_lifecycle}")
        string(APPEND gpu_worker "\n${native_gpu_lifecycle}")
        # Animate only until guest presentation, on the renderer-owning worker.
        set(hud_wait "            state.queue.PopWait(next, stop_token);")
        string(FIND "${gpu_worker}" "${hud_wait}" hud_wait_offset)
        if(hud_wait_offset LESS 0)
            message(FATAL_ERROR "Pinned GPU queue wait changed")
        endif()
        string(PREPEND gpu_worker "namespace VideoCore { class RendererBase; }\nnamespace Eden { bool LoadingTick(VideoCore::RendererBase&, bool idle); }\n")
        string(REPLACE "${hud_wait}"
            "            if (Eden::LoadingTick(renderer, false)) {\n                if (!state.queue.TryPop(next)) {\n                    Eden::LoadingTick(renderer, true);\n                    std::this_thread::sleep_for(std::chrono::milliseconds(1));\n                    continue;\n                }\n            } else {\n${hud_wait}\n            }" gpu_worker "${gpu_worker}")
        if(EDEN_DEV_PROFILE)
            string(PREPEND gpu_worker "#include \"${EDEN_PORT_DIR}/performance.h\"\n")
            string(REPLACE "${hud_wait}"
                "            { ::Eden::Performance::Timer idle_timer(::Eden::Performance::gpu_queue_wait);\n${hud_wait}\n            }"
                gpu_worker "${gpu_worker}")
            string(REPLACE "${dispatch_call}"
                "                { ::Eden::Performance::Timer dispatch_timer(::Eden::Performance::gpu_dispatch);\n${dispatch_call}\n                }"
                gpu_worker "${gpu_worker}")
            string(REPLACE "if (count <= 3) LOG_INFO(Render_OpenGL, \"EDEN_GPU_DISPATCH_"
                "if (count <= 3 || ::Eden::Performance::BootTrace()) LOG_INFO(Render_OpenGL, \"EDEN_GPU_DISPATCH_"
                gpu_worker "${gpu_worker}")
            # Time guest-side back-pressure: pushes that found the 8-entry queue full.
            set(full_wait "    while (!stop_source.stop_requested() &&\n           !state.queue.TryEmplace(std::move(command_data), fence, block))\n        std::this_thread::sleep_for(std::chrono::microseconds(100));")
            string(FIND "${gpu_worker}" "${full_wait}" full_wait_offset)
            if(full_wait_offset LESS 0)
                message(FATAL_ERROR "Pinned GPU queue push changed")
            endif()
            string(REPLACE "${full_wait}"
                "    if (stop_source.stop_requested() ||\n        !state.queue.TryEmplace(std::move(command_data), fence, block)) {\n        ::Eden::Performance::Timer full_timer(::Eden::Performance::gpu_queue_full);\n${full_wait}\n    }"
                gpu_worker "${gpu_worker}")
        endif()
        write_derived("${PORT_BUILD_DIR}/gpu_thread.cpp" "${gpu_worker}")
        list(REMOVE_ITEM video_sources gpu_thread.cpp)
        list(APPEND video_sources "${PORT_BUILD_DIR}/gpu_thread.cpp")
    endif()
    string(PREPEND gpu_source "#include <glad/glad.h>\n")
    write_derived("${PORT_BUILD_DIR}/include/video_core/gpu_thread.h" "${gpu_thread_header}")
    target_include_directories(video_core BEFORE PRIVATE "${PORT_BUILD_DIR}/include")
    string(REPLACE "    ~Impl() = default;" "    ~Impl() { gpu_thread.Stop(); if (renderer && Settings::IsOpenGL()) { renderer->Context().MakeCurrent(); glFinish(); } }" gpu_source "${gpu_source}")
    # Join before unique_ptr destruction clears GPU::impl: queued draws call back
    # through GPU::TickWork. NotifyShutdown also protects service-owned state.
    # Eden joins its worker first; here the waiters are woken first, so that a guest thread waiting
    # on the worker cannot keep it from ending.
    set(old_gpu_shutdown "    void NotifyShutdown() {\n        gpu_thread.NotifyShutdown();\n        std::unique_lock lk{sync_mutex};\n        shutting_down.store(true, std::memory_order::relaxed);\n        sync_cv.notify_all();\n    }")
    string(FIND "${gpu_source}" "${old_gpu_shutdown}" gpu_shutdown_offset)
    if(gpu_shutdown_offset LESS 0)
        message(FATAL_ERROR "Pinned GPU shutdown changed")
    endif()
    string(REPLACE "${old_gpu_shutdown}"
        "    void NotifyShutdown() {\n        {\n            std::unique_lock lk{sync_mutex};\n            shutting_down.store(true, std::memory_order::relaxed);\n            sync_cv.notify_all();\n        }\n        gpu_thread.Stop();\n    }" gpu_source "${gpu_source}")
    # In-flight guest waits must also unwind before Stop can join the worker.
    foreach(unit dma_pusher engines/puller)
        file(READ "${PROJECT_SOURCE_DIR}/src/video_core/${unit}.cpp" command_source)
        string(REPLACE "        sync_cv.wait(lk, [this]() { return synced; });"
            "        while (!synced && !system.IsShuttingDown())\n            sync_cv.wait_for(lk, std::chrono::milliseconds(10));\n        if (system.IsShuttingDown()) return false;" command_source "${command_source}")
        string(REPLACE "    while (word != value) {" "    while (word != value && !dma_pusher.system.IsShuttingDown()) {" command_source "${command_source}")
        string(PREPEND command_source "#include <chrono>\n")
        string(REPLACE "/" "_" unit_name "${unit}")
        write_derived("${PORT_BUILD_DIR}/${unit_name}.cpp" "${command_source}")
        list(REMOVE_ITEM video_sources "${unit}.cpp")
        list(APPEND video_sources "${PORT_BUILD_DIR}/${unit_name}.cpp")
    endforeach()
    # Once the worker has joined, queued callbacks can never execute. Drop them
    # before waking waiters: some callbacks borrow their callers' stack storage.
    string(REPLACE "        gpu_thread.Stop();\n    }"
        "        gpu_thread.Stop();\n        {\n            std::lock_guard lock{sync_request_mutex};\n            sync_requests.clear();\n            sync_requests_stopped = true;\n        }\n        sync_request_cv.notify_all();\n    }" gpu_source "${gpu_source}")
    string(REPLACE "    u64 pending_composite_fence{};" "    bool sync_requests_stopped{}; // Protected by sync_request_mutex.\n    u64 pending_composite_fence{};" gpu_source "${gpu_source}")
    string(REPLACE "        const u64 fence = ++last_sync_fence;" "        if (sync_requests_stopped) return CurrentSyncRequestFence();\n        const u64 fence = ++last_sync_fence;" gpu_source "${gpu_source}")
    string(REPLACE "return CurrentSyncRequestFence() >= fence;" "return sync_requests_stopped || CurrentSyncRequestFence() >= fence;" gpu_source "${gpu_source}")
    string(REPLACE "GPU::~GPU() = default;" "GPU::~GPU() { impl->NotifyShutdown(); }" gpu_source "${gpu_source}")
    string(REPLACE "void GPU::PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {"
        "void GPU::PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {\n    static std::atomic<unsigned> submissions{};\n    const auto count = ++submissions;\n    if (count <= 3) LOG_INFO(Render_OpenGL, \"EDEN_GUEST_GPU_SUBMIT count={} channel={}\", count, channel);"
        gpu_source "${gpu_source}")
    if(EDEN_DEV_PROFILE)
        string(PREPEND gpu_source "#include \"${EDEN_PORT_DIR}/performance.h\"\n")
        string(REPLACE "    if (count <= 3) LOG_INFO(Render_OpenGL, \"EDEN_GUEST_GPU_SUBMIT count={} channel={}\", count, channel);"
            "    if (count <= 3 || ::Eden::Performance::BootTrace()) LOG_INFO(Render_OpenGL, \"EDEN_GUEST_GPU_SUBMIT count={} channel={} entries={}\", count, channel, entries.command_lists.size());"
            gpu_source "${gpu_source}")
    endif()
endif()
write_derived("${PORT_BUILD_DIR}/gpu.cpp" "${gpu_source}")
list(APPEND video_sources "${PORT_BUILD_DIR}/gpu.cpp")
if(EDEN_DEV_PROFILE)
    # Render-enable evaluations per mode and result (EDEN_DEV_GUEST cond=), to tell GPU
    # render conditions apart from the game's own LOD choices (LOD swaps).
    file(READ "${PROJECT_SOURCE_DIR}/src/video_core/engines/maxwell_3d.cpp" maxwell_source)
    set(condition_accelerated "    if (rasterizer->AccelerateConditionalRendering()) {\n        execute_on = true;\n        return;\n    }")
    set(condition_end "    }\n    }\n}\n\nvoid Maxwell3D::ProcessCounterReset() {")
    foreach(anchor IN ITEMS condition_accelerated condition_end)
        string(FIND "${maxwell_source}" "${${anchor}}" anchor_at)
        if(anchor_at LESS 0)
            message(FATAL_ERROR "Pinned Maxwell3D::ProcessQueryCondition changed (${anchor})")
        endif()
    endforeach()
    string(REPLACE "${condition_accelerated}"
        "    if (rasterizer->AccelerateConditionalRendering()) {\n        execute_on = true;\n        ::Eden::Performance::CountCondition(0);\n        return;\n    }"
        maxwell_source "${maxwell_source}")
    string(REPLACE "${condition_end}"
        "    }\n    }\n    ::Eden::Performance::CountCondition(\n        regs.render_enable_override == Regs::RenderEnable::Override::AlwaysRender ? 1u :\n        regs.render_enable_override == Regs::RenderEnable::Override::NeverRender ? 2u :\n        3u + static_cast<unsigned>(regs.render_enable.mode) * 2u + (execute_on ? 1u : 0u));\n}\n\nvoid Maxwell3D::ProcessCounterReset() {"
        maxwell_source "${maxwell_source}")
    write_derived("${PORT_BUILD_DIR}/maxwell_3d.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${maxwell_source}")
    list(REMOVE_ITEM video_sources engines/maxwell_3d.cpp)
    list(APPEND video_sources "${PORT_BUILD_DIR}/maxwell_3d.cpp")
endif()
# The shader cache file's reader checks what it reads and keeps the whole entries of a damaged
# file (shader_cache_reader.inc; shader_cache_check.cpp checks it on the host).
file(READ "${PROJECT_SOURCE_DIR}/src/video_core/shader_environment.cpp" shader_environment_source)
string(FIND "${shader_environment_source}" "void LoadPipelines(" shader_reader_begin)
string(FIND "${shader_environment_source}" "} // namespace VideoCommon" shader_reader_end REVERSE)
if(shader_reader_begin LESS 0 OR shader_reader_end LESS shader_reader_begin)
    message(FATAL_ERROR "Pinned shader cache reader changed")
endif()
string(SUBSTRING "${shader_environment_source}" 0 ${shader_reader_begin} shader_environment_head)
string(SUBSTRING "${shader_environment_source}" ${shader_reader_end} -1 shader_environment_tail)
string(PREPEND shader_environment_head "#include <cstdio>\n#include <new>\n#include <stdexcept>\n#include <system_error>\n")
write_derived("${PORT_BUILD_DIR}/shader_environment.cpp"
    "${shader_environment_head}#include \"${EDEN_PORT_DIR}/shader_cache_reader.inc\"\n\n${shader_environment_tail}")
set_property(SOURCE "${PORT_BUILD_DIR}/shader_environment.cpp" APPEND PROPERTY OBJECT_DEPENDS
    "${EDEN_PORT_DIR}/shader_cache_reader.inc")
list(REMOVE_ITEM video_sources shader_environment.cpp)
list(APPEND video_sources "${PORT_BUILD_DIR}/shader_environment.cpp")
set_property(TARGET video_core PROPERTY SOURCES "${video_sources}")

if(PS5_NATIVE)
    target_compile_definitions(libzstd_static PRIVATE ZSTD_TRACE=0)
    # The native title cannot chdir. Set the shared path manager's data root
    # directly so every core/log/cache path remains inside its writable mount.
    file(READ "${PROJECT_SOURCE_DIR}/src/common/fs/path_util.cpp" paths_source)
    string(REPLACE "#else\n        eden_path = GetCurrentDir() / PORTABLE_DIR;"
        "#elif defined(PS5_NATIVE)\n        eden_path = Eden::UserDir();\n        eden_path_cache = eden_path / CACHE_DIR;\n        eden_path_config = eden_path / CONFIG_DIR;\n#else\n        eden_path = GetCurrentDir() / PORTABLE_DIR;" paths_source "${paths_source}")
    string(REPLACE "#else\n        // In Ryujinx's infinite wisdom"
        "#elif defined(PS5_NATIVE)\n        GenerateLegacyPath(EmuPath::RyujinxDir, eden_path / RYUJINX_DIR);\n#else\n        // In Ryujinx's infinite wisdom" paths_source "${paths_source}")
    # Eden's user folder follows the storage layout (storage_paths.h: /data/prosperoeden/user with
    # filesystem access, the sandbox's download0 without).
    set(paths_source "#include \"${EDEN_PORT_DIR}/storage_paths.h\"
${paths_source}")
    write_derived("${PORT_BUILD_DIR}/path_util.cpp" "${paths_source}")
    get_target_property(common_sources common SOURCES)
    file(READ "${PROJECT_SOURCE_DIR}/src/common/thread.cpp" thread_source)
    string(REPLACE "void SetCurrentThreadName(const char* name) {"
        "void SetCurrentThreadName(const char* name) {\n    ::Eden::Performance::RegisterWorker(name);" thread_source "${thread_source}")
    write_derived("${PORT_BUILD_DIR}/thread.cpp" "#include \"${EDEN_PORT_DIR}/performance.h\"\n${thread_source}")
    list(REMOVE_ITEM common_sources thread.cpp)
    list(APPEND common_sources "${PORT_BUILD_DIR}/thread.cpp")
    # PS5 advertises invariant TSC but no CPUID leaf0x15. The original fallback
    # was inside that leaf's branch, leaving frequency zero on this hardware.
    file(READ "${PROJECT_SOURCE_DIR}/src/common/cpu_features.cpp" features_source)
    set(old_tsc_fallback "        } else {\n            caps.tsc_frequency = X64::EstimateRDTSCFrequency();\n        }")
    string(FIND "${features_source}" "${old_tsc_fallback}" tsc_offset)
    if(tsc_offset LESS 0)
        message(FATAL_ERROR "Pinned TSC frequency detection changed")
    endif()
    string(REPLACE "${old_tsc_fallback}" "        }" features_source "${features_source}")
    string(REPLACE "    if (max_std_fn >= 0x16) {"
        "    if (caps.invariant_tsc && caps.tsc_frequency < 1'000'000'000ULL) {\n        caps.tsc_frequency = X64::EstimateRDTSCFrequency();\n    }\n\n    if (max_std_fn >= 0x16) {" features_source "${features_source}")
    write_derived("${PORT_BUILD_DIR}/cpu_features.cpp" "${features_source}")
    list(REMOVE_ITEM common_sources cpu_features.cpp)
    list(APPEND common_sources "${PORT_BUILD_DIR}/cpu_features.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/common/fs/fs.cpp" fs_source)
    string(REPLACE "fs::directory_iterator(path, ec)" "Eden::ReadNativeDirectory(path, ec)" fs_source "${fs_source}")
    string(PREPEND fs_source "#include \"${EDEN_PORT_DIR}/native_directory.h\"\n")
    write_derived("${PORT_BUILD_DIR}/fs.cpp" "${fs_source}")
    list(REMOVE_ITEM common_sources fs/fs.cpp)
    list(APPEND common_sources "${PORT_BUILD_DIR}/fs.cpp")
    list(REMOVE_ITEM common_sources fs/path_util.cpp)
    list(APPEND common_sources "${PORT_BUILD_DIR}/path_util.cpp")
    set_property(TARGET common PROPERTY SOURCES "${common_sources}")
    # Prefer this build's matching decoder headers over the C dependency
    # bundle's older FFmpeg headers exposed through OpenSSL's include root.
    target_include_directories(video_core BEFORE PRIVATE "${FFmpeg_INCLUDE_DIR}")
    # The older SDK omits DCCP's IP protocol name (wire value 33, also used by
    # Service::Sockets::Protocol). socket() still decides runtime support.
    get_target_property(core_sources core SOURCES)
    # Keep immutable firmware outside the small writable title-data quota.
    file(READ "${PROJECT_SOURCE_DIR}/src/core/file_sys/bis_factory.h" bis_header)
    string(REPLACE "VirtualDir dump_root);" "VirtualDir dump_root, VirtualDir firmware_root = {});" bis_header "${bis_header}")
    write_derived("${PORT_BUILD_DIR}/include/core/file_sys/bis_factory.h" "${bis_header}")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/file_sys/bis_factory.cpp" bis_source)
    string(REPLACE "VirtualDir dump_root_)" "VirtualDir dump_root_, VirtualDir firmware_root)" bis_source "${bis_source}")
    string(REPLACE "GetOrCreateDirectoryRelative(nand_root, \"/system/Contents/registered\")"
        "firmware_root ? std::move(firmware_root) : GetOrCreateDirectoryRelative(nand_root, \"/system/Contents/registered\")" bis_source "${bis_source}")
    # A game's mods folder is the player's (headless/mods.h): found whatever the case of its name,
    # and never created here (Eden makes one for every game that starts).
    eden_patch(bis_source
        "    return GetOrCreateDirectoryRelative(load_root, fmt::format(\"/{:016X}\", title_id));\n}\n\nVirtualDir BISFactory::GetModificationDumpRoot"
        "    const auto wanted = fmt::format(\"{:016X}\", title_id);\n    if (load_root != nullptr)\n        for (const auto& folder : load_root->GetSubdirectories())\n            if (Common::ToUpper(folder->GetName()) == wanted) return folder;\n    return nullptr;\n}\n\nVirtualDir BISFactory::GetModificationDumpRoot")
    string(PREPEND bis_source "#include \"common/string_util.h\"\n")
    write_derived("${PORT_BUILD_DIR}/bis_factory.cpp" "${bis_source}")
    list(REMOVE_ITEM core_sources file_sys/bis_factory.cpp)
    list(APPEND core_sources "${PORT_BUILD_DIR}/bis_factory.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/hle/service/filesystem/filesystem.cpp" fs_service)
    string(REPLACE "nand_directory, std::move(load_directory), std::move(dump_directory));"
        "nand_directory, std::move(load_directory), std::move(dump_directory), vfs.OpenDirectory(Eden::AssetsPath(\"firmware\"), FileSys::OpenMode::Read));" fs_service "${fs_service}")
    # Firmware lives in the player's game files folder (Settings > Game files, assets_dir.h).
    set(fs_service "#include \"${EDEN_PORT_DIR}/assets_dir.h\"
${fs_service}")
    write_derived("${PORT_BUILD_DIR}/filesystem.cpp" "${fs_service}")
    list(REMOVE_ITEM core_sources hle/service/filesystem/filesystem.cpp)
    list(APPEND core_sources "${PORT_BUILD_DIR}/filesystem.cpp")
    list(REMOVE_ITEM core_sources internal_network/network.cpp "${MULTIPLAYER_OUTPUT}/socket_network.cpp")
    file(READ "${MULTIPLAYER_OUTPUT}/socket_network.cpp" network_source)
    string(REPLACE "namespace Network {" "namespace Network {\n#ifndef IPPROTO_DCCP\nconstexpr int IPPROTO_DCCP = 33;\n#endif" network_source "${network_source}")
    # Native pipes can be created but fail their first byte write with ENOMEM.
    # Preserve Eden's poll/write/read wakeup design using a local socket pair.
    set(native_network_init [=[
void Initialize() {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, interrupt_pipe_fd) != 0)
        throw std::runtime_error("Native wakeup socketpair failed");
    try {
        unsigned char value = 0x5a, received = 0;
        errno = 0;
        const auto written = send(interrupt_pipe_fd[1], &value, 1, MSG_DONTWAIT);
        const int write_error = errno;
        errno = 0;
        const auto count = recv(interrupt_pipe_fd[0], &received, 1, MSG_DONTWAIT);
        const int read_error = errno;
        std::printf("EDEN_SOCKET_PRIME write=%lld write_errno=%d read=%lld read_errno=%d value=%u\n",
            static_cast<long long>(written), write_error, static_cast<long long>(count), read_error, received);
        if (written != 1 || count != 1 || received != value)
            throw std::runtime_error("Native wakeup socketpair initialization failed");
    } catch (...) {
        for (int& fd : interrupt_pipe_fd) { close(fd); fd = -1; }
        throw;
    }
}

]=])
    string(FIND "${network_source}" "void Initialize() {\n    if (pipe(" init_start)
    string(FIND "${network_source}" "void Finalize() {\n    if (interrupt_pipe_fd" init_end)
    if(init_start LESS 0 OR init_end LESS init_start)
        message(FATAL_ERROR "Pinned network initialization changed")
    endif()
    string(SUBSTRING "${network_source}" 0 ${init_start} network_prefix)
    string(SUBSTRING "${network_source}" ${init_end} -1 network_suffix)
    set(network_source "${network_prefix}${native_network_init}${network_suffix}")
    set(native_interrupt [=[
    errno = 0;
    const auto written = send(interrupt_pipe_fd[1], &value, sizeof(value), MSG_DONTWAIT);
    const int write_error = errno;
    // A full nonblocking stream already has a pending wakeup for poll().
    if (written == -1 && (write_error == EAGAIN || write_error == EWOULDBLOCK)) return;
    if (written != 1) std::fprintf(stderr, "EDEN_SOCKET_INTERRUPT result=%lld errno=%d\n",
        static_cast<long long>(written), write_error);
    ASSERT(written == 1);
]=])
    string(REPLACE "    ASSERT(write(interrupt_pipe_fd[1], &value, sizeof(value)) == 1);" "${native_interrupt}" network_source "${network_source}")
    string(REPLACE "ssize_t ret = read(interrupt_pipe_fd[0], &value, sizeof(value));" "ssize_t ret = recv(interrupt_pipe_fd[0], &value, sizeof(value), MSG_DONTWAIT);" network_source "${network_source}")
    string(PREPEND network_source "#include <cstdio>\n#include <stdexcept>\n")
    write_derived("${PORT_BUILD_DIR}/network.cpp" "${network_source}")
    list(APPEND core_sources "${PORT_BUILD_DIR}/network.cpp")
    # Reuse the getifaddrs path instead of newer BSD route-format helpers.
    # Keep its existing zero-gateway fallback; PS5 route discovery is unqualified.
    list(REMOVE_ITEM core_sources internal_network/network_interface.cpp "${MULTIPLAYER_OUTPUT}/network_interface.cpp")
    file(READ "${MULTIPLAYER_OUTPUT}/network_interface.cpp" interfaces_source)
    string(REPLACE "defined(__linux__) || defined(__ANDROID__)" "defined(__linux__) || defined(__ANDROID__) || defined(__FreeBSD__)" interfaces_source "${interfaces_source}")
    string(REPLACE "defined(__ANDROID__) || defined(__linux__)" "defined(__ANDROID__) || defined(__linux__) || defined(__FreeBSD__)" interfaces_source "${interfaces_source}")
    string(REPLACE "#ifdef __ANDROID__" "#if defined(__ANDROID__) || defined(__FreeBSD__)" interfaces_source "${interfaces_source}")
    write_derived("${PORT_BUILD_DIR}/network_interface.cpp" "${interfaces_source}")
    list(APPEND core_sources "${PORT_BUILD_DIR}/network_interface.cpp")
    set_property(TARGET core PROPERTY SOURCES "${core_sources}")
    # Eden only uses Boost.Process v1's header-only async_pipe. Its unused v2
    # library assumes newer FreeBSD process APIs than the PS5 SDK supplies.
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libraries core ${property})
        list(REMOVE_ITEM libraries Boost::process)
        set_property(TARGET core PROPERTY ${property} "${libraries}")
    endforeach()
    target_include_directories(core PRIVATE "$<TARGET_PROPERTY:boost_process,INTERFACE_INCLUDE_DIRECTORIES>")
    target_link_libraries(core PRIVATE "$<TARGET_PROPERTY:boost_process,INTERFACE_LINK_LIBRARIES>")
    set(native_template "${PORT_ROOT}/../ps5-native-app-boilerplate")
    set(heap_source "${PORT_ROOT}/third_party/app_heap.c")
    file(SHA256 "${heap_source}" heap_hash)
    if(NOT heap_hash STREQUAL "df61acb51502c3b75c4fda62859c42eac6d19a6690f9b68c0e2dfbf20ae72c0f")
        message(FATAL_ERROR "The shared heap changed since G6; qualify it before using it here.")
    endif()
    # The heap may grow to 3 GiB: with guest cores 1/2 at 192 MiB of code, a large game's
    # blocks (~0.5 KiB of metadata each) once exhausted 2 GiB. That is its address range, not its
    # memory: the heap commits 128 MiB pieces as allocations need them (heap_arenas.inc), because
    # everything the CPU holds comes out of the 12 GiB it shares with the GPU. Taken whole at
    # start, guest RAM, a dense page table, this heap and the A64 code caches were 8.6 GiB of it.
    file(READ "${heap_source}" heap)
    string(REPLACE "128 MiB" "3072 MiB" heap "${heap}")
    string(REPLACE "(128u * 1024u * 1024u)" "(3072u * 1024u * 1024u)" heap "${heap}")
    string(REPLACE "#include <sys/mman.h>" "#include <sys/mman.h>\nvoid *eden_heap_pages(size_t);\nvoid eden_heap_pages_free(void *, size_t);" heap "${heap}")
    string(REPLACE "mmap(NULL, PS5_OPENGL_HEAP_SIZE, PROT_READ | PROT_WRITE,\n                    MAP_PRIVATE | MAP_ANON, -1, 0)" "eden_heap_pages(PS5_OPENGL_HEAP_SIZE)" heap "${heap}")
    string(REPLACE "if (base == MAP_FAILED)" "if (base == NULL)" heap "${heap}")
    string(REPLACE "munmap(base, PS5_OPENGL_HEAP_SIZE)" "eden_heap_pages_free(base, PS5_OPENGL_HEAP_SIZE)" heap "${heap}")
    string(REPLACE "\"PS5-OpenGL\"" "\"Eden-headless\"" heap "${heap}")
    # Replace the single-mspace wrappers with per-thread arenas over the same heap
    # (headless/heap_arenas.inc). The shared source above stays pinned/unchanged.
    option(EDEN_HEAP_ARENAS "Per-thread arenas inside the owned heap" ON)
    if(EDEN_HEAP_ARENAS)
        set(heap_wrappers_begin "static int ps5_heap_ready(void) {")
        set(heap_wrappers_end "void ps5_opengl_heap_stats_print(unsigned iteration) {")
        string(FIND "${heap}" "${heap_wrappers_begin}" heap_begin_at)
        string(FIND "${heap}" "${heap_wrappers_end}" heap_end_at)
        if(heap_begin_at LESS 0 OR heap_end_at LESS heap_begin_at)
            message(FATAL_ERROR "Pinned heap wrapper layout changed")
        endif()
        string(SUBSTRING "${heap}" 0 ${heap_begin_at} heap_prefix)
        string(SUBSTRING "${heap}" ${heap_end_at} -1 heap_suffix)
        file(READ "${EDEN_PORT_DIR}/heap_arenas.inc" heap_arenas)
        set(heap "${heap_prefix}${heap_arenas}${heap_suffix}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_PORT_DIR}/heap_arenas.inc")
    endif()
    option(EDEN_HEAP_GUARD "Development trailing heap corruption guards" OFF)
    if(EDEN_HEAP_GUARD)
        string(REPLACE "static atomic_int ps5_heap_state;"
            "#include \"${EDEN_PORT_DIR}/heap_guard.h\"\nstatic atomic_int ps5_heap_state;" heap "${heap}")
    endif()
    write_derived("${PORT_BUILD_DIR}/app_heap.c" "${heap}")
    set(eden_app_crt "${native_template}/tooling/native/app_crt.cpp")
    if(EDEN_PS5_VULKAN AND EDEN_VULKAN_DRIVER STREQUAL "RADV")
        file(READ "${eden_app_crt}" radv_crt)
        string(FIND "${radv_crt}" "extern \"C\" void _init()\n{" fp_init_at)
        if(fp_init_at EQUAL -1)
            message(FATAL_ERROR "Native CRT initialization changed; review IEEE startup placement")
        endif()
        string(REPLACE "extern \"C\" void _init()\n{"
            "extern \"C\" void ps5_fp_ieee();\nextern \"C\" void _init()\n{\n    ps5_fp_ieee();"
            radv_crt "${radv_crt}")
        set(eden_app_crt "${PORT_BUILD_DIR}/radv_app_crt.cpp")
        write_derived("${eden_app_crt}" "${radv_crt}")
    endif()
    add_executable(eden-headless "${EDEN_PORT_DIR}/main.cpp" "${PORT_ROOT}/src/aligned_alloc.c"
        "${EDEN_PORT_DIR}/heap_pages.cpp" "${EDEN_PORT_DIR}/performance.cpp"
        "${EDEN_PORT_DIR}/crash_report.cpp" "${EDEN_PORT_DIR}/stop_limit.cpp"
        "${EDEN_PORT_DIR}/elevation/elevation.cpp"
        "${eden_app_crt}" "${PORT_ROOT}/src/lifecycle.c"
        "${PORT_BUILD_DIR}/app_heap.c")
    target_compile_definitions(eden-headless PRIVATE PS5_NATIVE=1)
    set_property(TARGET eden-headless APPEND PROPERTY LINK_DEPENDS
        "${PORT_ROOT}/tools/link-headless-native.sh" "${PORT_ROOT}/tools/unwind.ld"
        "${PORT_ROOT}/tools/app-symbols.map")
    if(EDEN_PS5_VULKAN AND EDEN_VULKAN_DRIVER STREQUAL "RADV")
        set_property(TARGET eden-headless APPEND PROPERTY LINK_DEPENDS
            "${PORT_ROOT}/tools/radv-link-eden.sh"
            "${PORT_ROOT}/../mihawk-vulkan-review/tools/radv-link.sh")
    endif()
    target_compile_definitions(common PRIVATE PS5_NATIVE=1)
else()
    add_executable(eden-headless "${EDEN_PORT_DIR}/main.cpp" "${PORT_ROOT}/src/aligned_alloc.c"
        "${EDEN_PORT_DIR}/stop_limit.cpp")
    target_link_options(eden-headless PRIVATE -Wl,--wrap=aligned_alloc)
    add_executable(eden-romfs-check "${EDEN_PORT_DIR}/romfs_check.cpp")
    target_include_directories(eden-romfs-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-romfs-check PRIVATE core)
    target_compile_options(eden-romfs-check PRIVATE -fno-rtti)
    add_executable(eden-scalar-check "${EDEN_PORT_DIR}/scalar_check.cpp")
    target_include_directories(eden-scalar-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-scalar-check PRIVATE dynarmic)
    add_executable(eden-memory-check "${EDEN_PORT_DIR}/memory_check.cpp")
    target_include_directories(eden-memory-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-memory-check PRIVATE dynarmic common)
    target_compile_options(eden-scalar-check PRIVATE -fno-rtti)
    add_executable(eden-ryujinx-check "${EDEN_PORT_DIR}/ryujinx_check.cpp")
    add_executable(eden-shader-cache-check "${EDEN_PORT_DIR}/shader_cache_check.cpp")
    target_include_directories(eden-shader-cache-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-shader-cache-check PRIVATE video_core)
    target_compile_options(eden-shader-cache-check PRIVATE -fno-rtti)
    add_executable(eden-mods-check "${EDEN_PORT_DIR}/mods_check.cpp")
    target_include_directories(eden-mods-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-mods-check PRIVATE core)
    target_compile_options(eden-mods-check PRIVATE -fno-rtti)
    # The settings store's check is written with assert(): they stay in a release build.
    add_executable(eden-settings-check "${EDEN_PORT_DIR}/preferences_check.cpp")
    target_link_libraries(eden-settings-check PRIVATE core)
    target_compile_options(eden-settings-check PRIVATE -UNDEBUG)
    # So is the check of the cheat files' entries (patch_library.h).
    add_executable(eden-patch-library-check "${EDEN_PORT_DIR}/patch_library_check.cpp")
    target_compile_options(eden-patch-library-check PRIVATE -UNDEBUG)
    add_executable(eden-devices-check "${EDEN_PORT_DIR}/devices_check.cpp")
    target_link_libraries(eden-devices-check PRIVATE core eden-pad eden-device-mock)
    target_compile_options(eden-devices-check PRIVATE -fno-rtti)
    # The player profiles (profiles.h): Eden's profile file and the choice in the settings file.
    add_executable(eden-profiles-check "${EDEN_PORT_DIR}/profiles_check.cpp")
    target_link_libraries(eden-profiles-check PRIVATE core)
    target_compile_options(eden-profiles-check PRIVATE -UNDEBUG)
    # save-sync.json kept in step with the profiles (save_sync_config.h).
    add_executable(eden-save-sync-check "${EDEN_PORT_DIR}/save_sync_check.cpp")
    target_link_libraries(eden-save-sync-check PRIVATE core)
    target_compile_options(eden-save-sync-check PRIVATE -UNDEBUG)
    # The update check's decision, without its network side (update_notice.h).
    add_executable(eden-update-check
        "${EDEN_PORT_DIR}/update_check_check.cpp" "${EDEN_PORT_DIR}/update_check/update_check.c")
    target_compile_definitions(eden-update-check PRIVATE UPDATE_CHECK_NO_NETWORK=1)
    target_compile_options(eden-update-check PRIVATE -UNDEBUG)
    # The self-update helper keeps the version it replaces: moving it to where it is kept.
    add_executable(eden-update-files-check
        "${EDEN_PORT_DIR}/self_update_files_check.cpp" "${EDEN_PORT_DIR}/self_update_helper/files.cpp")
    target_include_directories(eden-update-files-check PRIVATE "${EDEN_PORT_DIR}")
endif()
target_include_directories(eden-headless PRIVATE "${PROJECT_SOURCE_DIR}/src")
# The app version has one home, sce_sys/param.json (contentVersion), which the package carries as
# it is. A header of its own, so a new version recompiles only the two files that show it.
file(READ "${PORT_ROOT}/sce_sys/param.json" app_param)
string(JSON app_version GET "${app_param}" contentVersion)
if(NOT app_version MATCHES "^[0-9][0-9]\\.[0-9][0-9][0-9]\\.[0-9][0-9][0-9]$")
    message(FATAL_ERROR "sce_sys/param.json: contentVersion must be NN.NNN.NNN, not '${app_version}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PORT_ROOT}/sce_sys/param.json")
write_derived("${PORT_BUILD_DIR}/version/app_version.h"
    "// Generated from sce_sys/param.json (contentVersion).\n#pragma once\n#define EDEN_APP_VERSION \"${app_version}\"\n")
target_include_directories(eden-headless PRIVATE "${PORT_BUILD_DIR}/version")
target_link_libraries(eden-headless PRIVATE core eden-pad)
# A game's text entry on the PS5's own on-screen keyboard (system_keyboard.h, keyboard_applet.h).
target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/system_keyboard.cpp")
target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/multiplayer.cpp")
target_include_directories(eden-headless BEFORE PRIVATE "${MULTIPLAYER_SOCKET_HEADERS}" "${MULTIPLAYER_OUTPUT}")
if(PS5_NATIVE)
    target_link_libraries(eden-headless PRIVATE
        "${sdk}/target/lib/libSceImeDialog.so" "${sdk}/target/lib/libSceSysmodule.so"
        "${PORT_ROOT}/build/stubs/libSceCommonDialog.so")
endif()
add_library(eden-metadata STATIC "${EDEN_PORT_DIR}/metadata_bridge.cpp")
target_include_directories(eden-metadata PUBLIC "${EDEN_PORT_DIR}" PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(eden-metadata PUBLIC core stb)
target_compile_options(eden-metadata PRIVATE -fno-rtti -Wall -Wextra -Werror -Wno-unused-parameter)
if(PS5_NATIVE)
    target_compile_options(eden-metadata PRIVATE -femulated-tls)
endif()
add_dependencies(eden-headless eden-metadata)

option(EDEN_HOST_ASAN "Instrument host emulator code for memory diagnostics" OFF)
if(EDEN_HOST_ASAN)
    if(PS5_NATIVE)
        message(FATAL_ERROR "EDEN_HOST_ASAN is a Linux host diagnostic only")
    endif()
    # Generated guest instructions and custom fiber stacks are not covered.
    foreach(asan_target common core audio_core video_core hid_core eden-pad eden-device-mock eden-headless)
        target_compile_options(${asan_target} PRIVATE -fsanitize=address -g1 -fno-omit-frame-pointer
            -fno-sanitize-address-use-after-scope -fsanitize-address-use-after-return=never)
    endforeach()
    target_link_options(eden-headless PRIVATE -fsanitize=address)
endif()
target_compile_options(eden-headless PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -fwrapv $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(PS5_NATIVE)
    target_compile_options(eden-headless PRIVATE -femulated-tls)
    option(EDEN_DEVICE_FRONTEND "Enable the unqualified native controller/audio frontend" OFF)
    target_compile_definitions(eden-headless PRIVATE EDEN_DEVICE_FRONTEND=$<BOOL:${EDEN_DEVICE_FRONTEND}>)
endif()

# Consume the frozen, relocatable 4.6 SDK as one complete profile.
option(EDEN_PS5_OPENGL "Use the PS5 EGL/OpenGL frontend" OFF)
set(EDEN_DEV_ROM_ID "" CACHE STRING "Development-only first-launch title ID; empty uses launcher")
if(NOT EDEN_DEV_ROM_ID STREQUAL "")
    string(LENGTH "${EDEN_DEV_ROM_ID}" dev_id_length)
    if(NOT dev_id_length EQUAL 16 OR NOT EDEN_DEV_ROM_ID MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "Development title ID must be 16 hex digits")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_ROM_ID="${EDEN_DEV_ROM_ID}")
endif()
option(EDEN_DEV_PROFILE "Development-only autoboot, timed input replay and profiling" OFF)
set(EDEN_DEV_PROFILE_TITLE "" CACHE STRING "Title ID the development profile boots and replays input for")
option(EDEN_DEV_WAIT_CALLERS "Development-only bounded GPU-owner caller sampling" OFF)
if(EDEN_DEV_WAIT_CALLERS)
    if(NOT EDEN_DEV_PROFILE OR NOT PS5_NATIVE)
        message(FATAL_ERROR "Caller sampling requires the native development profile")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_WAIT_CALLERS=1)
endif()
if(EDEN_DEV_PROFILE)
    string(LENGTH "${EDEN_DEV_PROFILE_TITLE}" profile_title_length)
    if(NOT profile_title_length EQUAL 16 OR NOT EDEN_DEV_PROFILE_TITLE MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "EDEN_DEV_PROFILE needs EDEN_DEV_PROFILE_TITLE (a 16-digit title ID)")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_PROFILE=1
        EDEN_DEV_PROFILE_TITLE="${EDEN_DEV_PROFILE_TITLE}")
endif()
option(EDEN_GPU_PROBE "Run bounded graphics-only architecture matrix, then exit" OFF)
if(EDEN_GPU_PROBE)
    if(NOT EDEN_PS5_OPENGL)
        message(FATAL_ERROR "GPU probe requires native OpenGL")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_GPU_PROBE=1)
endif()
if(EDEN_PS5_OPENGL)
    if(NOT PS5_NATIVE OR NOT ENABLE_OPENGL)
        message(FATAL_ERROR "PS5 graphics requires the native OpenGL build")
    endif()
    # The pinned PS5 OpenGL 4.6 SDK release (tools/deps.json, make deps).
    set(gl46 "${PORT_ROOT}/.deps/ps5-opengl-sdk-1.0.0/sdk")
    set(gl46_expected "d0c5accb58e07f1359fecfd9e4313df08acb86af71fb80052da3e9f06203fdc8")
    if(NOT EXISTS "${gl46}/lib/libps5_opengl_core33.a")
        message(FATAL_ERROR "Missing the PS5 OpenGL SDK in ${gl46}; run make deps")
    endif()
    file(SHA256 "${gl46}/lib/libps5_opengl_core33.a" gl46_hash)
    if(EDEN_DEV_PROFILE)
        target_compile_definitions(eden-headless PRIVATE EDEN_DEV_GL_VERSION="${gl46_hash}-sdk-1.0.0")
    endif()
    if(NOT gl46_hash STREQUAL gl46_expected)
        message(FATAL_ERROR "Frozen OpenGL 4.6 runtime changed")
    endif()
    set(PS5OpenGL_DIR "${gl46}/lib/cmake/PS5OpenGL" CACHE PATH "Frozen Eden graphics SDK" FORCE)
    find_package(PS5OpenGL CONFIG REQUIRED PATHS "${gl46}/lib/cmake/PS5OpenGL" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/graphics.cpp")
    # The loading screen's shader as a string for the OpenGL window; the Vulkan HUD pass includes
    # the same two files (vulkan_hud.frag).
    file(READ "${EDEN_PORT_DIR}/loading_text.glsl" loading_wordmark)
    file(READ "${EDEN_PORT_DIR}/loading_scene.glsl" loading_scene)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${EDEN_PORT_DIR}/loading_text.glsl" "${EDEN_PORT_DIR}/loading_scene.glsl")
    write_derived("${PORT_BUILD_DIR}/include/loading_scene_glsl.h"
        "// Generated from loading_text.glsl and loading_scene.glsl.\n#pragma once\ninline constexpr const char* kLoadingSceneGlsl = R\"glsl(\n${loading_wordmark}${loading_scene})glsl\";\n")
    target_compile_definitions(eden-headless PRIVATE EDEN_PS5_OPENGL=1)
    target_link_libraries(eden-headless PRIVATE PS5OpenGL::OpenGL glad)
endif()
    # Launcher UI: its own small OpenGL renderer, mixer and screens (headless/prosperoeden/pe),
    # all gone before a game starts.
    if(PS5_NATIVE)
        set(launcher_dir "${EDEN_PORT_DIR}/prosperoeden")
        target_sources(eden-headless PRIVATE
            "${launcher_dir}/runtime_shims.cpp"
            "${launcher_dir}/radio_input.c")
        target_include_directories(eden-headless PRIVATE "${launcher_dir}" "${PORT_ROOT}/third_party")
        if(EDEN_PS5_OPENGL)
            set(launcher_sources
                "${EDEN_PORT_DIR}/multiplayer_directory.cpp"
                "${launcher_dir}/frontend.cpp"
                "${launcher_dir}/eden_services.cpp"
                "${launcher_dir}/pe/audio/mixer.cpp"
                "${launcher_dir}/pe/audio/sounds.cpp"
                "${launcher_dir}/pe/audio/wav.cpp"
                "${launcher_dir}/pe/core/strings.cpp"
                "${launcher_dir}/pe/gfx/bidi.cpp"
                "${launcher_dir}/pe/gfx/draw_list.cpp"
                "${launcher_dir}/pe/gfx/font.cpp"
                "${launcher_dir}/pe/gfx/gl_batch.cpp"
                "${launcher_dir}/pe/gfx/gl_program.cpp"
                "${launcher_dir}/pe/gfx/harfbuzz.cpp"
                "${launcher_dir}/pe/gfx/image.cpp"
                "${launcher_dir}/pe/gfx/system_fonts.cpp"
                "${launcher_dir}/pe/platform/audio_out.cpp"
                "${launcher_dir}/pe/platform/display_egl.cpp"
                "${launcher_dir}/pe/ui/browse.cpp"
                "${launcher_dir}/pe/ui/game_options.cpp"
                "${launcher_dir}/pe/ui/profiles.cpp"
                "${launcher_dir}/pe/ui/remote.cpp"
                "${launcher_dir}/pe/ui/save_sync.cpp"
                "${launcher_dir}/pe/ui/pairing.cpp"
                "${launcher_dir}/pe/ui/multiplayer.cpp"
                "${launcher_dir}/pe/ui/qr_code.cpp"
                "${launcher_dir}/pe/ui/update.cpp"
                "${launcher_dir}/pe/ui/home.cpp"
                "${launcher_dir}/pe/ui/launcher.cpp"
                "${launcher_dir}/pe/ui/library.cpp"
                "${launcher_dir}/pe/ui/settings.cpp"
                "${launcher_dir}/pe/ui/textures.cpp"
                "${launcher_dir}/pe/ui/widgets.cpp")
            target_sources(eden-headless PRIVATE ${launcher_sources})
            # Download sources and their queue (remote/remote.h), with their backends (remote/source.h,
            # backends.h) and the console's FTP server that writes the files (remote/ftp.h); their
            # HTTP (remote/http.c) is linked into the update check's object below, with its libcurl.
            target_sources(eden-headless PRIVATE
                "${EDEN_PORT_DIR}/remote/remote.cpp"
                "${EDEN_PORT_DIR}/remote/backends.cpp"
                "${EDEN_PORT_DIR}/remote/ftp.cpp"
                "${EDEN_PORT_DIR}/remote/stream_check.cpp"
                "${EDEN_PORT_DIR}/remote/save_archive.cpp"
                "${EDEN_PORT_DIR}/remote/save_sync.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_client.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_saves.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_source.cpp")
            # The save data's zips (remote/save_archive.h): miniz, as the self-update helper has it.
            add_library(prosperoeden-miniz STATIC
                "${PORT_ROOT}/third_party/miniz/miniz.c"
                "${PORT_ROOT}/third_party/miniz/miniz_tdef.c"
                "${PORT_ROOT}/third_party/miniz/miniz_tinfl.c"
                "${PORT_ROOT}/third_party/miniz/miniz_zip.c")
            target_include_directories(prosperoeden-miniz PUBLIC "${PORT_ROOT}/third_party/miniz")
            target_compile_options(prosperoeden-miniz PRIVATE -w)
            target_link_libraries(eden-headless PRIVATE prosperoeden-miniz)
            # Their MD5 is OpenSSL's (linked below for the update check), as is the SHA-256 a
            # download's contents are checked with (remote/stream_check.h). Its include folder, for
            # these files only: the prefix it is in has older FFmpeg headers as well.
            set_source_files_properties("${EDEN_PORT_DIR}/remote/save_archive.cpp"
                "${EDEN_PORT_DIR}/remote/stream_check.cpp"
                PROPERTIES INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
            # The update check (update_notice.h). Its network side is the boilerplate's
            # update_check.c and console_curl.c with libcurl and libpsl from the pinned PacBrew
            # prefix that already supplies OpenSSL and zlib (zstd is Eden's own). They are linked
            # into one object first, and what console_curl.c puts in place of libc for libcurl
            # (name lookup, fcntl on sockets, a few more functions) gets a name of its own there
            # (console_curl_getaddrinfo, ...): the emulator's network and file code keeps the
            # functions it had, so the update check changes nothing else. The boilerplate wraps
            # fcntl for the whole program (--wrap=fcntl); here only this object's calls go
            # through the wrapper.
            get_filename_component(update_libraries "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
            if(NOT EXISTS "${update_libraries}/libcurl.a" OR NOT EXISTS "${update_libraries}/libpsl.a")
                message(FATAL_ERROR "libcurl is missing from the PacBrew prefix (${update_libraries}): run make deps")
            endif()
            find_program(UPDATE_CHECK_LINKER NAMES ld.lld-18 ld.lld REQUIRED)
            find_program(UPDATE_CHECK_OBJCOPY NAMES llvm-objcopy-18 llvm-objcopy REQUIRED)
            add_library(update-check OBJECT
                "${EDEN_PORT_DIR}/update_check/update_check.c"
                "${EDEN_PORT_DIR}/update_check/console_curl.c"
                "${EDEN_PORT_DIR}/update_check/self_update.c"
                "${EDEN_PORT_DIR}/update_check/self_update_ps5.c"
                "${EDEN_PORT_DIR}/update_check/self_update_sha256.c"
                "${EDEN_PORT_DIR}/remote/http.c")
            target_include_directories(update-check PRIVATE "${OPENSSL_INCLUDE_DIR}" "${EDEN_PORT_DIR}/update_check")
            # The kit's paths are the app's own: after elevation /app0 and /download0 are gone
            # (update_notice.cpp gives them).
            target_compile_definitions(update-check PRIVATE CURL_STATICLIB=1
                "SELF_UPDATE_HELPER_PATH=eden_self_update_path(0)"
                "SELF_UPDATE_PARAM_PATH=eden_self_update_path(1)"
                "SELF_UPDATE_SEQUENCE_PATH=eden_self_update_path(2)")
            set_source_files_properties("${EDEN_PORT_DIR}/update_check/self_update_ps5.c"
                PROPERTIES COMPILE_OPTIONS "-include;${EDEN_PORT_DIR}/update_check/eden_paths.h")
            # Every function console_curl.c defines under a libc name.
            set(update_own
                getaddrinfo freeaddrinfo gai_strerror gethostbyname getnameinfo fnmatch getpwuid_r
                _setjmp _longjmp openlog closelog dladdr if_nametoindex pipe2 recvmmsg sendmmsg
                popen pclose isatty mkstemp gmtime_r ZSTD_trace_compress_begin
                ZSTD_trace_compress_end ZSTD_trace_decompress_begin ZSTD_trace_decompress_end)
            set(update_rename --redefine-sym fcntl=__wrap_fcntl --redefine-sym __real_fcntl=fcntl)
            foreach(name IN LISTS update_own)
                list(APPEND update_rename --redefine-sym "${name}=console_curl_${name}")
            endforeach()
            set(update_object "${CMAKE_CURRENT_BINARY_DIR}/update-check.o")
            add_custom_command(OUTPUT "${update_object}"
                COMMAND "${UPDATE_CHECK_LINKER}" -r -o "${update_object}.all"
                    "$<TARGET_OBJECTS:update-check>"
                    "${update_libraries}/libcurl.a" "${update_libraries}/libpsl.a"
                COMMAND "${UPDATE_CHECK_OBJCOPY}" ${update_rename}
                    "${update_object}.all" "${update_object}"
                DEPENDS update-check "$<TARGET_OBJECTS:update-check>"
                    "${update_libraries}/libcurl.a" "${update_libraries}/libpsl.a"
                COMMAND_EXPAND_LISTS VERBATIM)
            set_source_files_properties("${update_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
            target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/update_notice.cpp" "${update_object}")
            target_link_libraries(eden-headless PRIVATE
                "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}" "${ZLIB_LIBRARY}")
            # The launcher calls OpenGL by name (the SDK's headers), not through glad.
            set(launcher_definitions "GL_GLEXT_PROTOTYPES=1")
            if(EXISTS "${EDEN_PORT_DIR}/ryujinx_saves.h")
                list(APPEND launcher_definitions "EDEN_SAVE_IMPORT=1")
            endif()
            set_source_files_properties(${launcher_sources}
                PROPERTIES COMPILE_DEFINITIONS "${launcher_definitions}")
            # Text in the scripts the baked font lacks: the console's own fonts, shaped by HarfBuzz
            # (a pinned source release, tools/deps.json) and drawn through stb_truetype.
            set(harfbuzz_dir "${PORT_ROOT}/.deps/harfbuzz-12.3.2/src")
            if(NOT EXISTS "${harfbuzz_dir}/harfbuzz.cc")
                message(FATAL_ERROR "HarfBuzz is missing (${harfbuzz_dir}): run make deps")
            endif()
            set_source_files_properties(
                "${launcher_dir}/pe/gfx/system_fonts.cpp" "${launcher_dir}/pe/gfx/harfbuzz.cpp"
                PROPERTIES INCLUDE_DIRECTORIES "${harfbuzz_dir};${PORT_ROOT}/tools/launcher/stb")
            # HarfBuzz is not ours to tidy: its warnings are off.
            set_source_files_properties("${launcher_dir}/pe/gfx/harfbuzz.cpp" PROPERTIES COMPILE_OPTIONS "-w")
        else()
            target_sources(eden-headless PRIVATE "${launcher_dir}/frontend_stub.cpp")
        endif()
        target_link_libraries(eden-headless PRIVATE eden-metadata)
    endif()

if(EDEN_PS5_VULKAN)
    # Both backends import the same system module; use the superset facade.
    get_target_property(gl_imports PS5OpenGL::OpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER gl_imports EXCLUDE REGEX "/libSceAgcDriver[.]so$")
    list(APPEND gl_imports "${PORT_ROOT}/build/stubs/libSceAgcDriver.so")
    set_property(TARGET PS5OpenGL::OpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${gl_imports}")
    target_compile_definitions(eden-headless PRIVATE EDEN_PS5_VULKAN=1)
    if(EDEN_VULKAN_DRIVER STREQUAL "RADV")
        target_compile_definitions(eden-headless PRIVATE EDEN_PS5_RADV=1)
    endif()
    target_include_directories(eden-headless BEFORE PRIVATE "${PORT_BUILD_DIR}/include")
endif()

option(EDEN_DEV_VULKAN "Force Vulkan for a development autoboot without changing preferences" OFF)
if(EDEN_DEV_VULKAN)
    if(NOT EDEN_PS5_VULKAN OR EDEN_DEV_ROM_ID STREQUAL "")
        message(FATAL_ERROR "EDEN_DEV_VULKAN requires Vulkan and an explicit development ROM ID")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_VULKAN=1)
endif()
