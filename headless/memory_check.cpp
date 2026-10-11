// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <map>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <source_location>
#include <type_traits>
#include <atomic>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "common/host_memory.h"
#include "common/sparse_large_vector.h"
#include "../src/fastmem.h"
#include <csignal>
#include <sys/wait.h>
namespace Common {
void SparseUsage(std::size_t* reserved, std::size_t* committed) noexcept;
}
#define EDEN_JIT_LIST_FORMAT_ONLY
#include "jit_list.h"
#include <unistd.h>

static void require(bool value, std::source_location at = std::source_location::current()) {
    if (!value) { std::fprintf(stderr, "Memory check failed at line %u\n", at.line()); std::abort(); }
}
using namespace Dynarmic;
constexpr uint64_t pointer_mask = 0x00fffffffffff000ULL;

template<bool a32>
struct Memory : std::conditional_t<a32, A32::UserCallbacks, A64::UserCallbacks> {
    using Address = std::conditional_t<a32, uint32_t, uint64_t>;
    using Exception = std::conditional_t<a32, A32::Exception, A64::Exception>;
    using Jit = std::conditional_t<a32, A32::Jit, A64::Jit>;
    Jit* jit{};
    uint32_t instruction{};
    uint32_t second_instruction{};
    Address address{};
    unsigned reads{}, writes{};
    A64::Vector value{0xfedcba9876543210ULL, 0x123456789abcdef0ULL};
    std::optional<uint32_t> MemoryReadCode(Address pc) {
        require(pc == 0x1000 || pc == 0x1004 || (second_instruction && pc == 0x1008));
        return pc == 0x1000 ? instruction : pc == 0x1004 && second_instruction ? second_instruction :
            a32 ? 0xef000000u : 0xd4000001u;
    }
    size_t Offset(Address at) const {
        const bool split = a32 && (instruction & ~0x00100000u) == 0xed800b00u;
        require(at == address || (split && at == address + 4));
        return at - address;
    }
    template<class T> T Read(Address at) {
        const auto offset = Offset(at); ++reads;
        T result; std::memcpy(&result, reinterpret_cast<const uint8_t*>(value.data()) + offset, sizeof result); return result;
    }
    template<class T> void Write(Address at, T result) {
        const auto offset = Offset(at);
        require(std::memcmp(&result, reinterpret_cast<const uint8_t*>(value.data()) + offset, sizeof result) == 0);
        ++writes;
    }
    // The JIT's callback takes the size; the checks below keep one function per size.
    uint64_t MemoryRead(Address at, std::size_t size) override {
        switch (size) {
        case 1: return MemoryRead8(at);
        case 2: return MemoryRead16(at);
        case 4: return MemoryRead32(at);
        case 8: return MemoryRead64(at);
        default: std::abort();
        }
    }
    void MemoryWrite(Address at, uint64_t v, std::size_t size) override {
        switch (size) {
        case 1: return MemoryWrite8(at, static_cast<uint8_t>(v));
        case 2: return MemoryWrite16(at, static_cast<uint16_t>(v));
        case 4: return MemoryWrite32(at, static_cast<uint32_t>(v));
        case 8: return MemoryWrite64(at, v);
        default: std::abort();
        }
    }
    virtual uint8_t MemoryRead8(Address at) { return Read<uint8_t>(at); }
    virtual uint16_t MemoryRead16(Address at) { return Read<uint16_t>(at); }
    virtual uint32_t MemoryRead32(Address at) { return Read<uint32_t>(at); }
    virtual uint64_t MemoryRead64(Address at) { return Read<uint64_t>(at); }
    A64::Vector MemoryRead128(Address at) { return Read<A64::Vector>(at); }
    virtual void MemoryWrite8(Address at, uint8_t v) { Write(at, v); }
    virtual void MemoryWrite16(Address at, uint16_t v) { Write(at, v); }
    virtual void MemoryWrite32(Address at, uint32_t v) { Write(at, v); }
    virtual void MemoryWrite64(Address at, uint64_t v) { Write(at, v); }
    void MemoryWrite128(Address at, A64::Vector v) { Write(at, v); }
    template<class T> bool ExclusiveWrite(Address at, T v, T expected) {
        require(std::memcmp(&expected, value.data(), sizeof expected) == 0);
        Write(at, v); return true;
    }
    bool MemoryWriteExclusive8(Address a, uint8_t v, uint8_t e) { return ExclusiveWrite(a, v, e); }
    bool MemoryWriteExclusive16(Address a, uint16_t v, uint16_t e) { return ExclusiveWrite(a, v, e); }
    bool MemoryWriteExclusive32(Address a, uint32_t v, uint32_t e) { return ExclusiveWrite(a, v, e); }
    bool MemoryWriteExclusive64(Address a, uint64_t v, uint64_t e) { return ExclusiveWrite(a, v, e); }
    bool MemoryWriteExclusive128(Address a, A64::Vector v, A64::Vector e) { return ExclusiveWrite(a, v, e); }
    void CallSVC(uint32_t n) { require(n == 0); jit->HaltExecution(); }
    void ExceptionRaised(Address, Exception) { std::abort(); }
    void AddTicks(uint64_t) { std::abort(); }
    uint64_t GetTicksRemaining() { std::abort(); }
    uint64_t GetCNTPCT() { std::abort(); }
};

template<bool a32>
unsigned Check(uint8_t* backing) {
    unsigned cases = 0;
    for (unsigned stride : {3u, 4u})
    for (unsigned format : {0u, 1u, 2u})
    for (bool absolute : {false, true}) {
        Memory<a32> memory;
        std::vector<void*> pages((a32 ? 1u << 20 : 1u << 12) << (stride - 3));
        std::conditional_t<a32, A32::UserConfig, A64::UserConfig> config{};
        config.callbacks = &memory;
        config.enable_cycle_counting = false;
        config.code_cache_size = 16 * 1024 * 1024;
        if constexpr (a32) {
            config.page_table = reinterpret_cast<decltype(config.page_table)>(pages.data());
        } else {
            config.page_table = pages.data();
            config.page_table_address_space_bits = 24;
            config.silently_mirror_page_table = false;
        }
        config.page_table_log2_stride = stride;
        config.absolute_offset_page_table = absolute;
        config.page_table_pointer_mask = format == 0 ? pointer_mask : format == 1 ? 0 : ~uint64_t(4095);
        if (format == 0) config.page_table_sign_extension = 8;
        config.page_table_marked_bit = 0;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
        typename Memory<a32>::Jit jit{config}; memory.jit = &jit;
        for (uint64_t base : {0x100000u, 0x400000u})
        for (unsigned size : {1u, 2u, 4u, 8u, 16u})
        for (bool scalar_simd : {false, true}) {
            if (scalar_simd && size != 4 && size != 8) continue;
            if constexpr (a32) { if (size > 4 && !scalar_simd) continue; }
            const unsigned index = std::countr_zero(size);
            constexpr uint32_t loads64[]{0x39400001, 0x79400001, 0xb9400001, 0xf9400001, 0x3dc00001};
            constexpr uint32_t loads32[]{0xe5d01000, 0xe1d010b0, 0xe5901000};
            for (bool write : {false, true}) {
                memory.instruction = scalar_simd
                    ? (a32 ? (size == 4 ? 0xed900a00u : 0xed900b00u)
                           : (size == 4 ? 0xbd400001u : 0xfd400001u))
                    : (a32 ? loads32[index] : loads64[index]);
                if (write) memory.instruction -= a32 ? 0x00100000 : 0x00400000;
                for (unsigned path = 0; path < (a32 ? 5u : 6u); ++path) {
                    // Ordinary, unaligned, crossing, marked, unmapped, out of range.
                    const unsigned offset = path == 1 ? 1 : path == 2 ? 4096 - size / 2 : 0;
                    memory.address = path == 5 ? 0x1000000 : base + offset;
                    const auto raw = reinterpret_cast<uintptr_t>(backing) - (absolute ? base : 0);
                    uint64_t entry = format == 0 ? (raw & pointer_mask) | 0xab00000000000ffeULL : raw;
                    if (path == 3) entry |= 1;
                    if (path == 4) entry = format == 0 ? 0xab00000000000ffeULL : 0;
                    pages[(base >> 12) << (stride - 3)] = reinterpret_cast<uint8_t*>(entry);
                    const bool callback = path >= 3 || (path == 2 && size > 1);
                    // A byte at offset4096 belongs to the next, unmapped page.
                    const bool expected_callback = callback || (path == 2 && size == 1);
                    memory.reads = memory.writes = 0;
                    std::memcpy(backing + offset, memory.value.data(), size);
                    if (write && !expected_callback) std::memset(backing + offset, 0, size);
                    jit.Reset(); jit.ClearHalt(~HaltReason{});
                    if (path == 0) jit.ClearCache(); // Reset also clears the invalidation halt bit.
                    if constexpr (a32) {
                        jit.Regs()[0] = memory.address; jit.Regs()[1] = write ? memory.value[0] : 0;
                        jit.Regs()[15] = 0x1000;
                        std::memset(jit.ExtRegs().data(), 0xcc, sizeof(jit.ExtRegs()));
                        if (write) std::memcpy(jit.ExtRegs().data(), memory.value.data(), size);
                    } else {
                        jit.SetPC(0x1000); jit.SetRegister(0, memory.address);
                        jit.SetRegister(1, write ? memory.value[0] : 0);
                        jit.SetVector(1, write ? memory.value : A64::Vector{~0ULL, ~0ULL});
                    }
                    require(jit.Run() == HaltReason::UserDefined1);
                    // A32 VLDR/VSTR D expands to two 32-bit accesses; at a page
                    // boundary only the second half takes the callback.
                    const unsigned callbacks = !expected_callback ? 0 :
                        a32 && scalar_simd && size == 8 && path >= 3 ? 2 : 1;
                    require(memory.reads == (write ? 0 : callbacks));
                    require(memory.writes == (write ? callbacks : 0));
                    if (write) {
                        if (!expected_callback) require(std::memcmp(backing + offset, memory.value.data(), size) == 0);
                    } else {
                        A64::Vector result{};
                        if constexpr (a32) {
                            if (scalar_simd) {
                                std::memcpy(result.data(), jit.ExtRegs().data(), size);
                                require(jit.ExtRegs()[size / 4] == 0xccccccccu);
                            } else result[0] = jit.Regs()[1];
                        } else {
                            result = size == 16 || scalar_simd ? jit.GetVector(1) : A64::Vector{jit.GetRegister(1), 0};
                            if (scalar_simd) {
                                require(result[1] == 0);
                                if (size == 4) require(result[0] >> 32 == 0);
                            }
                        }
                        require(std::memcmp(result.data(), memory.value.data(), size) == 0);
                    }
                    ++cases;
                }
            }
        }
    }
    return cases;
}


