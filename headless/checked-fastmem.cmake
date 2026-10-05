# Checked A32 fastmem. HostMemory (src/host_memory.cpp) keeps one byte per 4 KiB guest
# page directly below the fastmem window: bit 0 blocks direct reads, bit 1 direct writes,
# each also set when the following page is blocked, so a direct access never crosses into
# a blocked page. A32 loads/stores test that byte, access [r13 + vaddr] when clear and
# otherwise run the checked page-table lookup out of line. Unaliased, GPU-tracked and
# read-only pages therefore never fault; the fault handler only covers races with concurrent
# mapping changes. An access site that keeps finding its page blocked (the window cannot alias
# memory whose backing is out of step with 16 KiB chunks, such as thread stacks and TLS) is
# recompiled onto the inline page-table path after checked_site_budget blocked executions,
# the same way a faulting site is, so such sites cost what they cost without fastmem. Exclusive accesses keep the page-table path. A64 is
# unchanged. Operates on memory_emitter (emit_x64_memory.h) and memory_source (.cpp.inc).

set(checked_helpers [=[
// Checked A32 fastmem: see headless/checked-fastmem.cmake.
constexpr int checked_map_bytes = 1 << 20;
constexpr u8 checked_read_blocked = 1, checked_write_blocked = 2;

inline void EmitCheckedFastmemTest(BlockOfCode& code, Xbyak::Reg64 vaddr, Xbyak::Reg64 index, u8 blocked_bit, Xbyak::Label& blocked) {
    code.mov(index.cvt32(), vaddr.cvt32());
    code.shr(index.cvt32(), int(page_table_const_bits));
    code.test(code.byte[r13 + index - checked_map_bytes], blocked_bit);
    code.jnz(blocked, code.T_NEAR);
}

// EmitVAddrLookup<A32EmitContext> for code inside a deferred block (instantiated for A32
// only): the misalignment check is inline because deferred emits cannot register more.
template<typename EmitContext>
Xbyak::RegExp EmitCheckedFallbackLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page) {
    ASSERT(ctx.conf.absolute_offset_page_table);
    if (bitsize != 8 && (ctx.conf.detect_misaligned_access_via_page_table & bitsize) != 0) {
        const u32 align_mask = u32(bitsize / 8 - 1);
        Xbyak::Label aligned;
        code.test(vaddr, align_mask);
        code.jz(aligned, code.T_NEAR);
        if (ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
            const u32 page_align_mask = u32(page_table_const_size - 1) & ~align_mask;
            code.mov(page, vaddr);
            code.and_(page, page_align_mask);
            code.cmp(page, page_align_mask);
            code.je(abort, code.T_NEAR);
        } else {
            code.jmp(abort, code.T_NEAR);
        }
        code.L(aligned);
    }
    code.mov(page, vaddr);
    code.shr(page, int(page_table_const_bits));
    if (ctx.conf.page_table_log2_stride <= 3) {
        code.mov(page, qword[r14 + page * (1 << ctx.conf.page_table_log2_stride)]);
    } else {
        code.shl(page, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + page]);
    }
    if (ctx.conf.page_table_marked_bit) {
        if (*ctx.conf.page_table_marked_bit == 0) {
            code.test(page.cvt32(), 1);
            code.jnz(abort, code.T_NEAR);
        } else {
            code.bt(page, *ctx.conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }
    }
    if (ctx.conf.page_table_pointer_mask == 0x00fffffffffff000ULL && ctx.conf.page_table_sign_extension == 8) {
        code.shl(page, 8);
        code.sar(page, 8);
        code.and_(page, -4096);
    } else {
        ASSERT(ctx.conf.page_table_pointer_mask == 0 || std::in_range<s32>(ctx.conf.page_table_pointer_mask));
        if (ctx.conf.page_table_pointer_mask == 0) {
            code.test(page, page);
        } else {
            code.and_(page, ctx.conf.page_table_pointer_mask);
        }
        if (ctx.conf.page_table_sign_extension) {
            code.shl(page, *ctx.conf.page_table_sign_extension);
            code.sar(page, *ctx.conf.page_table_sign_extension);
        }
    }
    code.jz(abort, code.T_NEAR);
    return page + vaddr;
}

]=])
set(checked_anchor "template<typename EmitContext>\nXbyak::RegExp EmitFastmemVAddr")
string(FIND "${memory_emitter}" "${checked_anchor}" checked_at)
if(checked_at LESS 0)
    message(FATAL_ERROR "Pinned fastmem address emitter changed")
