// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#ifdef __linux__
#include <ucontext.h>
#endif

// A32 fastmem for the PS5 port. HostMemory reserves a window covering the 32-bit guest
// address space, aliases guest pages into it at the kernel's 16 KiB granularity and keeps
// one access byte per 4 KiB page below it. JIT loads/stores test that byte and access the
// window directly, or take the page-table path for blocked pages (unaliased, read-only or
// GPU-tracked) without faulting. The fault handler only covers races with remapping.
namespace Eden::Fastmem {

// Select before Core::System (and so HostMemory) is constructed.
void Request(bool enabled) noexcept;
bool Requested() noexcept;
// Development A/B (dev-settings fastmem_large=off): keep every alias at 16 KiB.
void RequestLarge(bool enabled) noexcept;

struct Stats {
    std::uint64_t window;          // window base, 0 without a window
    std::uint64_t mapped_pages;    // 4 KiB guest pages mapped inside the window
    std::uint64_t aliased_chunks;  // 16 KiB chunks aliased into the window
    std::uint64_t direct_reads, direct_writes; // pages whose loads/stores go direct
    std::uint64_t map_calls, unmap_calls, protect_calls; // HostMemory requests
    std::uint64_t kernel_calls, kernel_ns; // mapping system calls and their duration
    std::uint64_t failures;        // mappings the kernel refused (chunk left unaliased)
    std::uint64_t large_blocks;    // 2 MiB blocks mapped as one large mapping
};
Stats WindowStats() noexcept;

// JIT faults redirected to an access fallback (dynarmic exception handler): races only.
std::uint64_t Faults() noexcept;
// Checked access sites moved to the page-table path because their pages kept being blocked.
std::uint64_t Demotions() noexcept;
// Development A/B (dev-settings fastmem_sites=off): keep the window and its reserved register
// but emit every access on the page-table path. Select before the JITs are created.
void RequestSites(bool enabled) noexcept;
bool SitesRequested() noexcept;

// Registers of an interrupted thread in a signal handler's context argument
// (targets without PS5_NATIVE, such as dynarmic, still build for the console).
inline std::uint64_t& ContextRip(void* context) noexcept {
#ifndef __linux__
    // The console's ucontext has 48 bytes the SDK header omits before the registers:
    // libkernel's __Ux86_64_setcontext restores rip from 0xe0 and rsp from 0xf8
    // (qualified by the 2026-09-20 native fault-recovery probe).
    return static_cast<std::uint64_t*>(context)[0xe0 / 8];
#else
    return *reinterpret_cast<std::uint64_t*>(&static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#endif
}
inline std::uint64_t& ContextRsp(void* context) noexcept {
#ifndef __linux__
    return static_cast<std::uint64_t*>(context)[0xf8 / 8];
#else
    return *reinterpret_cast<std::uint64_t*>(&static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
#endif
}

} // namespace Eden::Fastmem