struct ProgramMemory : Memory<false> {
    std::vector<uint32_t> program;
    std::optional<uint32_t> MemoryReadCode(uint64_t pc) override {
        require(pc >= 0x1000 && (pc - 0x1000) % 4 == 0);
        return program.at((pc - 0x1000) / 4);
    }
};

static A64::UserConfig TableConfig(Memory<false>& memory, void** pages, unsigned bits = 24) {
    A64::UserConfig config{};
    config.callbacks = &memory;
    config.enable_cycle_counting = false;
    config.code_cache_size = 16 * 1024 * 1024;
    config.page_table = pages;
    config.page_table_address_space_bits = bits;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.page_table_pointer_mask = pointer_mask;
    config.page_table_sign_extension = 8;
    config.page_table_marked_bit = 0;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    return config;
}

static unsigned CheckTableExclusives(uint8_t* backing) {
    unsigned cases = 0;
    std::vector<void*> pages(1 << 12);
    for (uint64_t base : {0x100000u, 0x400000u})
    for (unsigned size : {1u, 2u, 4u, 8u, 16u}) {
        ProgramMemory memory;
        ExclusiveMonitor monitor{2};
        auto config = TableConfig(memory, pages.data());
        config.global_monitor = &monitor;
        config.fastmem_exclusive_access = true;
        const unsigned index = std::countr_zero(size);
        const uint32_t load = size == 16 ? 0xc87f0c01u : 0x085f7c01u | (index << 30);
        const uint32_t store = size == 16 ? 0xc8220c01u : 0x08027c01u | (index << 30);
        const uintptr_t entry = ((uintptr_t(backing) - base) & pointer_mask) | 0xab00000000000ffeULL;
        A64::Jit jit{config}; memory.jit = &jit;
        auto start = [&](bool clear = false) {
            memory.reads = memory.writes = 0;
            jit.Reset(); jit.ClearHalt(~HaltReason{});
            if (clear) jit.ClearCache();
            jit.SetPC(0x1000); jit.SetRegister(0, memory.address);
            jit.SetRegister(4, memory.address + 16);
        };
        memory.program = {load, store, 0xd4000001u};
        for (unsigned path = 0; path < 5; ++path) {
            if (path == 4 && size == 1) continue;
            memory.address = path == 3 ? 1 << 24 : base + (path == 4);
            pages[base >> 12] = reinterpret_cast<void*>(path == 2 ? 0 : entry | (path == 1));
            std::memcpy(backing, memory.value.data(), size);
            start(); require(jit.Run() == HaltReason::UserDefined1);
            require(jit.GetRegister(2) == 0);
            require(memory.reads == unsigned(path != 0) && memory.writes == unsigned(path != 0));
            const A64::Vector value = size == 16 ? A64::Vector{jit.GetRegister(1), jit.GetRegister(3)} :
                A64::Vector{jit.GetRegister(1), 0};
            require(std::memcmp(value.data(), memory.value.data(), size) == 0);
            require(std::memcmp(backing, memory.value.data(), size) == 0);
            ++cases;
        }
        pages[base >> 12] = reinterpret_cast<void*>(entry);
        memory.address = base;
        for (bool mismatch : {false, true}) {
            memory.program = mismatch ? std::vector<uint32_t>{load, store | (4u << 5), 0xd4000001u} :
                                        std::vector<uint32_t>{store, 0xd4000001u};
            start(true); require(jit.Run() == HaltReason::UserDefined1);
            require(jit.GetRegister(2) == 1 && memory.reads == 0 && memory.writes == 0);
            ++cases;
        }
        memory.program = {load, 0xd4000001u, store, 0xd4000001u};
        for (bool other_core : {false, true}) {
            std::memcpy(backing, memory.value.data(), size);
            start(true); require(jit.Run() == HaltReason::UserDefined1);
            if (other_core) {
                monitor.ReadAndMark<uint8_t>(1, base, [&] { return backing[0]; });
                require(monitor.DoExclusiveOperation<uint8_t>(1, base, [&](uint8_t v) { return v == backing[0]; }));
            } else {
                backing[0] ^= 1; // Reservation survives, but compare/exchange must fail.
            }
            jit.ClearHalt(~HaltReason{}); jit.SetPC(0x1008);
            require(jit.Run() == HaltReason::UserDefined1);
            require(jit.GetRegister(2) == 1 && memory.reads == 0 && memory.writes == 0);
            require(backing[0] == (reinterpret_cast<uint8_t*>(memory.value.data())[0] ^ !other_core));
            ++cases;
        }
    }
    return cases;
}

static void CheckAtomicLoop(uint8_t* backing, bool timing) {
    struct AtomicMemory : ProgramMemory {
        uint64_t* counter{};
        uint64_t MemoryRead64(uint64_t at) override {
            require(at == 0x100000); ++reads;
            return std::atomic_ref(*counter).load();
        }
        bool MemoryWriteExclusive64(uint64_t at, uint64_t value, uint64_t expected) override {
            require(at == 0x100000); ++writes;
            return std::atomic_ref(*counter).compare_exchange_strong(expected, value);
        }
    };
    std::vector<void*> pages(1 << 12);
    pages[0x100000 >> 12] = reinterpret_cast<void*>(((uintptr_t(backing) - 0x100000) & pointer_mask) | 0xab00000000000ffeULL);
    auto* counter = reinterpret_cast<uint64_t*>(backing);
    for (bool auto_accuracy : {false, true})
    for (bool inline_access : {false, true})
    for (unsigned workers : {1u, 2u}) {
        ExclusiveMonitor monitor{workers};
        std::vector<AtomicMemory> memories(workers);
        std::vector<std::unique_ptr<A64::Jit>> jits;
        for (unsigned core = 0; core < workers; ++core) {
            auto& memory = memories[core]; memory.counter = counter;
            memory.program = {0xc85f7c01u, 0x91000421u, 0xc8027c01u,
                0x35000002u | ((-3u & 0x7ffff) << 5), 0xf1000463u,
                0x54000001u | ((-5u & 0x7ffff) << 5), 0xd4000001u};
            auto config = TableConfig(memory, pages.data());
            config.global_monitor = &monitor; config.processor_id = core;
            config.fastmem_exclusive_access = inline_access;
            if (auto_accuracy) {
                // Match upstream Auto, also used by the Windows comparison.
                config.unsafe_optimizations = true;
                config.optimizations |= OptimizationFlag::Unsafe_UnfuseFMA |
                                        OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
                config.fastmem_address_space_bits = 64;
            }
            jits.push_back(std::make_unique<A64::Jit>(config)); memory.jit = jits.back().get();
        }
        for (unsigned trial = 0; trial < (timing ? 4u : 1u); ++trial) {
            *counter = 0;
            const uint64_t iterations = trial ? 100000 : 1000;
            for (auto& jit : jits) {
                jit->Reset(); jit->ClearHalt(~HaltReason{});
                jit->SetPC(0x1000); jit->SetRegister(0, 0x100000); jit->SetRegister(3, iterations);
            }
            std::atomic<unsigned> ready{};
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> threads;
            for (unsigned core = 0; core < workers; ++core) threads.emplace_back([&, core] {
                ready.fetch_add(1);
                while (ready.load() != workers) std::this_thread::yield();
                require(jits[core]->Run() == HaltReason::UserDefined1);
            });
            for (auto& thread : threads) thread.join();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(*counter == iterations * workers);
            if (inline_access) for (auto& memory : memories) require(memory.reads == 0 && memory.writes == 0);
            if (trial) std::printf("ATOMIC_PRESSURE auto=%u inline=%u workers=%u trial=%u seconds=%.9f\n", unsigned(auto_accuracy), unsigned(inline_access), workers, trial, seconds);
        }
    }
}