endif()
string(REPLACE "${checked_anchor}" "${checked_helpers}${checked_anchor}" memory_emitter "${memory_emitter}")

# Loads and stores: checked direct access for A32, upstream fastmem otherwise.
foreach(direction Read Write)
    if(direction STREQUAL "Read")
        set(pointer src_ptr)
        set(blocked_bit checked_read_blocked)
        set(direct_access "EmitReadMemoryMov<bitsize>(code, value_idx, r13 + vaddr, ordered)")
        set(fallback_access "EmitReadMemoryMov<bitsize>(code, value_idx, fallback_ptr, ordered)")
    else()
        set(pointer dest_ptr)
        set(blocked_bit checked_write_blocked)
        set(direct_access "EmitWriteMemoryMov<bitsize>(code, r13 + vaddr, value_idx, ordered)")
        set(fallback_access "EmitWriteMemoryMov<bitsize>(code, fallback_ptr, value_idx, ordered)")
    endif()
    set(open_old "    if (fastmem_marker) {\n        // Use fastmem\n        bool require_abort_handling = false;\n        const auto ${pointer} = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);\n")
    set(close_old "        });\n    } else {\n        // Use page table\n        ASSERT(conf.page_table);\n        const auto ${pointer} = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);\n")
    foreach(anchor open_old close_old)
        string(FIND "${memory_source}" "${${anchor}}" anchor_at)
        if(anchor_at LESS 0)
            message(FATAL_ERROR "Pinned fastmem memory ${direction} emitter changed")
        endif()
    endforeach()
    string(REPLACE "${open_old}" "    if (fastmem_marker) {
        // Use fastmem
        if constexpr (checked_fastmem) {
            const Xbyak::Reg64 index = ctx.reg_alloc.ScratchGpr(code);
            SharedLabel blocked = ctx.GenSharedLabel(), slow = ctx.GenSharedLabel(), demote = ctx.GenSharedLabel();
            CheckedSite* const site = conf.recompile_on_fastmem_failure ? NewCheckedSite(*fastmem_marker) : nullptr;
            EmitCheckedFastmemTest(code, vaddr, index, ${blocked_bit}, *blocked);
            const auto location = ${direct_access};
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                EmitCheckedSiteCount(code, site, index, *demote);
                code.L(*slow);
                const auto fallback_ptr = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, index);
                ${fallback_access};
                code.jmp(*end, code.T_NEAR);
                code.L(*abort);
                code.call(wrapped_fn);
                fastmem_patch_info.emplace(
                    std::bit_cast<u64>(location),
                    FastmemPatchInfo{
                        std::bit_cast<u64>(code.getCurr()),
                        std::bit_cast<u64>(wrapped_fn),
                        *fastmem_marker,
                        conf.recompile_on_fastmem_failure,
                    });
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
                EmitCheckedSiteDemote(code, this, site, *demote, *slow);
            });
        } else {
        bool require_abort_handling = false;
        const auto ${pointer} = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);
" memory_source "${memory_source}")
    string(REPLACE "${close_old}" "        });\n        }\n    } else {\n        // Use page table\n        ASSERT(conf.page_table);\n        const auto ${pointer} = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);\n"
        memory_source "${memory_source}")
endforeach()

