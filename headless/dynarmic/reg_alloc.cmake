# ProsperoEden: register-allocator bookkeeping in the x64 JIT emitters (compile time only).
# Upstream releases all 102 host locations after every IR instruction (EndOfAllocScope) and scans
# all of them for every value lookup (ValueLocation): together about a quarter of block compile
# time on the console. The allocator now records the locations changed since the last scope end
# (through the non-const LocInfo) and those that held values at that point, and visits only those.
# Releasing an unchanged location changes nothing (it was released at the scope end after its last
# change, and ReleaseAll is idempotent), and lookups keep upstream's ascending order, so every
# allocation decision and the emitted code stay the same. Host builds check both claims at every
# step (EDEN_REGALLOC_VERIFY).

function(eden_reg_alloc_require source anchor what)
    string(FIND "${source}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned register allocator changed (${what})")
    endif()
endfunction()

macro(eden_reg_alloc_sources)
    set(eden_ra_dir "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/x64")
    file(READ "${eden_ra_dir}/reg_alloc.h" eden_ra_header)
    set(eden_ra_includes [==[#include <array>
]==])
    set(eden_ra_scope [==[    inline void EndOfAllocScope() noexcept {
        for (auto& iter : hostloc_info)
            iter.ReleaseAll();
    }
]==])
    set(eden_ra_locinfo [==[    inline HostLocInfo& LocInfo(const HostLoc loc) noexcept {
        DEBUG_ASSERT(loc != HostLoc::RSP && loc != ABI_JIT_PTR);
        return hostloc_info[size_t(loc)];
    }
]==])
    set(eden_ra_members [==[    size_t reserved_stack_space = 0;
};
]==])
    foreach(anchor eden_ra_includes eden_ra_scope eden_ra_locinfo eden_ra_members)
        eden_reg_alloc_require("${eden_ra_header}" "${${anchor}}" "reg_alloc.h ${anchor}")
    endforeach()
    string(REPLACE "${eden_ra_includes}" [==[#include <array>
#include <bit>
#include <utility>
]==] eden_ra_header "${eden_ra_header}")
    string(REPLACE "${eden_ra_scope}" [==[    inline void EndOfAllocScope() noexcept {
        // ProsperoEden: only the locations changed since the last scope end (reg_alloc.cmake).
#ifdef EDEN_REGALLOC_VERIFY
        EdenVerifyReleased();
#endif
        for (size_t word = 0; word < eden_touched.size(); ++word) {
            for (u64 bits = std::exchange(eden_touched[word], 0); bits != 0; bits &= bits - 1) {
                const size_t i = word * 64 + size_t(std::countr_zero(bits));
                hostloc_info[i].ReleaseAll();
                const u64 bit = u64(1) << (i % 64);
                eden_live[word] = hostloc_info[i].IsEmpty() ? (eden_live[word] & ~bit) : (eden_live[word] | bit);
            }
        }
    }
]==] eden_ra_header "${eden_ra_header}")
    string(REPLACE "${eden_ra_locinfo}" [==[    inline HostLocInfo& LocInfo(const HostLoc loc) noexcept {
        DEBUG_ASSERT(loc != HostLoc::RSP && loc != ABI_JIT_PTR);
        eden_touched[size_t(loc) / 64] |= u64(1) << (size_t(loc) % 64);
        return hostloc_info[size_t(loc)];
    }
]==] eden_ra_header "${eden_ra_header}")
    string(REPLACE "${eden_ra_members}" [==[    size_t reserved_stack_space = 0;
    // ProsperoEden: locations changed since the last EndOfAllocScope, and locations that held
    // values at that point; no other location can hold a value (reg_alloc.cmake).
    static constexpr size_t eden_location_words = (NonSpillHostLocCount + SpillCount + 63) / 64;
    std::array<u64, eden_location_words> eden_touched{};
    std::array<u64, eden_location_words> eden_live{};
#ifdef EDEN_REGALLOC_VERIFY
    // Every location outside the changed set is already in its released state, so upstream's
    // ReleaseAll would leave it unchanged, and its live bit matches its values.
    void EdenVerifyReleased() const noexcept {
        for (size_t i = 0; i < hostloc_info.size(); ++i) {
            if ((eden_touched[i / 64] >> (i % 64)) & 1)
                continue;
            const HostLocInfo& info = hostloc_info[i];
            ASSERT(info.current_references == 0 && info.is_being_used_count == 0 && !info.is_scratch && !info.is_set_last_use);
            ASSERT(info.total_uses != info.accumulated_uses || (info.values.empty() && info.total_uses == 0 && info.max_bit_width == 0));
            ASSERT(bool((eden_live[i / 64] >> (i % 64)) & 1) == !info.values.empty());
        }
    }
#endif
};
]==] eden_ra_header "${eden_ra_header}")
    write_derived("${PORT_BUILD_DIR}/include/dynarmic/backend/x64/reg_alloc.h" "${eden_ra_header}")

    file(READ "${eden_ra_dir}/reg_alloc.cpp" eden_ra_source)
    set(eden_ra_lookup [==[std::optional<HostLoc> RegAlloc::ValueLocation(const IR::Inst* value) const noexcept {
    for (size_t i = 0; i < hostloc_info.size(); i++)
        if (hostloc_info[i].ContainsValue(value)) {
            //for (size_t j = 0; j < hostloc_info.size(); ++j)
            //    ASSERT((i == j || !hostloc_info[j].ContainsValue(value)) && "duplicate defs");
            return HostLoc(i);
        }
    return std::nullopt;
}
]==])
    eden_reg_alloc_require("${eden_ra_source}" "${eden_ra_lookup}" "reg_alloc.cpp ValueLocation")
    string(REPLACE "${eden_ra_lookup}" [==[std::optional<HostLoc> RegAlloc::ValueLocation(const IR::Inst* value) const noexcept {
    // ProsperoEden: only locations that held values at the last scope end or changed since (no
    // other one can hold a value), in upstream's ascending order (reg_alloc.cmake).
    std::optional<HostLoc> found;
    for (size_t word = 0; word < eden_live.size() && !found; ++word) {
        for (u64 bits = eden_live[word] | eden_touched[word]; bits != 0; bits &= bits - 1) {
            const size_t i = word * 64 + size_t(std::countr_zero(bits));
            if (hostloc_info[i].ContainsValue(value)) {
                found = HostLoc(i);
                break;
            }
        }
    }
#ifdef EDEN_REGALLOC_VERIFY
    std::optional<HostLoc> full;
    for (size_t i = 0; i < hostloc_info.size() && !full; i++)
        if (hostloc_info[i].ContainsValue(value))
            full = HostLoc(i);
    ASSERT(full == found);
#endif
    return found;
}
]==] eden_ra_source "${eden_ra_source}")
    # A register an emitter needs (RAX for the flags of a compare, the ABI registers of a call...)
    # that still holds a value has it moved out of the way. Upstream always moves it to a stack
    # slot, and the next use loads it back: in a large game's hottest blocks, stack spill traffic
    # was about 8% of the samples. A general-purpose value now goes to a free general-purpose
    # register when there is one (a register move, which Zen 2 eliminates at rename), callee-saved
    # ones first so a host call's caller-saved sweep does not move it again; the stack otherwise,
    # as before. SelectARegister already places values in any free register, so this adds no new
    # kind of placement. dev-settings jit_free_moves=off restores upstream's spill for A/B runs.
    set(eden_ra_move [==[void RegAlloc::MoveOutOfTheWay(BlockOfCode& code, HostLoc reg) noexcept {
    ASSERT(!LocInfo(reg).IsLocked());
    if (!LocInfo(reg).IsEmpty()) {
        SpillRegister(code, reg);
    }
}
]==])
    eden_reg_alloc_require("${eden_ra_source}" "${eden_ra_move}" "reg_alloc.cpp MoveOutOfTheWay")
    string(REPLACE "${eden_ra_move}" [==[extern "C" {
bool eden_free_moves = true;
bool eden_alloc_r13 = true;
unsigned long long eden_free_moves_done = 0, eden_free_moves_spilled = 0;
}

void RegAlloc::MoveOutOfTheWay(BlockOfCode& code, HostLoc reg) noexcept {
    ASSERT(!LocInfo(reg).IsLocked());
    if (!LocInfo(reg).IsEmpty()) {
        // ProsperoEden: a free general-purpose register before a stack slot (reg_alloc.cmake).
        // Only the emitter's GPR set (gpr_order); RBP, R14, R15 and RSP stay out, as in
        // SelectARegister, and R13 under the same switch as there.
        if (HostLocIsGPR(reg) && __atomic_load_n(&eden_free_moves, __ATOMIC_RELAXED)) {
            static constexpr HostLoc order[] = {
                HostLoc::RBX, HostLoc::R12, HostLoc::R13, HostLoc::RSI, HostLoc::RDI, HostLoc::R8, HostLoc::R9,
                HostLoc::R10, HostLoc::R11, HostLoc::RCX, HostLoc::RDX, HostLoc::RAX,
            };
            for (const HostLoc to : order) {
                if (to != reg && gpr_order.test(size_t(to)) && hostloc_info[size_t(to)].IsEmpty() &&
                    (to != HostLoc::R13 || __atomic_load_n(&eden_alloc_r13, __ATOMIC_RELAXED))) {
                    Move(code, to, reg);
                    __atomic_fetch_add(&eden_free_moves_done, 1, __ATOMIC_RELAXED);
                    return;
                }
            }
        }
        __atomic_fetch_add(&eden_free_moves_spilled, 1, __ATOMIC_RELAXED);
        SpillRegister(code, reg);
    }
}
]==] eden_ra_source "${eden_ra_source}")
    # R13 holds the fastmem base when a JIT has one; the A64 and A32 emitters then leave it out of
    # their GPR set, and otherwise keep it in. SelectARegister still skipped it in every case, so
    # A64 blocks (fastmem off on the console) had 11 allocatable GPRs instead of 12. Nothing else
    # writes R13 inside blocks with fastmem off (it is only loaded by GenRCP when fastmem is set,
    # and saved/restored with the other callee-saved registers around RunCode), so R13 is now
    # handed out whenever the requested set contains it. dev-settings jit_r13=off skips it again.
    set(eden_ra_select [==[            } else if (i == HostLoc::RBP || (i >= HostLoc::R13 && i <= HostLoc::R15)) {
]==])
    eden_reg_alloc_require("${eden_ra_source}" "${eden_ra_select}" "reg_alloc.cpp SelectARegister")
    string(REPLACE "${eden_ra_select}" [==[            } else if (i == HostLoc::RBP || (i >= HostLoc::R14 && i <= HostLoc::R15) ||
                       (i == HostLoc::R13 && !__atomic_load_n(&eden_alloc_r13, __ATOMIC_RELAXED))) {
]==] eden_ra_source "${eden_ra_source}")
    string(PREPEND eden_ra_source "extern \"C\" bool eden_alloc_r13;  // ProsperoEden (reg_alloc.cmake)\n")
    write_derived("${PORT_BUILD_DIR}/reg_alloc.cpp" "${eden_ra_source}")
    get_target_property(eden_ra_sources dynarmic SOURCES)
    list(FILTER eden_ra_sources EXCLUDE REGEX "(^|/)backend/x64/reg_alloc\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${eden_ra_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/reg_alloc.cpp")
    # The override header shadows upstream's only by include order: a definition on the target
    # recompiles every dynarmic object that saw the upstream layout.
    target_compile_definitions(dynarmic PRIVATE EDEN_REGALLOC_TRACKED=1)
    if(NOT PS5_NATIVE)
        target_compile_definitions(dynarmic PRIVATE EDEN_REGALLOC_VERIFY=1)
    endif()
endmacro()
