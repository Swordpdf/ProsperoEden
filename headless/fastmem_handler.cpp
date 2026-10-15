// SPDX-License-Identifier: GPL-3.0-or-later
// Dynarmic x64 fastmem fault handler for the PS5 port and its Linux host checks, after
// dynarmic's exception_handler_posix.cpp (0BSD). A fault at a fastmem access in JIT code
// becomes a call to that access's fallback, which returns after the faulting instruction;
// dynarmic then recompiles the access onto the page-table path. Guest code runs on
// fiber stacks with ample room, so no alternate signal stack is used.
#include <signal.h>
#include <array>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "../src/fastmem.h"

namespace Dynarmic::Backend {
namespace {
struct CodeRange {
    std::atomic<std::uint64_t> begin{0}, end{0};
    std::atomic<const std::function<FakeCall(u64)>*> callback{nullptr};
};
// One entry per JIT code cache with fastmem (a few per emulated core). Lock-free so the
// handler never waits on a thread that registers or retires a JIT.
std::array<CodeRange, 64> ranges;
std::atomic<std::uint64_t> fault_count{0};
struct sigaction previous_segv{}, previous_bus{};
std::once_flag install_once;
bool installed = false;

void Forward(int signal, siginfo_t* info, void* context) {
    const struct sigaction& previous = signal == SIGSEGV ? previous_segv : previous_bus;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_DFL) {
        std::signal(signal, SIG_DFL); // The faulting instruction repeats and terminates.
    } else if (previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    }
}

void Handle(int signal, siginfo_t* info, void* context) {
    const std::uint64_t pc = Eden::Fastmem::ContextRip(context);
    for (auto& range : ranges) {
        const auto begin = range.begin.load(std::memory_order_acquire);
        if (!begin || pc < begin || pc >= range.end.load(std::memory_order_relaxed)) continue;
        const auto* callback = range.callback.load(std::memory_order_acquire);
        if (!callback) break;
        fault_count.fetch_add(1, std::memory_order_relaxed);
        const FakeCall call = (*callback)(pc);
        auto& sp = Eden::Fastmem::ContextRsp(context);
        sp -= sizeof(std::uint64_t);
        *reinterpret_cast<std::uint64_t*>(sp) = call.ret_rip;
        Eden::Fastmem::ContextRip(context) = call.call_rip;
        return;
    }
    Forward(signal, info, context);
}

bool Install() {
    std::call_once(install_once, [] {
        struct sigaction action{};
        action.sa_sigaction = Handle;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        installed = sigaction(SIGSEGV, &action, &previous_segv) == 0 &&
                    sigaction(SIGBUS, &action, &previous_bus) == 0;
        std::printf("EDEN_FASTMEM_HANDLER installed=%d\n", installed ? 1 : 0);
    });
    return installed;
}
} // namespace

struct ExceptionHandler::Impl final {
    std::uint64_t begin{}, size{};
    std::function<FakeCall(u64)> callback;
    CodeRange* range{};
    ~Impl() {
        if (!range) return;
        range->callback.store(nullptr, std::memory_order_release);
        range->begin.store(0, std::memory_order_release);
    }
};

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

void ExceptionHandler::Register(X64::BlockOfCode& code) {
    impl = std::make_unique<Impl>();
    impl->begin = std::bit_cast<u64>(code.getCode());
    impl->size = code.GetTotalCodeSize();
}

bool ExceptionHandler::SupportsFastmem() const noexcept {
    return impl && impl->range;
}

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    if (!impl || impl->range || !Install()) return;
    impl->callback = std::move(cb);
    for (auto& range : ranges) {
        std::uint64_t expected = 0;
        // Claim a free entry; publish the callback before the range becomes visible.
        if (!range.callback.load(std::memory_order_acquire) &&
            range.begin.compare_exchange_strong(expected, ~std::uint64_t{0}, std::memory_order_acq_rel)) {
            range.end.store(impl->begin + impl->size, std::memory_order_relaxed);
            range.callback.store(&impl->callback, std::memory_order_release);
            range.begin.store(impl->begin, std::memory_order_release);
            impl->range = &range;
            return;
        }
    }
    std::printf("EDEN_FASTMEM_HANDLER ranges=full\n");
}

} // namespace Dynarmic::Backend

namespace Dynarmic::Backend::X64 {
namespace {
std::atomic<std::uint64_t> demotion_count{0};
std::atomic<bool> sites_enabled{true};
}
// Whether A32 loads/stores may use the window (headless/checked-fastmem.cmake).
bool CheckedFastmemSites() noexcept {
    return sites_enabled.load(std::memory_order_relaxed);
}
// A checked fastmem site recompiled onto the page-table path after its blocked budget
// (headless/checked-fastmem.cmake).
void CheckedFastmemDemoted() noexcept {
    demotion_count.fetch_add(1, std::memory_order_relaxed);
}
} // namespace Dynarmic::Backend::X64

namespace Eden::Fastmem {
std::uint64_t Faults() noexcept {
    return Dynarmic::Backend::fault_count.load(std::memory_order_relaxed);
}
std::uint64_t Demotions() noexcept {
    return Dynarmic::Backend::X64::demotion_count.load(std::memory_order_relaxed);
}
void RequestSites(bool enabled) noexcept {
    Dynarmic::Backend::X64::sites_enabled.store(enabled, std::memory_order_relaxed);
}
bool SitesRequested() noexcept {
    return Dynarmic::Backend::X64::CheckedFastmemSites();
}
} // namespace Eden::Fastmem