// A32 LDREX/STREX increment loops (32-bit guests): the callback
// path under the global monitor versus the page-table inline path, 1-4 cores.
static void CheckAtomicLoopA32(uint8_t* backing, bool timing) {
    struct AtomicMemory32 : Memory<true> {
        std::vector<uint32_t> program;
        uint32_t* counter{};
        std::optional<uint32_t> MemoryReadCode(uint32_t pc) override {
            require(pc >= 0x1000 && (pc - 0x1000) % 4 == 0);
            return program.at((pc - 0x1000) / 4);
        }
        uint32_t MemoryRead32(uint32_t at) override {
            require(at == 0x100000); ++reads;
            return std::atomic_ref(*counter).load();
        }
        bool MemoryWriteExclusive32(uint32_t at, uint32_t value, uint32_t expected) override {
            require(at == 0x100000); ++writes;
            return std::atomic_ref(*counter).compare_exchange_strong(expected, value);
        }
    };
    std::vector<void*> pages(1u << 20);
    pages[0x100000 >> 12] = reinterpret_cast<void*>(((uintptr_t(backing) - 0x100000) & pointer_mask) | 0xab00000000000ffeULL);
    auto* counter = reinterpret_cast<uint32_t*>(backing);
    for (bool auto_accuracy : {false, true})
    for (bool inline_access : {false, true})
    for (unsigned workers : {1u, 2u, 4u}) {
        ExclusiveMonitor monitor{workers};
        std::vector<AtomicMemory32> memories(workers);
        std::vector<std::unique_ptr<A32::Jit>> jits;
        for (unsigned core = 0; core < workers; ++core) {
            auto& memory = memories[core]; memory.counter = counter;
            // loop: ldrex r1,[r0]; add r1,r1,#1; strex r2,r1,[r0]; cmp r2,#0; bne loop;
            //       subs r3,r3,#1; bne loop; svc #0
            memory.program = {0xe1901f9fu, 0xe2811001u, 0xe1802f91u, 0xe3520000u, 0x1afffffau,
                              0xe2533001u, 0x1afffff8u, 0xef000000u};
            A32::UserConfig config{};
            config.callbacks = &memory;
            config.enable_cycle_counting = false;
            config.code_cache_size = 16 * 1024 * 1024;
            config.page_table = reinterpret_cast<decltype(config.page_table)>(pages.data());
            config.absolute_offset_page_table = true;
            config.page_table_pointer_mask = pointer_mask;
            config.page_table_sign_extension = 8;
            config.page_table_marked_bit = 0;
            config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
            config.only_detect_misalignment_via_page_table_on_page_boundary = true;
            config.global_monitor = &monitor; config.processor_id = core;
            config.fastmem_exclusive_access = inline_access;
            if (auto_accuracy) {
                // The port's Auto accuracy (arm_dynarmic_32.cpp).
                config.unsafe_optimizations = true;
                config.optimizations |= OptimizationFlag::Unsafe_UnfuseFMA |
                                        OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
            }
            jits.push_back(std::make_unique<A32::Jit>(config)); memory.jit = jits.back().get();
        }
        for (unsigned trial = 0; trial < (timing ? 4u : 1u); ++trial) {
            *counter = 0;
            const uint32_t iterations = trial ? 100000 : 1000;
            for (auto& jit : jits) {
                jit->Reset(); jit->ClearHalt(~HaltReason{});
                jit->Regs()[0] = 0x100000; jit->Regs()[3] = iterations; jit->Regs()[15] = 0x1000;
            }
            std::atomic<unsigned> ready{};
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> threads;
            for (unsigned core = 0; core < workers; ++core) threads.emplace_back([&, core] {
                ready.fetch_add(1);
                while (ready.load() != workers) std::this_thread::yield();
                require(jits[core]->Run() == HaltReason::UserDefined1);
            });
            for (auto& thread : threads) thread.join();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(*counter == iterations * workers);
            if (inline_access) for (auto& memory : memories) require(memory.reads == 0 && memory.writes == 0);
            if (trial) std::printf("ATOMIC32_PRESSURE auto=%u inline=%u workers=%u trial=%u seconds=%.9f\n", unsigned(auto_accuracy), unsigned(inline_access), workers, trial, seconds);
        }
    }
}

