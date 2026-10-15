# One page lookup for nearby A64 accesses. With the JIT's direct page table (jit-page-table.cmake)
# every load and store still checks its alignment, range-checks and shifts its address, loads the
# table and tests the value: about ten x86 instructions before the access. In a game's hot blocks
# most accesses use the same register plus small constants ([x12, #8] ... [x12, #44]), and
# grouping them leaves a quarter of the lookups. The optimizer (page_reuse_pass.inc) gives each
# such group one A64EdenPageLookup of its lowest address and its span: the direct value when the
# whole span is in one page, 0 otherwise. The group's accesses take that value in place of their
# location argument and emit test, jz and the access; on 0 they run the full lookup out of line,
# and the callback only when that fails, as before. dev-settings jit_page_reuse=off keeps the
# per-access lookup for A/B runs.

# Every dynarmic object sees the added opcode (a definition on the target recompiles them all).
target_compile_definitions(dynarmic PRIVATE EDEN_PAGE_REUSE=1)

function(eden_page_reuse_opcodes)
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/ir/opcodes.inc" opcodes)
    set(anchor "A64OPC(ExclusiveWriteMemory128,")
    string(FIND "${opcodes}" "${anchor}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned A64 memory opcodes changed")
    endif()
    string(REPLACE "${anchor}" "A64OPC(EdenPageLookup,                                      U64,            U64,            U64                             )\n${anchor}"
        opcodes "${opcodes}")
    write_derived("${PORT_BUILD_DIR}/include/dynarmic/ir/opcodes.inc" "${opcodes}")
    # opcodes.h and opcodes.cpp include it by a relative path: overlay the header beside it and
    # compile a copy of the table that names the overlay.
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/ir/opcodes.h" opcodes_header)
    write_derived("${PORT_BUILD_DIR}/include/dynarmic/ir/opcodes.h" "${opcodes_header}")
    file(READ "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/ir/opcodes.cpp" opcodes_table)
    string(FIND "${opcodes_table}" "#include \"./opcodes.inc\"" at)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned opcode table changed")
    endif()
    string(REPLACE "#include \"./opcodes.inc\"" "#include \"dynarmic/ir/opcodes.inc\"" opcodes_table "${opcodes_table}")
    write_derived("${PORT_BUILD_DIR}/opcodes.cpp" "${opcodes_table}")
    get_target_property(jit_sources dynarmic SOURCES)
    list(FILTER jit_sources EXCLUDE REGEX "(^|/)ir/opcodes\\.cpp$")
    set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
    target_sources(dynarmic PRIVATE "${PORT_BUILD_DIR}/opcodes.cpp")
endfunction()