# FP/SIMD scalar accesses (VLDR/VSTR): checked direct access straight between the window
# and an XMM register, like the port's page-table scalar path (scalar-memory-*.inc).
set(vec_read [=[
    if constexpr (checked_fastmem && (bitsize == 32 || bitsize == 64)) {
        if (fastmem_marker && args[2].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.ScratchXmm(code);
            const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel blocked = ctx.GenSharedLabel(), slow = ctx.GenSharedLabel(), demote = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            CheckedSite* const site = conf.recompile_on_fastmem_failure ? NewCheckedSite(*fastmem_marker) : nullptr;
            EmitCheckedFastmemTest(code, vaddr, temporary, checked_read_blocked, *blocked);
            const auto location = code.getCurr();
            if constexpr (bitsize == 32) code.movd(vector, code.dword[r13 + vaddr]);
            else code.movq(vector, code.qword[r13 + vaddr]);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                EmitCheckedSiteCount(code, site, temporary, *demote);
                code.L(*slow);
                const auto pointer = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, temporary);
                if constexpr (bitsize == 32) code.movd(vector, code.dword[pointer]);
                else code.movq(vector, code.qword[pointer]);
                code.jmp(*end, code.T_NEAR);
                code.L(*abort);
                code.call(wrapped_fn);
                // A faulting direct load also returns here, with the value in temporary.
                fastmem_patch_info.emplace(
                    std::bit_cast<u64>(location),
                    FastmemPatchInfo{
                        std::bit_cast<u64>(code.getCurr()),
                        std::bit_cast<u64>(wrapped_fn),
                        *fastmem_marker,
                        conf.recompile_on_fastmem_failure,
                    });
                if constexpr (bitsize == 32) code.movd(vector, temporary.cvt32());
                else code.movq(vector, temporary);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
                EmitCheckedSiteDemote(code, this, site, *demote, *slow);
            });
            code.L(*end);
            ctx.reg_alloc.DefineValue(code, inst, vector);
            return;
        }
    }
]=])
set(vec_write [=[
    if constexpr (checked_fastmem && (bitsize == 32 || bitsize == 64)) {
        if (fastmem_marker && args[3].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.UseXmm(code, args[2]);
            const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel blocked = ctx.GenSharedLabel(), slow = ctx.GenSharedLabel(), demote = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            CheckedSite* const site = conf.recompile_on_fastmem_failure ? NewCheckedSite(*fastmem_marker) : nullptr;
            EmitCheckedFastmemTest(code, vaddr, temporary, checked_write_blocked, *blocked);
            const auto location = code.getCurr();
            if constexpr (bitsize == 32) code.movd(code.dword[r13 + vaddr], vector);
            else code.movq(code.qword[r13 + vaddr], vector);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                EmitCheckedSiteCount(code, site, temporary, *demote);
                code.L(*slow);
                const auto pointer = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, temporary);
                if constexpr (bitsize == 32) code.movd(code.dword[pointer], vector);
                else code.movq(code.qword[pointer], vector);
                code.jmp(*end, code.T_NEAR);
                // A faulting direct store enters here: the fallback takes its value in temporary.
                const auto fault_entry = code.getCurr();
                if constexpr (bitsize == 32) code.movd(temporary.cvt32(), vector);
                else code.movq(temporary, vector);
                code.jmp(reinterpret_cast<const void*>(wrapped_fn), code.T_NEAR);
                code.L(*abort);
                if constexpr (bitsize == 32) code.movd(temporary.cvt32(), vector);
                else code.movq(temporary, vector);
                code.call(wrapped_fn);
                fastmem_patch_info.emplace(
                    std::bit_cast<u64>(location),
                    FastmemPatchInfo{
                        std::bit_cast<u64>(code.getCurr()),
                        std::bit_cast<u64>(fault_entry),
                        *fastmem_marker,
                        conf.recompile_on_fastmem_failure,
                    });
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
                EmitCheckedSiteDemote(code, this, site, *demote, *slow);
            });
            code.L(*end);
            return;
        }
    }
]=])
foreach(direction Read Write)
    if(direction STREQUAL "Read")
        set(vec_anchor "    if constexpr (bitsize == 32 || bitsize == 64) {\n        if (!fastmem_marker && args[2].GetImmediateAccType() == IR::AccType::VEC) {")
        set(vec_block "${vec_read}")
    else()
        set(vec_anchor "    if constexpr (bitsize == 32 || bitsize == 64) {\n        if (!fastmem_marker && args[3].GetImmediateAccType() == IR::AccType::VEC) {")
        set(vec_block "${vec_write}")
    endif()
    string(FIND "${memory_source}" "${vec_anchor}" vec_at)
    if(vec_at LESS 0)
        message(FATAL_ERROR "Pinned scalar ${direction} path changed")
    endif()
    string(REPLACE "${vec_anchor}" "${vec_block}${vec_anchor}" memory_source "${memory_source}")
endforeach()

# Exclusive accesses keep the page-table inline path (EmitExclusiveVAddrLookup).
set(exclusive_marker "    const auto fastmem_marker = ShouldFastmem(ctx, inst);\n")
string(FIND "${memory_source}" "void AxxEmitX64::EmitExclusiveReadMemoryInline" checked_exclusive_at)
if(checked_exclusive_at LESS 0)
    message(FATAL_ERROR "Pinned exclusive emitter changed")