static void CheckPressure(uint8_t* backing, bool timing) {
    ProgramMemory memory;
    constexpr size_t table_bytes = (size_t{1} << 27) * sizeof(void*);
    auto** pages = static_cast<void**>(mmap(nullptr, table_bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    require(pages != MAP_FAILED);
    pages[0x100000 >> 12] = reinterpret_cast<void*>(((uintptr_t(backing) - 0x100000) & pointer_mask) |
                                                  0xab00000000000ffeULL);
    auto config = TableConfig(memory, pages, 39);
    for (unsigned live : {4u, 8u, 12u, 20u}) {
        memory.program.clear();
        for (unsigned r = 1; r <= live; ++r) {
            const uint64_t value = r;
            std::memcpy(backing + (r - 1) * 8, &value, 8);
            memory.program.push_back(0xf9400000u | ((r - 1) << 10) | r); // LDR Xr,[X0,#offset]
        }
        for (unsigned r = 1; r <= live; ++r)
            memory.program.push_back(0x8b000000u | (r << 16) | (21u << 5) | 21); // ADD X21,X21,Xr
        memory.program.push_back(0xf1000400u | (22u << 5) | 22); // SUBS X22,X22,#1
        memory.program.push_back(0x54000001u | ((-uint32_t(memory.program.size()) & 0x7ffff) << 5)); // B.NE loop
        memory.program.push_back(0xd4000001u); // SVC 0
        A64::Jit jit{config}; memory.jit = &jit;
        for (unsigned trial = 0; trial < (timing ? 6u : 1u); ++trial) {
            const uint64_t iterations = trial ? 10000000 : 7; // Compile before timing.
            jit.Reset(); jit.ClearHalt(~HaltReason{});
            jit.SetPC(0x1000); jit.SetRegister(0, 0x100000); jit.SetRegister(22, iterations);
            const auto start = std::chrono::steady_clock::now();
            require(jit.Run() == HaltReason::UserDefined1);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(jit.GetRegister(21) == iterations * live * (live + 1) / 2);
            for (unsigned r = 1; r <= live; ++r) require(jit.GetRegister(r) == r);
            require(memory.reads == 0 && memory.writes == 0);
            if (trial) std::printf("MEMORY_PRESSURE live=%u trial=%u seconds=%.9f\n", live, trial, seconds);
        }
        if (std::getenv("EDEN_DUMP_PRESSURE")) {
            jit.SetPC(0x1000);
            std::printf("MEMORY_PRESSURE_ASM live=%u\n%s\n", live, jit.Disassemble().c_str());
        }
    }
    require(munmap(pages, table_bytes) == 0);
}

static void CheckSimdPressure(uint8_t* backing) {
    ProgramMemory memory;
    memory.address = 0x110000;
    memory.value = {std::bit_cast<uint32_t>(1.0f), 0};
    std::vector<void*> pages(1u << 12);
    pages[0x100000 >> 12] = reinterpret_cast<void*>((uintptr_t(backing) - 0x100000) & pointer_mask);
    auto config = TableConfig(memory, pages.data(), 24);
    for (unsigned live : {4u, 8u, 12u, 20u}) {
        memory.program.clear();
        for (unsigned r = 1; r <= live; ++r) {
            const float value = float(r);
            std::memcpy(backing + (r - 1) * 4, &value, 4);
            // S4 takes the callback; the rest use checked direct loads.
            memory.program.push_back(r == 4 ? 0xbd400024u : 0xbd400000u | ((r - 1) << 10) | r);
        }
        memory.program.push_back(0xbd000024u); // STR S4,[X1]: callback with all values live.
        for (unsigned r = 1; r <= live; ++r)
            memory.program.push_back(0x1e202800u | (r << 16) | (21u << 5) | 21); // FADD S21,S21,Sr
        memory.program.push_back(0xf1000400u | (22u << 5) | 22);
        memory.program.push_back(0x54000001u | ((-uint32_t(memory.program.size()) & 0x7ffff) << 5));
        memory.program.push_back(0xd4000001u);
        A64::Jit jit{config}; memory.jit = &jit;
        // Reuse IR/code storage as well as exercising XMM spills and mixed callbacks.
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            jit.Reset(); jit.ClearHalt(~HaltReason{}); jit.ClearCache();
            jit.SetPC(0x1000); jit.SetRegister(0, 0x100000);
            jit.SetRegister(1, memory.address); jit.SetRegister(22, 7);
            memory.reads = memory.writes = 0;
            require(jit.Run() == HaltReason::UserDefined1);
            require(memory.reads == 7 && memory.writes == 7);
            require(jit.GetVector(21) == A64::Vector{std::bit_cast<uint32_t>(float(7 * (live * (live + 1) / 2 - 3))), 0});
            for (unsigned r = 1; r <= live; ++r)
                require(jit.GetVector(r) == A64::Vector{std::bit_cast<uint32_t>(float(r == 4 ? 1 : r)), 0});
        }
    }
    std::puts("Scalar SIMD pressure PASS: 4/8/12/20 live values, read/write callbacks, spills and reuse");
}

static void CheckColdCompilation(bool chains = false) {
    const unsigned blocks = chains ? 262144 : 8192;
    constexpr unsigned instructions = 17;
    const unsigned chain_length = chains ? 128 : 1;
    ProgramMemory memory;
    for (unsigned block = 0; block < blocks; ++block) {
        for (unsigned immediate = 1; immediate <= 16; ++immediate)
            memory.program.push_back(0x91000000u | (immediate << 10)); // ADD X0,X0,#imm
        memory.program.push_back((block + 1) % chain_length ? 0x14000001u : 0xd4000001u); // B next / SVC 0
    }
    for (unsigned trial = 0; trial < 3; ++trial) {
        A64::UserConfig config{};
        config.callbacks = &memory;
        config.enable_cycle_counting = false;
        config.code_cache_size = (chains ? 16 : 64) * 1024 * 1024;
        A64::Jit jit{config}; memory.jit = &jit;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned block = 0; block < blocks; block += chain_length) {
            jit.ClearHalt(~HaltReason{});
            jit.SetPC(0x1000 + block * instructions * 4);
            jit.SetRegister(0, block);
            require(jit.Run() == HaltReason::UserDefined1);
            require(jit.GetRegister(0) == block + 136 * chain_length);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("COLD_COMPILE trial=%u blocks=%u chain=%u seconds=%.9f\n", trial, blocks, chain_length, seconds);
        if (chains) {
            // An already linked target must observe precise invalidation and relinking.
            const unsigned patched = 16 * instructions;
            memory.program[patched] = 0x91004000u; // ADD X0,X0,#16 instead of #1
            jit.ClearHalt(~HaltReason{});
            jit.InvalidateCacheRange(0x1000 + patched * 4, 4);
            jit.SetPC(0x1000); jit.SetRegister(0, 0);
            require(jit.Run() == HaltReason::UserDefined1);
            require(jit.GetRegister(0) == 136 * chain_length + 15);
            jit.ClearHalt(~HaltReason{}); jit.ClearCache();
            jit.SetPC(0x1000); jit.SetRegister(0, 0);
            jit.Step();
            require(jit.GetPC() == 0x1004 && jit.GetRegister(0) == 1);
            memory.program[patched] = 0x91000400u;
        }
    }
    if (chains) {
        struct FaultMemory : ProgramMemory {
            unsigned faults = 0;
            std::optional<uint32_t> MemoryReadCode(uint64_t pc) override {
                return pc >= 0x1000 && pc < 0x1000 + program.size() * 4 ? ProgramMemory::MemoryReadCode(pc) : std::nullopt;
            }
            void ExceptionRaised(uint64_t pc, A64::Exception exception) override {
                require(pc == 0x2000 && exception == A64::Exception::NoExecuteFault);
                require(jit->GetRegister(0) == 1); // Root executed before the target fault.
                ++faults; jit->HaltExecution();
            }
        } fault;
        fault.program = {0x91000400u, 0x140003ffu}; // ADD #1; B 0x2000
        A64::UserConfig config{}; config.callbacks = &fault;
        config.enable_cycle_counting = false; config.code_cache_size = 16 * 1024 * 1024;
        A64::Jit jit{config}; fault.jit = &jit; jit.SetPC(0x1000);
        require(jit.Run() == HaltReason::UserDefined1 && fault.faults == 1);
        fault.program = {0xb4000041u, 0x140003ffu, 0xd4000001u}; // CBZ X1,+8; B unmapped; SVC
        jit.ClearHalt(~HaltReason{}); jit.ClearCache();
        jit.SetPC(0x1000); jit.SetRegister(0, 0); jit.SetRegister(1, 0);
        require(jit.Run() == HaltReason::UserDefined1 && fault.faults == 1);
        require(jit.GetRegister(0) == 0); // Unselected faulting target cannot execute.
        jit.ClearHalt(~HaltReason{}); jit.SetPC(0x1000);
        jit.SetRegister(0, 1); jit.SetRegister(1, 1);
        require(jit.Run() == HaltReason::UserDefined1 && fault.faults == 2);
    }
}

// A32 checked fastmem through the port's HostMemory window: aliased, 4 KiB out-of-phase,
// read-only, partially unmapped and GPU-tracked pages, and accesses crossing into a
// blocked page. Blocked pages take the page-table path, so no case may fault.
struct FastmemRegion { uint32_t address, offset, bytes; };
struct WindowMemory32 : Memory<true> {
    std::vector<uint32_t> program;
    std::vector<FastmemRegion> regions;
    uint8_t* backing{};
    unsigned callbacks{};
    uint8_t* Host(uint32_t at) {
        for (const auto& region : regions)
            if (at >= region.address && at - region.address < region.bytes)
                return backing + region.offset + (at - region.address);
        std::abort();
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t pc) override {
        require(pc >= 0x1000 && (pc - 0x1000) / 4 < program.size());
        return program[(pc - 0x1000) / 4];
    }
    uint32_t MemoryRead32(uint32_t at) override {
        ++callbacks; uint32_t value;
        for (unsigned i = 0; i < 4; ++i) reinterpret_cast<uint8_t*>(&value)[i] = *Host(at + i);
        return value;
    }
    void MemoryWrite32(uint32_t at, uint32_t value) override {
        ++callbacks;
        for (unsigned i = 0; i < 4; ++i) *Host(at + i) = reinterpret_cast<uint8_t*>(&value)[i];
    }
    bool MemoryWriteExclusive32(uint32_t at, uint32_t value, uint32_t expected) override {
        ++callbacks;
        return std::atomic_ref(*reinterpret_cast<uint32_t*>(Host(at))).compare_exchange_strong(expected, value);
    }
    uint64_t MemoryRead64(uint32_t at) override {
        ++callbacks; uint64_t value;
        for (unsigned i = 0; i < 8; ++i) reinterpret_cast<uint8_t*>(&value)[i] = *Host(at + i);
        return value;
    }
    void MemoryWrite64(uint32_t at, uint64_t value) override {
        ++callbacks;
        for (unsigned i = 0; i < 8; ++i) *Host(at + i) = reinterpret_cast<uint8_t*>(&value)[i];
    }
};

struct FastmemFixture {
    Common::HostMemory host{64u << 20, uint64_t{1} << 39};
    uint8_t* backing = host.BackingBasePointer();
    std::vector<void*> pages = std::vector<void*>(1u << 20, reinterpret_cast<void*>(0xab00000000000ffeULL));
    std::vector<FastmemRegion> regions;
    void Map(FastmemRegion region) {
        host.Map(region.address, region.offset, region.bytes, Common::MemoryPermission::ReadWrite, false);
        for (uint32_t at = 0; at < region.bytes; at += 4096) Entry(region.address + at, region.offset + at, false);
        regions.push_back(region);
    }
    void Entry(uint32_t address, uint32_t offset, bool marked) {
        pages[address >> 12] = reinterpret_cast<void*>(
            ((uintptr_t(backing + offset) - address) & pointer_mask) | 0xab00000000000ffeULL | (marked ? 1 : 0));
    }
    A32::UserConfig Config(WindowMemory32& memory, ExclusiveMonitor& monitor, bool fastmem) {
        A32::UserConfig config{};
        config.callbacks = &memory;
        config.enable_cycle_counting = false;
        config.code_cache_size = 16 * 1024 * 1024;
        config.page_table = reinterpret_cast<decltype(config.page_table)>(pages.data());
        config.absolute_offset_page_table = true;
        config.page_table_pointer_mask = pointer_mask;
        config.page_table_sign_extension = 8;
        config.page_table_marked_bit = 0;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
        if (fastmem) config.fastmem_pointer = reinterpret_cast<uintptr_t>(host.VirtualBasePointer());
        config.fastmem_exclusive_access = true;
        config.global_monitor = &monitor;
        return config;
    }
};

static void CheckFastmemA32() {
    using Common::MemoryPermission;
    Eden::Fastmem::Request(true);
    FastmemFixture fixture;
    Eden::Fastmem::Request(false);
    require(fixture.host.VirtualBasePointer() != nullptr);
    auto& host = fixture.host;
    fixture.Map({0x100000, 0x10000, 0x10000}); // four aliased chunks
    fixture.Map({0x200000, 0x21000, 0x8000});  // backing 4 KiB out of phase: never aliased
    fixture.Map({0x300000, 0x40000, 0x4000});  // aliased, then read-only
    host.Protect(0x300000, 0x4000, MemoryPermission::Read);
    fixture.Map({0x400000, 0x50000, 0x4000});  // aliased, then one page unmapped
    host.Unmap(0x403000, 0x1000, false);
    fixture.pages[0x403] = reinterpret_cast<void*>(0xab00000000000ffeULL);
    fixture.Map({0x500000, 0x60000, 0x8000});  // aliased; page 0x501000 GPU-tracked below
    host.Protect(0x501000, 0x1000, MemoryPermission{});
    fixture.Entry(0x501000, 0x61000, true);
    host.Map(uint64_t{1} << 32, 0x70000, 0x4000, MemoryPermission::ReadWrite, false); // clipped away
    auto stats = Eden::Fastmem::WindowStats();
    require(stats.aliased_chunks == 4 + 1 + 2 && stats.failures == 0);
    require(stats.mapped_pages == 16 + 8 + 4 + 3 + 8);
    // Direct reads: A (16), C (4), E (8) minus the tracked page and its predecessor.
    require(stats.direct_reads == 16 - 1 + 4 - 1 + 8 - 2 - 1);

    unsigned cases = 0;
    const auto run = [&](const std::vector<uint32_t>& program, uint32_t address, uint32_t count, uint32_t iterations,
                         unsigned callbacks, const std::function<bool(uint32_t)>& expect, uint64_t expected_faults = 0) {
        WindowMemory32 memory;
        memory.program = program;
        memory.regions = fixture.regions;
        memory.backing = fixture.backing;
        ExclusiveMonitor monitor{1};
        A32::Jit jit{fixture.Config(memory, monitor, true)};
        memory.jit = &jit;
        const auto before = Eden::Fastmem::Faults();
        for (uint32_t pass = 0; pass < 2; ++pass) {
            jit.Regs()[0] = address; jit.Regs()[1] = count; jit.Regs()[2] = 1; jit.Regs()[3] = iterations;
            jit.Regs()[15] = 0x1000;
            jit.ClearHalt(~HaltReason{});
            require(jit.Run() == HaltReason::UserDefined1);
        }
        const auto faults = Eden::Fastmem::Faults() - before;
        if (faults != expected_faults || memory.callbacks != callbacks || !expect(jit.Regs()[3]))
            std::fprintf(stderr, "fastmem case %u address=%x faults=%llu callbacks=%u r3=%x\n", cases, address,
                         static_cast<unsigned long long>(faults), memory.callbacks, jit.Regs()[3]);
        require(faults == expected_faults && memory.callbacks == callbacks && expect(jit.Regs()[3]));
        ++cases;
    };
    const auto host_at = [&](uint32_t at) -> uint8_t* {
        for (const auto& region : fixture.regions)
            if (at >= region.address && at - region.address < region.bytes)
                return fixture.backing + region.offset + (at - region.address);
        std::abort();
    };
    const auto fill = [&](uint32_t address, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) std::memcpy(host_at(address + 4 * i), &i, 4);
    };
    const auto incremented = [&](uint32_t address, uint32_t count) {
        return [&, address, count](uint32_t) {
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t value; std::memcpy(&value, host_at(address + 4 * i), 4);
                if (value != i + 2) return false;
            }
            return true;
        };
    };
    // ldr r3,[r0]; add r3,r3,r2; str r3,[r0],#4; subs r1,r1,#1; bne loop; svc #0
    const std::vector<uint32_t> increment{0xe5903000u, 0xe0833002u, 0xe4803004u, 0xe2511001u, 0x1afffffau, 0xef000000u};
    const struct { uint32_t address, words; unsigned callbacks; } increments[]{
        {0x100000, 0x4000, 0},       // aliased: direct
        {0x200000, 0x2000, 0},       // unaliased: page table
        {0x300000, 0x1000, 0},       // read-only: direct loads, page-table stores
        {0x400000, 0xc00, 0},        // partially unmapped chunk: page table
        {0x500000, 0x2000, 2 * 2048}, // tracked page: callbacks for its 1024 words, both passes
    };
    for (const auto& c : increments) {
        fill(c.address, c.words);
        run(increment, c.address, c.words, 0, c.callbacks, incremented(c.address, c.words));
    }
    // Unaligned word loads ending two bytes into the next page: direct only when both
    // pages allow it, otherwise the page-table path's crossing check calls back.
    // ldr r3,[r0]; subs r1,r1,#1; bne loop; svc #0
    const std::vector<uint32_t> straddle{0xe5903000u, 0xe2511001u, 0x1afffffcu, 0xef000000u};
    const uint32_t word = 0x04030201u;
    std::memcpy(fixture.backing + 0x10000 + 0x1ffe, &word, 4);
    run(straddle, 0x101ffe, 100, 0, 0, [&](uint32_t r3) { return r3 == word; });
    std::memcpy(fixture.backing + 0x60000 + 0xffe, &word, 4);
    run(straddle, 0x500ffe, 100, 0, 2 * 100, [&](uint32_t r3) { return r3 == word; });
    // FP scalar copies (VLDR/VSTR), straight between the window and XMM registers.
    const auto copy = [&](bool doubles, uint32_t source, uint32_t destination, uint32_t count, unsigned callbacks,
                          uint64_t expected_faults = 0) {
        WindowMemory32 memory;
        // vldr s0/d0,[r0]; vstr s0/d0,[r1]; add r0,r0,#4/8; add r1,r1,#4/8; subs r2,r2,#1; bne loop; svc #0
        memory.program = doubles
            ? std::vector<uint32_t>{0xed900b00u, 0xed810b00u, 0xe2800008u, 0xe2811008u, 0xe2522001u, 0x1afffff9u, 0xef000000u}
            : std::vector<uint32_t>{0xed900a00u, 0xed810a00u, 0xe2800004u, 0xe2811004u, 0xe2522001u, 0x1afffff9u, 0xef000000u};
        memory.regions = fixture.regions;
        memory.backing = fixture.backing;
        ExclusiveMonitor monitor{1};
        A32::Jit jit{fixture.Config(memory, monitor, true)};
        memory.jit = &jit;
        const uint32_t bytes = count * (doubles ? 8 : 4);
        for (uint32_t i = 0; i < bytes; ++i) *host_at(source + i) = uint8_t(i * 7 + count);
        const auto before = Eden::Fastmem::Faults();
        jit.Regs()[0] = source; jit.Regs()[1] = destination; jit.Regs()[2] = count; jit.Regs()[15] = 0x1000;
        require(jit.Run() == HaltReason::UserDefined1);
        const auto faults = Eden::Fastmem::Faults() - before;
        bool same = true;
        for (uint32_t i = 0; i < bytes; ++i) same = same && *host_at(destination + i) == uint8_t(i * 7 + count);
        if (faults != expected_faults || memory.callbacks != callbacks || !same)
            std::fprintf(stderr, "fastmem copy case %u faults=%llu callbacks=%u same=%d\n", cases,
                         static_cast<unsigned long long>(faults), memory.callbacks, int(same));
        require(faults == expected_faults && memory.callbacks == callbacks && same);
        ++cases;
    };
    copy(false, 0x100000, 0x502000, 1024, 0);    // direct both ways
    copy(false, 0x200000, 0x501000, 1024, 1024); // page-table load, tracked store calls back
    copy(true, 0x100000, 0x300000, 512, 0);      // direct load, read-only chunk stores via table
    copy(true, 0x103ffc, 0x506000, 1, 0);        // crossing between two aliased chunks: direct
    copy(true, 0x500ffc, 0x506000, 1, 1);        // crossing into the tracked page: calls back
    // A mapping change racing a direct access: the access byte still allows it, but the
    // chunk behind it is gone. Each direct site faults once, completes through its
    // fallback with the right value, and recompiles onto the page table.
    fixture.Map({0x600000, 0x70000, 0x4000});
    auto* chunk = fixture.host.VirtualBasePointer() + 0x600000;
    require(mmap(chunk, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == chunk);
    copy(false, 0x100000, 0x600000, 1, 1, 1);    // VSTR s0: fault entry moves the value first
    copy(true, 0x100000, 0x600008, 1, 2, 2);     // VSTR d0: A32 stores its two words separately
    copy(false, 0x600000, 0x502000, 1, 1, 1);    // VLDR s0: fallback result lands in s0
    copy(true, 0x600008, 0x502008, 1, 2, 2);     // VLDR d0: two word loads
    fill(0x600100, 1);
    run(increment, 0x600100, 1, 0, 2, [&](uint32_t) {
        uint32_t value; std::memcpy(&value, host_at(0x600100), 4); return value == 2;
    }, 2);                                       // LDR and STR each fault once
    // ldrex r1,[r0]; add r1,r1,#1; strex r2,r1,[r0]; cmp r2,#0; bne loop; subs r3,r3,#1; bne loop; svc #0
    const std::vector<uint32_t> atomic{0xe1901f9fu, 0xe2811001u, 0xe1802f91u, 0xe3520000u, 0x1afffffau,
                                       0xe2533001u, 0x1afffff8u, 0xef000000u};
    for (uint32_t address : {0x100000u, 0x200000u}) {
        std::memset(host_at(address), 0, 4);
        run(atomic, address, 1, 1000, 0, [](uint32_t r3) { return r3 == 0; });
        uint32_t value; std::memcpy(&value, host_at(address), 4);
        require(value == 2000);
    }
    // Unmapping drops the aliases; mapping again restores them with full access.
    host.Unmap(0x100000, 0x10000, false);
    host.Map(0x300000, 0x40000, 0x4000, MemoryPermission::ReadWrite, false);
    stats = Eden::Fastmem::WindowStats();
    require(stats.aliased_chunks == 1 + 2 + 1 && stats.failures == 0); // C, E and the race chunk remain
    std::printf("Fastmem A32 PASS: %u cases, %llu faults, %llu kernel calls\n", cases,
                static_cast<unsigned long long>(Eden::Fastmem::Faults()),
                static_cast<unsigned long long>(stats.kernel_calls));
}