# Operates on optimizer (opt_passes.cpp).
macro(eden_page_reuse_optimizer)
    set(reuse_declarations "void Optimize(IR::Block& block, const A32::UserConfig& conf, const Optimization::PolyfillOptions& polyfill_options) {")
    set(reuse_call [=[    Optimization::IdentityRemovalPass(block);
    if (!conf.HasOptimization(OptimizationFlag::DisableVerification)) {
        Optimization::VerificationPass(block);
    }
}

}  // namespace Dynarmic::Optimization]=])
    foreach(anchor IN ITEMS reuse_declarations reuse_call)
        string(FIND "${optimizer}" "${${anchor}}" at)
        if(at LESS 0)
            message(FATAL_ERROR "Pinned optimizer changed (${anchor})")
        endif()
    endforeach()
    file(READ "${EDEN_PORT_DIR}/dynarmic/page_reuse_pass.inc" reuse_pass)
    string(REPLACE "${reuse_declarations}" "${reuse_pass}\n${reuse_declarations}" optimizer "${optimizer}")
    string(REPLACE "${reuse_call}" [=[    Optimization::IdentityRemovalPass(block);
    EdenPageReusePass(block, conf);
    if (!conf.HasOptimization(OptimizationFlag::DisableVerification)) {
        Optimization::VerificationPass(block);
    }
}

}  // namespace Dynarmic::Optimization]=] optimizer "${optimizer}")
    string(PREPEND optimizer "#include <optional>\n#include <vector>\n")
endmacro()

# Operates on memory_source (emit_x64_memory.cpp.inc), after the port's other changes to it.
macro(eden_page_reuse_memory_inc)
    set(reuse_helper [=[
// ProsperoEden: the full page-table lookup of a grouped access whose group lookup gave 0
// (headless/dynarmic/page-reuse.cmake), for code inside a deferred block: the direct table, its
// address space range-checked, misalignment detected only on page boundaries.
template<typename Config>
size_t EdenReuseAddressBits(const Config& config) {
    if constexpr (requires { config.page_table_address_space_bits; }) {
        return config.page_table_address_space_bits;
    } else {
        return 32;  // A32: never grouped
    }
}

inline void EdenReuseSlowLookup(BlockOfCode& code, size_t bitsize, bool detect, size_t valid_page_index_bits,
        Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page) {
    if (bitsize != 8 && detect) {
        const u32 align_mask = static_cast<u32>(bitsize / 8 - 1);
        const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;
        Xbyak::Label aligned;
        code.test(vaddr, align_mask);
        code.jz(aligned);
        code.mov(page, vaddr);
        code.and_(page, page_align_mask);
        code.cmp(page, page_align_mask);
        code.je(abort, code.T_NEAR);
        code.L(aligned);
    }
    code.mov(page, vaddr);
    code.shr(page, int(page_table_const_bits));
    code.test(page, u32(-(1 << valid_page_index_bits)));
    code.jnz(abort, code.T_NEAR);
    code.mov(page, code.qword[r14 + page * 8]);
    code.test(page, page);
    code.jz(abort, code.T_NEAR);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryRead(AxxEmitContext& ctx, IR::Inst* inst) {]=])
    set(read_anchor "template<std::size_t bitsize, auto callback>\nvoid AxxEmitX64::EmitMemoryRead(AxxEmitContext& ctx, IR::Inst* inst) {")
    set(read_site "    const bool ordered = IsOrdered(args[2].GetImmediateAccType());\n    const auto fastmem_marker = ShouldFastmem(ctx, inst);\n")
    set(write_site "    const bool ordered = IsOrdered(args[3].GetImmediateAccType());\n    const auto fastmem_marker = ShouldFastmem(ctx, inst);\n")
    foreach(anchor IN ITEMS read_anchor read_site write_site)
        string(FIND "${memory_source}" "${${anchor}}" at)
        if(at LESS 0)
            message(FATAL_ERROR "Pinned memory emitter changed (${anchor})")
        endif()
    endforeach()
    string(REPLACE "${read_anchor}" "${reuse_helper}" memory_source "${memory_source}")
    string(REPLACE "${read_site}" [=[    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // ProsperoEden: a grouped access (headless/dynarmic/page-reuse.cmake), its group's page in args[0].
    if (!args[0].IsImmediate()) {
        const bool vector = (bitsize == 32 || bitsize == 64) && args[2].GetImmediateAccType() == IR::AccType::VEC;
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const int value_idx = bitsize == 128 || vector ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
        const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), vector ? page.getIdx() : value_idx)];
        const bool detect = (conf.detect_misaligned_access_via_page_table & bitsize) != 0;
        const size_t valid_page_index_bits = EdenReuseAddressBits(conf) - page_table_const_bits;
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        const auto read = [this, vector, value_idx](const Xbyak::RegExp& address) {
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(Xbyak::Xmm{value_idx}, code.dword[address]);
                    else code.movq(Xbyak::Xmm{value_idx}, code.qword[address]);
                    return;
                }
            }
            EmitReadMemoryMov<bitsize>(code, value_idx, address, false);
        };
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        read(host + vaddr);
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*slow);
            EdenReuseSlowLookup(code, bitsize, detect, valid_page_index_bits, *abort, vaddr, page);
            read(page + vaddr);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            code.call(wrapped_fn);
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(Xbyak::Xmm{value_idx}, page.cvt32());
                    else code.movq(Xbyak::Xmm{value_idx}, page);
                }
            }
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        if (bitsize == 128 || vector) {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
        } else {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
        }
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);
]=] memory_source "${memory_source}")
    string(REPLACE "${write_site}" [=[    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    // ProsperoEden: a grouped access (headless/dynarmic/page-reuse.cmake), its group's page in args[0].
    if (!args[0].IsImmediate()) {
        const bool vector = (bitsize == 32 || bitsize == 64) && args[3].GetImmediateAccType() == IR::AccType::VEC;
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const int value_idx = bitsize == 128 || vector ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx();
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), vector ? page.getIdx() : value_idx)];
        const bool detect = (conf.detect_misaligned_access_via_page_table & bitsize) != 0;
        const size_t valid_page_index_bits = EdenReuseAddressBits(conf) - page_table_const_bits;
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        const auto write = [this, vector, value_idx](const Xbyak::RegExp& address) {
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(code.dword[address], Xbyak::Xmm{value_idx});
                    else code.movq(code.qword[address], Xbyak::Xmm{value_idx});
                    return;
                }
            }
            EmitWriteMemoryMov<bitsize>(code, address, value_idx, false);
        };
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        write(host + vaddr);
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*slow);
            EdenReuseSlowLookup(code, bitsize, detect, valid_page_index_bits, *abort, vaddr, page);
            write(page + vaddr);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(page.cvt32(), Xbyak::Xmm{value_idx});
                    else code.movq(page, Xbyak::Xmm{value_idx});
                }
            }
            code.call(wrapped_fn);
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);
]=] memory_source "${memory_source}")
endmacro()