endif()
string(SUBSTRING "${memory_source}" 0 ${checked_exclusive_at} checked_prefix)
string(SUBSTRING "${memory_source}" ${checked_exclusive_at} -1 checked_exclusive)
string(FIND "${checked_exclusive}" "${exclusive_marker}" exclusive_marker_at)
if(exclusive_marker_at LESS 0)
    message(FATAL_ERROR "Pinned exclusive fastmem selection changed")
endif()
string(REPLACE "${exclusive_marker}"
    "    const auto fastmem_marker = checked_fastmem ? decltype(ShouldFastmem(ctx, inst)){} : ShouldFastmem(ctx, inst);\n"
    checked_exclusive "${checked_exclusive}")
set(memory_source "${checked_prefix}${checked_exclusive}")

# Blocked-site budget (see the top of this file). The .inc is compiled once for A32 and once
# for A64; only A32 emits these.
set(checked_sites [=[
// Checked fastmem: one record per access site, allocated when the site is emitted and kept
// for the session. The JIT counts the site's blocked executions down in place.
void CheckedFastmemDemoted() noexcept; // headless/fastmem_handler.cpp
namespace {
constexpr u8 checked_site_budget = 64;
struct CheckedSite {
    u64 location;
    unsigned name;
    u8 budget;
    bool demoted;
};
[[maybe_unused]] CheckedSite* NewCheckedSite(const std::tuple<IR::LocationDescriptor, unsigned>& marker) {
    return new CheckedSite{std::get<0>(marker).Value(), std::get<1>(marker), checked_site_budget, false};
}
// In a site's out-of-line path, before its page-table lookup: count one blocked execution.
[[maybe_unused]] void EmitCheckedSiteCount(BlockOfCode& code, CheckedSite* site, Xbyak::Reg64 scratch, Xbyak::Label& demote) {
    if (!site) return;
    code.mov(scratch, std::bit_cast<u64>(site));
    code.dec(code.byte[scratch + offsetof(CheckedSite, budget)]);
    code.jz(demote, code.T_NEAR);
}
// Recompile the site without fastmem, as FastmemCallback does after a fault; the current
// block finishes on its lookup. Called once the budget runs out (and again, harmlessly,
// every 256 blocked executions after that until the block is replaced).
template<typename Emitter>
void CheckedSiteDemote(Emitter* emitter, CheckedSite* site) {
    if (site->demoted) return;
    site->demoted = true;
    const IR::LocationDescriptor location{site->location};
    emitter->do_not_fastmem.insert(std::make_tuple(location, site->name));
    emitter->InvalidateBasicBlocks({location});
    CheckedFastmemDemoted();
}
// The call goes through a local thunk so the caller-saved spill sees the stack as a callee
// does, like the access fallbacks; afterwards the access continues with its lookup.
template<typename Emitter>
void EmitCheckedSiteDemote(BlockOfCode& code, Emitter* emitter, CheckedSite* site, Xbyak::Label& demote, Xbyak::Label& resume) {
    if (!site) return;
    Xbyak::Label thunk;
    code.L(demote);
    code.call(thunk);
    code.jmp(resume, code.T_NEAR);
    code.L(thunk);
    ABI_PushCallerSaveRegistersAndAdjustStack(code);
    code.mov(code.ABI_PARAM1, std::bit_cast<u64>(emitter));
    code.mov(code.ABI_PARAM2, std::bit_cast<u64>(site));
    code.CallFunction(&CheckedSiteDemote<Emitter>);
    ABI_PopCallerSaveRegistersAndAdjustStack(code);
    code.ret();
}
} // namespace
]=])
# The .inc is compiled once for A32 and once for A64.
set(checked_select "namespace {\nusing Vector = std::array<u64, 2>;\n}\n")
string(FIND "${memory_source}" "${checked_select}" checked_select_at)
if(checked_select_at LESS 0)
    message(FATAL_ERROR "Pinned memory emitter prologue changed")
endif()
string(REPLACE "${checked_select}"
    "${checked_select}\nconstexpr bool checked_fastmem = std::is_same_v<AxxEmitContext, A32EmitContext>;\n${checked_sites}"
    memory_source "${memory_source}")