// Checked fastmem under concurrency: three JIT workers increment their own slices of an
// aliased region while a mutator flips GPU-style tracking (access byte + marked PTE) on
// random pages and a reader runs over a region that is repeatedly unmapped and mapped.
// Every access path writes the same backing, so the slices must end exact.
static void StressFastmemA32() {
    using Common::MemoryPermission;
    Eden::Fastmem::Request(true);
    FastmemFixture fixture;
    Eden::Fastmem::Request(false);
    require(fixture.host.VirtualBasePointer() != nullptr);
    constexpr uint32_t hot = 0x100000, hot_bytes = 0x30000, slice = 0x10000;
    constexpr uint32_t cold = 0x800000, cold_bytes = 0x10000;
    fixture.Map({hot, 0x100000, hot_bytes});
    fixture.Map({cold, 0x200000, cold_bytes});
    std::memset(fixture.backing + 0x100000, 0, hot_bytes);
    const std::vector<uint32_t> increment{0xe5903000u, 0xe0833002u, 0xe4803004u, 0xe2511001u, 0x1afffffau, 0xef000000u};
    // ldr r3,[r0],#4; subs r1,r1,#1; bne loop; svc #0
    const std::vector<uint32_t> scan{0xe4903004u, 0xe2511001u, 0x1afffffcu, 0xef000000u};
    constexpr unsigned passes = 300;
    std::atomic<unsigned> running{4};
    std::atomic<unsigned long long> callbacks{0};
    const auto worker = [&](std::vector<uint32_t> program, uint32_t address, uint32_t words, unsigned count) {
        WindowMemory32 memory;
        memory.program = std::move(program);
        memory.regions = fixture.regions;
        memory.backing = fixture.backing;
        ExclusiveMonitor monitor{1};
        A32::Jit jit{fixture.Config(memory, monitor, true)};
        memory.jit = &jit;
        for (unsigned pass = 0; pass < count; ++pass) {
            jit.Regs()[0] = address; jit.Regs()[1] = words; jit.Regs()[2] = 1; jit.Regs()[15] = 0x1000;
            jit.ClearHalt(~HaltReason{});
            require(jit.Run() == HaltReason::UserDefined1);
        }
        callbacks += memory.callbacks;
        running.fetch_sub(1);
    };
    const auto before = Eden::Fastmem::Faults();
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < 3; ++i) threads.emplace_back(worker, increment, hot + i * slice, slice / 4, passes);
    threads.emplace_back(worker, scan, cold, cold_bytes / 4, passes * 4);
    unsigned flips = 0, remaps = 0;
    uint32_t seed = 1;
    while (running.load()) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t page = hot + ((seed >> 8) % (hot_bytes / 4096)) * 4096;
        fixture.host.Protect(page, 4096, MemoryPermission{});
        fixture.Entry(page, 0x100000 + (page - hot), true);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        fixture.host.Protect(page, 4096, MemoryPermission::ReadWrite);
        fixture.Entry(page, 0x100000 + (page - hot), false);
        if (++flips % 16 == 0) {
            fixture.host.Unmap(cold, cold_bytes, false);
            for (uint32_t at = 0; at < cold_bytes; at += 4096)
                fixture.pages[(cold + at) >> 12] = reinterpret_cast<void*>(0xab00000000000ffeULL);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            fixture.host.Map(cold, 0x200000, cold_bytes, MemoryPermission::ReadWrite, false);
            for (uint32_t at = 0; at < cold_bytes; at += 4096) fixture.Entry(cold + at, 0x200000 + at, false);
            ++remaps;
        }
    }
    for (auto& thread : threads) thread.join();
    for (uint32_t at = 0; at < hot_bytes; at += 4) {
        uint32_t value; std::memcpy(&value, fixture.backing + 0x100000 + at, 4);
        require(value == passes);
    }
    std::printf("Fastmem A32 stress PASS: %u flips, %u remaps, %llu callbacks, %llu faults\n", flips, remaps,
                callbacks.load(), static_cast<unsigned long long>(Eden::Fastmem::Faults() - before));
}