# Operates on memory_source (a64_emit_x64_memory.cpp).
macro(eden_page_reuse_memory_unit)
    set(unit_end "}  // namespace Dynarmic::Backend::X64")
    string(FIND "${memory_source}" "${unit_end}" at REVERSE)
    if(at LESS 0)
        message(FATAL_ERROR "Pinned A64 memory emitter unit changed")
    endif()
    string(SUBSTRING "${memory_source}" 0 ${at} unit_prefix)
    string(SUBSTRING "${memory_source}" ${at} -1 unit_suffix)
    set(unit_lookup [=[
// ProsperoEden: the direct-table value for a group of accesses (headless/dynarmic/page-reuse.cmake):
// 0 when its span crosses a page boundary or leaves the address space, or when the page must
// take the callback path.
void A64EmitX64::EmitA64EdenPageLookup(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 span = static_cast<u32>(args[1].GetImmediateU64());
    const Xbyak::Reg64 host = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    const Xbyak::Reg64 offset = ctx.reg_alloc.ScratchGpr(code);
    const size_t valid_page_index_bits = conf.page_table_address_space_bits - page_table_const_bits;
    SharedLabel none = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
    code.mov(offset.cvt32(), host.cvt32());
    code.and_(offset.cvt32(), static_cast<u32>(page_table_const_size - 1));
    code.cmp(offset.cvt32(), static_cast<u32>(page_table_const_size - span));
    code.ja(*none, code.T_NEAR);
    code.shr(host, int(page_table_const_bits));
    code.test(host, u32(-(1 << valid_page_index_bits)));
    code.jnz(*none, code.T_NEAR);
    code.mov(host, code.qword[r14 + host * 8]);
    code.L(*end);
    ctx.deferred_emits.emplace_back([=, this] {
        code.L(*none);
        code.xor_(host.cvt32(), host.cvt32());
        code.jmp(*end, code.T_NEAR);
    });
    ctx.reg_alloc.DefineValue(code, inst, host);
}

]=])
    set(memory_source "${unit_prefix}${unit_lookup}${unit_suffix}")
endmacro()