// Page-table versus checked direct access on the same aliased memory (host Zen 2).
static void BenchFastmemA32() {
    Eden::Fastmem::Request(true);
    FastmemFixture fixture;
    Eden::Fastmem::Request(false);
    require(fixture.host.VirtualBasePointer() != nullptr);
    constexpr uint32_t base = 0x1000000, bytes = 0x400000; // 4 MiB, aliased
    fixture.Map({base, 0, bytes});
    struct Kind { const char* name; std::vector<uint32_t> program; uint32_t count; };
    const Kind kinds[]{
        // ldr r2,[r0],#4; add r4,r4,r2; str r4,[r0,#-4]; subs r1,r1,#1; bne loop; svc #0
        {"stream", {0xe4902004u, 0xe0844002u, 0xe5004004u, 0xe2511001u, 0x1afffffau, 0xef000000u}, bytes / 4},
        // ldr r0,[r0]; subs r1,r1,#1; bne loop; svc #0
        {"chase", {0xe5900000u, 0xe2511001u, 0x1afffffcu, 0xef000000u}, bytes / 64},
        // 8 x ldr r0,[r0]; subs r1,r1,#1; bne loop; svc #0 -- over a 16 KiB random cycle
        {"chase8-l1", {0xe5900000u, 0xe5900000u, 0xe5900000u, 0xe5900000u, 0xe5900000u, 0xe5900000u,
                       0xe5900000u, 0xe5900000u, 0xe2511001u, 0x1afffff5u, 0xef000000u}, 1u << 16},
        // 4 x (ldr r2,[r0,#4]; add r2,r2,#1; str r2,[r0,#4]; ldr r0,[r0]) per iteration: a record walk
        {"walk4-l1", {0xe5902004u, 0xe2822001u, 0xe5802004u, 0xe5900000u,
                      0xe5902004u, 0xe2822001u, 0xe5802004u, 0xe5900000u,
                      0xe5902004u, 0xe2822001u, 0xe5802004u, 0xe5900000u,
                      0xe5902004u, 0xe2822001u, 0xe5802004u, 0xe5900000u,
                      0xe2511001u, 0x1affffedu, 0xef000000u}, 1u << 16},
    };
    for (const auto& kind : kinds) {
        for (bool fastmem : {false, true}) {
            // Pointer chain with a 64-byte stride through the region (the stream rewrites it);
            // the -l1 kinds walk a random cycle over the first 16 KiB instead.
            const bool l1 = std::strstr(kind.name, "-l1") != nullptr;
            const uint32_t span = l1 ? 0x4000 : bytes;
            std::vector<uint32_t> order(span / 64);
            for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
            if (l1) {
                uint32_t seed = 12345;
                for (uint32_t i = order.size() - 1; i > 0; --i) {
                    seed = seed * 1103515245u + 12345u;
                    std::swap(order[i], order[(seed >> 8) % (i + 1)]);
                }
            }
            for (uint32_t i = 0; i < order.size(); ++i) {
                const uint32_t next = base + order[(i + 1) % order.size()] * 64;
                std::memcpy(fixture.backing + order[i] * 64, &next, 4);
            }
            WindowMemory32 memory;
            memory.program = kind.program;
            memory.regions = fixture.regions;
            memory.backing = fixture.backing;
            ExclusiveMonitor monitor{1};
            A32::Jit jit{fixture.Config(memory, monitor, fastmem)};
            memory.jit = &jit;
            double best = 1e9;
            for (unsigned trial = 0; trial < 12; ++trial) {
                jit.Regs()[0] = base + order[0] * 64; jit.Regs()[1] = kind.count; jit.Regs()[4] = 0; jit.Regs()[15] = 0x1000;
                jit.ClearHalt(~HaltReason{});
                const auto start = std::chrono::steady_clock::now();
                require(jit.Run() == HaltReason::UserDefined1);
                const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                if (trial) best = std::min(best, ns / kind.count);
            }
            require(memory.callbacks == 0);
            std::printf("FASTMEM_BENCH kind=%s mode=%s ns_per_iteration=%.3f\n", kind.name,
                        fastmem ? "checked" : "table", best);
        }
    }
}

// Shared compiled code (headless/dynarmic/jit_group.h): four guest cores run one branchy program
// through one page table, so they share one JIT group. Small code caches force whole-group clears
// while the cores run, and another thread invalidates random code ranges and clears the caches;
// every core must still end with the checksum of its own path through the program.
extern "C" bool eden_jit_shared;
extern "C" void eden_jit_path_counters(unsigned core, unsigned long long* out);
static std::atomic<unsigned long long> shared_check_compiles{0};
extern "C" void eden_jit_compile(unsigned, unsigned long long) {
    shared_check_compiles.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void* eden_jit_list_open();
extern "C" void eden_jit_list_close(void* handle);
extern "C" int eden_jit_precompile(void* handle, unsigned long long location);
extern "C" std::size_t eden_jit_history(void* handle, unsigned long long* out, std::size_t capacity);
// same_path: every core starts at the same block, so the cores need the same blocks at once.
// precompile: a fifth thread compiles the program's blocks into the cores' regions meanwhile
// (jit_impl.inc, EdenPrecompile), as a saved block list does at a game's start.
static void CheckSharedJit(unsigned rounds, bool shared, unsigned disturb, bool same_path, bool precompile = false) {
    struct SharedProgram : Memory<false> {
        const std::vector<uint32_t>* program{};
        std::optional<uint32_t> MemoryReadCode(uint64_t pc) override {
            require(pc >= 0x1000 && (pc - 0x1000) % 4 == 0 && (pc - 0x1000) / 4 < program->size());
            return (*program)[(pc - 0x1000) / 4];
        }
    };
    constexpr unsigned blocks = 16384, functions = 64, cores = 4;
    constexpr uint64_t base = 0x1000, end = base + blocks * 32, function_base = end + 4;
    const auto next = [](unsigned i) { return (i * 7919u + 13u) % blocks; };
    const auto slot = [](unsigned i) { return base + uint64_t(i) * 32; };
    const auto branch = [](uint32_t opcode, uint64_t from, uint64_t to) {
        return opcode | uint32_t(((to - from) / 4) & 0x3ffffff);
    };
    std::vector<uint32_t> program;
    for (unsigned i = 0; i < blocks; ++i) {
        const uint64_t pc = slot(i);
        const unsigned function = (i / 8) % functions;
        program.push_back(i % 8 == 0 ? branch(0x94000000u, pc, function_base + function * 8) : 0xd503201fu);  // BL f / NOP
        program.push_back(0x91000021u | ((i & 0xfffu) << 10));                                    // ADD X1, X1, #i
        program.push_back(0xf1000442u);                                                           // SUBS X2, X2, #1
        program.push_back(0x54000000u | uint32_t((((end - (pc + 12)) / 4) & 0x7ffff) << 5));      // B.EQ end
        program.push_back(branch(0x14000000u, pc + 16, slot(next(i))));                           // B next
        for (unsigned pad = 0; pad < 3; ++pad) program.push_back(0xd503201fu);
    }
    program.push_back(0xd4000001u);  // end: SVC #0
    for (unsigned function = 0; function < functions; ++function) {
        program.push_back(0x91000021u | ((function + 1) << 10));  // ADD X1, X1, #function+1
        program.push_back(0xd65f03c0u);                           // RET
    }
    const auto expected = [&](unsigned i, uint64_t steps) {
        uint64_t sum = 0;
        for (;; i = next(i)) {
            if (i % 8 == 0) sum += (i / 8) % functions + 1;
            sum += i & 0xfff;
            if (--steps == 0) return sum;
        }
    };
    const bool previous = eden_jit_shared;
    eden_jit_shared = shared;
    std::vector<void*> pages(1 << 12);
    ExclusiveMonitor monitor{cores};
    std::vector<SharedProgram> memories(cores);
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (unsigned core = 0; core < cores; ++core) {
        memories[core].program = &program;
        auto config = TableConfig(memories[core], pages.data());
        config.global_monitor = &monitor;
        config.processor_id = core;
        config.code_cache_size = 16 * 1024 * 1024;
        jits.push_back(std::make_unique<A64::Jit>(config));
        memories[core].jit = jits.back().get();
    }
    eden_jit_shared = previous;
    uint64_t invalidations = 0, clears = 0;
    std::atomic<unsigned long long> precompiled{0};
    const auto first_block = [&](unsigned core, unsigned round) {
        return ((same_path ? 0 : core * 4099) + round * 31) % blocks;
    };
    shared_check_compiles = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) {
        const uint64_t steps = (disturb > 1 ? 400000 : 200000) + round * 50000;
        for (unsigned core = 0; core < cores; ++core) {
            auto& jit = *jits[core];
            jit.Reset(); jit.ClearHalt(~HaltReason{});
            jit.SetPC(slot(first_block(core, round)));
            jit.SetRegister(1, 0);
            jit.SetRegister(2, steps);
        }
        std::atomic<unsigned> running{cores};
        std::thread disturber([&] {
            uint64_t seed = 0x9e3779b97f4a7c15ULL + round;
            unsigned tick = 0;
            while (disturb && running.load() != 0) {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                const uint64_t from = base + (seed >> 33) % (end - base);
                const size_t length = 4 + (seed >> 20) % 512;
                for (auto& jit : jits) jit->InvalidateCacheRange(from, length);
                ++invalidations;
                if (disturb > 1 && tick++ % 32 == 0) {
                    for (auto& jit : jits) jit->ClearCache();
                    ++clears;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::thread precompiler([&] {
            if (!precompile) return;
            // With the floating-point modes at zero a block's location is its address. A busy
            // answer (2) is tried again, as the block list does.
            void* const list = eden_jit_list_open();
            require(list != nullptr);
            for (unsigned i = round * 977; running.load() != 0;) {
                const int result = eden_jit_precompile(list, slot(i % blocks));
                if (result == 1) precompiled.fetch_add(1);
                if (result == 2) std::this_thread::yield(); else ++i;
            }
            eden_jit_list_close(list);
        });
        std::vector<std::thread> threads;
        for (unsigned core = 0; core < cores; ++core) threads.emplace_back([&, core] {
            while (!Has(jits[core]->Run(), HaltReason::UserDefined1)) {}
            running.fetch_sub(1);
        });
        for (auto& thread : threads) thread.join();
        disturber.join();
        precompiler.join();
        for (unsigned core = 0; core < cores; ++core)
            require(jits[core]->GetRegister(1) == expected(first_block(core, round), steps));
        for (unsigned core = 0; core < cores; ++core) {
            unsigned long long path[8]{};
            eden_jit_path_counters(core, path);
            std::printf("SHARED_JIT_PATH round=%u core=%u runs=%llu thunks=%llu lookups=%llu hits=%llu contended=%llu far=%llu\n",
                        round, core, path[0], path[1], path[2], path[3], path[4], path[7]);
        }
        std::printf("SHARED_JIT_ROUND shared=%u disturb=%u same=%u round=%u seconds=%.3f compiles=%llu\n", unsigned(shared),
                    disturb, unsigned(same_path), round,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    static_cast<unsigned long long>(shared_check_compiles.load()));
        std::fflush(stdout);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (disturb > 1)
        require(clears > 0);
    if (precompile)
        require(shared && precompiled.load() > 0);
    std::printf("Shared JIT %s disturb=%u same=%u PASS: %u rounds x %u cores, %llu compilations, %llu invalidations, "
                "%llu clears, %llu precompiled, %.3f s\n", shared ? "shared" : "per-core", disturb, unsigned(same_path),
                rounds, cores, static_cast<unsigned long long>(shared_check_compiles.load()),
                static_cast<unsigned long long>(invalidations), static_cast<unsigned long long>(clears),
                precompiled.load(), seconds);
}

// A saved block list: the locations one session published, compiled before the next session's
// cores start, leave those cores (almost) nothing to compile, and the same results.
static void CheckBlockList() {
    struct ListProgram : Memory<false> {
        const std::vector<uint32_t>* program{};
        std::optional<uint32_t> MemoryReadCode(uint64_t pc) override {
            if (pc < 0x1000 || (pc - 0x1000) % 4 != 0 || (pc - 0x1000) / 4 >= program->size()) return std::nullopt;
            return (*program)[(pc - 0x1000) / 4];
        }
    };
    constexpr unsigned blocks = 4096, cores = 4;
    constexpr uint64_t base = 0x1000, end = base + blocks * 16, steps = 60000;
    const auto next = [](unsigned i) { return (i * 3571u + 7u) % blocks; };
    std::vector<uint32_t> program;
    for (unsigned i = 0; i < blocks; ++i) {
        const uint64_t pc = base + uint64_t(i) * 16;
        program.push_back(0x91000021u | ((i & 0xfffu) << 10));                                // ADD X1, X1, #i
        program.push_back(0xf1000442u);                                                       // SUBS X2, X2, #1
        program.push_back(0x54000000u | uint32_t((((end - (pc + 8)) / 4) & 0x7ffff) << 5));   // B.EQ end
        program.push_back(0x14000000u | uint32_t((((base + uint64_t(next(i)) * 16) - (pc + 12)) / 4) & 0x3ffffff));
    }
    program.push_back(0xd4000001u);  // end: SVC #0
    const auto expected = [&](unsigned i) {
        uint64_t sum = 0;
        for (uint64_t left = steps;; i = next(i)) {
            sum += i & 0xfff;
            if (--left == 0) return sum;
        }
    };
    const bool previous = eden_jit_shared;
    std::vector<unsigned long long> list;
    unsigned long long cold = 0, warm = 0, ahead = 0;
    for (unsigned session = 0; session < 2; ++session) {
        eden_jit_shared = true;
        std::vector<void*> pages(1 << 12);
        ExclusiveMonitor monitor{cores};
        std::vector<ListProgram> memories(cores);
        std::vector<std::unique_ptr<A64::Jit>> jits;
        for (unsigned core = 0; core < cores; ++core) {
            memories[core].program = &program;
            auto config = TableConfig(memories[core], pages.data());
            config.global_monitor = &monitor;
            config.processor_id = core;
            config.code_cache_size = 16 * 1024 * 1024;
            jits.push_back(std::make_unique<A64::Jit>(config));
            memories[core].jit = jits.back().get();
        }
        eden_jit_shared = previous;
        void* const handle = eden_jit_list_open();
        require(handle != nullptr);
        if (session == 1) {
            // What the first session saved, compiled before any core runs; an address without code
            // and a second request for the same block are harmless.
            for (const unsigned long long location : list)
                if (eden_jit_precompile(handle, location) == 1) ++ahead;
            require(ahead == list.size());
            require(eden_jit_precompile(handle, list.front()) == 0);
            require(eden_jit_precompile(handle, 0x10) >= 0);
        }
        shared_check_compiles = 0;
        for (unsigned core = 0; core < cores; ++core) {
            auto& jit = *jits[core];
            jit.Reset(); jit.ClearHalt(~HaltReason{});
            jit.SetPC(base + uint64_t((core * 1021) % blocks) * 16);
            jit.SetRegister(1, 0);
            jit.SetRegister(2, steps);
        }
        std::vector<std::thread> threads;
        for (unsigned core = 0; core < cores; ++core) threads.emplace_back([&, core] {
            while (!Has(jits[core]->Run(), HaltReason::UserDefined1)) {}
        });
        for (auto& thread : threads) thread.join();
        for (unsigned core = 0; core < cores; ++core)
            require(jits[core]->GetRegister(1) == expected((core * 1021) % blocks));
        (session == 0 ? cold : warm) = shared_check_compiles.load();
        if (session == 0) {
            list.resize(eden_jit_history(handle, nullptr, 0));
            require(!list.empty() && eden_jit_history(handle, list.data(), list.size()) == list.size());
        }
        // The handle outlives the JITs: with them gone, precompiling says stop.
        jits.clear();
        require(eden_jit_precompile(handle, base) == -1);
        eden_jit_list_close(handle);
    }
    require(cold >= blocks && warm * 20 < cold);
    std::printf("Block list PASS: %zu locations saved, %llu compiled ahead, the cores compiled %llu blocks instead of "
                "%llu\n", list.size(), ahead, warm, cold);
}

// The block list's file: offsets from where the code starts, only inside the game's own modules,
// the session's blocks before earlier ones, one build per list, nothing from a damaged file.
static void CheckBlockListFile() {
    using namespace Eden::JitList;
    const Value start = 0x8055c000, image = 0x100000, mode = Value{0x21} << 56;
    const std::vector<Value> session{
        mode | (start + 0x40), start + 0x80, start + 0x40,  // the same address in two modes is two blocks
        mode | (start + 0x40),                              // a repeat
        kSingleStep | (start + 0xc0),                       // single-step blocks are not kept
        start - 4, start + image, start + image + 0x1000,   // outside the game's own modules
    };
    const std::vector<Value> earlier{mode | 0x40, 0x200, image + 8};  // a repeat, a new one, one outside
    const std::vector<Value> entries = Encode(session, start, image, earlier);
    require((entries == std::vector<Value>{mode | 0x40, 0x80, 0x40, 0x200}));
    // The next session's code is somewhere else.
    const Value moved = 0x80550000;
    require(Decode(entries[0], moved) == (mode | (moved + 0x40)) && Decode(entries[3], moved) == moved + 0x200);
    char folder[] = "/tmp/eden-block-list-XXXXXX";
    require(mkdtemp(folder) != nullptr);
    const std::string path = std::string(folder) + "/0100000000000000.blocks";
    BuildId build{}, other{};
    build[0] = 7;
    other[0] = 8;
    require(Load(path, build).empty());
    require(Save(path, build, entries) && Load(path, build) == entries);
    require(Load(path, other).empty());
    require(truncate(path.c_str(), 4 + 32 + 8 + 8 * 3) == 0 && Load(path, build).empty());
    require(Save(path, build, entries) && Load(path, build) == entries);  // replaced whole
    std::remove(path.c_str());
    rmdir(folder);
    std::puts("Block list file PASS: offsets, own modules only, session first, one build, damaged file ignored");
}

// Dispatcher lookups of three hot loops (100k iterations each) once their blocks exist: a
// conditional branch to itself (block link), a call and return (return stack buffer) and an
// indirect branch (fast dispatch). Linked code needs only a handful of lookups per loop.
static void CheckLinks() {
    struct LoopProgram : Memory<false> {
        std::map<uint64_t, uint32_t> code;
        std::optional<uint32_t> MemoryReadCode(uint64_t pc) override {
            const auto it = code.find(pc);
            require(it != code.end());
            return it->second;
        }
    };
    const std::map<uint64_t, uint32_t> programs[] = {
        {{0x1000, 0xf1000442u}, {0x1004, 0x54ffffe1u}, {0x1008, 0xd4000001u}},             // SUBS; B.NE; SVC
        {{0x1000, 0x94000400u}, {0x1004, 0xf1000442u}, {0x1008, 0x54ffffc1u},               // BL f; SUBS; B.NE
         {0x100c, 0xd4000001u}, {0x2000, 0xd65f03c0u}},                                    // SVC; f: RET
        {{0x1000, 0xf1000442u}, {0x1004, 0x54000060u}, {0x1008, 0xd2820003u},               // SUBS; B.EQ end; MOVZ X3
         {0x100c, 0xd61f0060u}, {0x1010, 0xd4000001u}},                                    // BR X3; end: SVC
    };
    const char* names[] = {"block link", "call/return", "indirect branch"};
    std::vector<void*> pages(1 << 12);
    for (unsigned k = 0; k < 3; ++k) {
        LoopProgram memory;
        memory.code = programs[k];
        auto config = TableConfig(memory, pages.data());
        config.processor_id = 0;
        A64::Jit jit{config};
        memory.jit = &jit;
        unsigned long long before[8]{}, after[8]{};
        eden_jit_path_counters(0, before);
        jit.SetPC(0x1000);
        jit.SetRegister(2, 100000);
        while (!Has(jit.Run(), HaltReason::UserDefined1)) {}
        require(jit.GetRegister(2) == 0);
        eden_jit_path_counters(0, after);
        std::printf("LINK_CHECK %s: runs=%llu thunks=%llu lookups=%llu hits=%llu\n", names[k], after[0] - before[0],
                    after[1] - before[1], after[2] - before[2], after[3] - before[3]);
        require(after[1] - before[1] < 16);  // a lookup per iteration means an unlinked path
    }
    std::puts("Linked loops PASS: block link, call/return and indirect branch without per-iteration lookups");
}

// Eden's large tables (src/memory_pages.cpp): zero wherever nothing was written, memory only for
// the 2 MiB slots that were, and a fault for a write that was not announced.
static void CheckSparseTables() {
    constexpr std::size_t slot = std::size_t{2} << 20;
    std::size_t span0 = 0, held0 = 0, span = 0, held = 0;
    Common::SparseUsage(&span0, &held0);
    {
        // A 39-bit address space in 4 KiB pages: 1 GiB of 8-byte entries.
        constexpr std::size_t entries = std::size_t{1} << 27;
        Common::SparseLargeVector<std::uint64_t> table(entries);
        Common::SparseUsage(&span, &held);
        require(span - span0 == entries * 8 && held == held0);
        // Reads anywhere see zero and take nothing: the JIT reads the table for any guest address.
        std::uint64_t seen = 0;
        for (std::size_t index = 0; index < entries; index += 509) seen |= table.data()[index];
        require(seen == 0 && table[0] == 0 && table[entries - 1] == 0);
        Common::SparseUsage(&span, &held);
        require(held == held0);
        // A write that was not announced must fault, as it would on the console.
        const pid_t child = fork();
        require(child >= 0);
        if (child == 0) {
            table.GetUnchecked(entries / 4) = 1;
            _exit(0);
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
        // A written entry reads back, its slot is the only memory taken, its neighbours stay zero.
        table.Set(entries / 2 + 5, 0x1122334455667788ull);
        require(table[entries / 2 + 5] == 0x1122334455667788ull && table[entries / 2 + 4] == 0 &&
                table[entries / 2 - 1] == 0);
        Common::SparseUsage(&span, &held);
        require(held - held0 == slot);
        // A region across a slot boundary takes both slots; announcing it again takes nothing.
        const std::size_t boundary = slot / 8 * 7;
        for (int pass = 0; pass < 2; ++pass) {
            table.CommitRegion(boundary - 10, boundary + 10);
            for (std::size_t index = boundary - 10; index < boundary + 10; ++index) table.GetUnchecked(index) = index;
        }
        Common::SparseUsage(&span, &held);
        require(held - held0 == 3 * slot);
        for (std::size_t index = boundary - 10; index < boundary + 10; ++index) require(table[index] == index);
        // Threads announcing and writing pages of the same few slots at once.
        std::vector<std::thread> threads;
        const std::size_t shared = slot / 8 * 20;
        for (unsigned thread = 0; thread < 6; ++thread)
            threads.emplace_back([&table, shared, thread] {
                for (std::size_t i = 0; i < 40000; ++i) {
                    const std::size_t index = shared + i * 13 + thread;
                    if (index % 6 == thread) table.GetAndFault(index) = index * 3 + 1;
                }
            });
        for (auto& thread : threads) thread.join();
        for (unsigned thread = 0; thread < 6; ++thread)
            for (std::size_t i = 0; i < 40000; ++i) {
                const std::size_t index = shared + i * 13 + thread;
                if (index % 6 == thread) require(table[index] == index * 3 + 1);
            }
        Common::SparseUsage(&span, &held);
        require(held - held0 == 5 * slot); // entries [shared, shared + 520006): two more slots
        // An unmap zeroes what was written and nothing else. Upstream decides by the page of entry
        // start / 8: here that page is written and the range itself never was (it must not be
        // touched), then the other way round (it must be zeroed from its first entry).
        table.ZeroRegion(boundary * 8 + 3, boundary * 8 + 100);
        Common::SparseUsage(&span, &held);
        require(held - held0 == 5 * slot && table[boundary * 8 + 3] == 0);
        table.ZeroRegion(boundary - 7, boundary + 5);
        require(table[boundary - 8] == boundary - 8 && table[boundary + 5] == boundary + 5);
        for (std::size_t index = boundary - 7; index < boundary + 5; ++index) require(table[index] == 0);
        // The same vector emptied and taken again starts from zero.
        table.ResizeAndClear(entries / 2);
        require(table[boundary] == 0 && table[shared + 13] == 0);
        Common::SparseUsage(&span, &held);
        require(span - span0 == entries * 4 && held == held0);
    }
    Common::SparseUsage(&span, &held);
    require(span == span0 && held == held0);
    std::puts("Sparse tables PASS: zero until written, 2 MiB per slot written, a fault for an unannounced write, "
              "six threads on shared slots, an unmap that touches only what was written");
}

int main(int argc, char** argv) {
    CheckSparseTables();
    if (argc == 2 && std::strcmp(argv[1], "--link-check") == 0) {
        CheckLinks();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--shared-jit") == 0) {
        for (bool same_path : {false, true})
        for (unsigned disturb : {0u, 1u, 2u}) {
            CheckSharedJit(4, false, disturb, same_path);
            CheckSharedJit(4, true, disturb, same_path);
            CheckSharedJit(4, true, disturb, same_path, true);
        }
        CheckBlockList();
        CheckBlockListFile();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--compile-chains") == 0) {
        CheckColdCompilation(true);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--fastmem") == 0) {
        CheckFastmemA32();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--fastmem-bench") == 0) {
        BenchFastmemA32();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--fastmem-stress") == 0) {
        StressFastmemA32();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--cold-compile") == 0) {
        CheckColdCompilation();
        return 0;
    }
    // Low mapping makes one absolute offset positive and the other negative.
    auto* backing = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(0x200000), 8192,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(backing != MAP_FAILED && uintptr_t(backing) > 0x100000 && uintptr_t(backing) < 0x400000);
    if (argc == 2 && std::strcmp(argv[1], "--pressure") == 0) {
        CheckPressure(backing, true);
        require(munmap(backing, 8192) == 0);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--exclusive-pressure") == 0) {
        CheckAtomicLoop(backing, true);
        require(munmap(backing, 8192) == 0);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--exclusive32-pressure") == 0) {
        CheckAtomicLoopA32(backing, true);
        require(munmap(backing, 8192) == 0);
        return 0;
    }
    require(argc == 1);
    const auto a64 = Check<false>(backing), a32 = Check<true>(backing);
    CheckPressure(backing, false);
    CheckSimdPressure(backing);
    std::printf("Page-table exclusives PASS: %u cases\n", CheckTableExclusives(backing));
    CheckAtomicLoop(backing, false);
    CheckAtomicLoopA32(backing, false);
    CheckSharedJit(2, true, 2, true);
    CheckSharedJit(2, true, 2, false, true);
    CheckBlockList();
    CheckBlockListFile();
    CheckLinks();
    CheckFastmemA32();
    StressFastmemA32();
    require(munmap(backing, 8192) == 0);
    std::printf("Page-table JIT PASS: A64=%u A32=%u checked loads/stores\n", a64, a32);
}
