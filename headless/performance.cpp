// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance.h"
#include "crash_report.h"
#include "stall_watchdog.h"
#include "../src/fastmem.h"
#include "common/cpu_features.h"
#include "common/sparse_large_vector.h"
#include <algorithm>
#include <string>
#include <set>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cpuid.h>
#include <cstring>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <latch>
#include <time.h>
#ifdef EDEN_DEV_PROFILE
#include <signal.h>
#endif
#ifdef PS5_NATIVE
#include <sys/param.h>
#include <sys/cpuset.h>
#endif

// dynarmic A64 dispatch-path counters (headless/dynarmic/jit_group_support.inc).
extern "C" void eden_jit_path_counters(unsigned core, unsigned long long* out) __attribute__((weak));
namespace Common {
// Address space Eden's sparse tables span and the memory they hold (src/memory_pages.cpp).
void SparseUsage(std::size_t* reserved, std::size_t* committed) noexcept;
} // namespace Common

namespace Eden::Performance {
namespace {
constexpr std::array names{"CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3",
                           "GPU", "HostTiming", "VSyncThread"};
struct Worker {
    pthread_t thread{};
    clockid_t clock{};
    int clock_error = ENOENT, affinity_error = ENOENT, allowed = 0;
    unsigned long long mask = 0;
    long long wall_ns = 0, mono_ns = 0;
    bool registered = false;
};
std::mutex workers_mutex;
std::array<Worker, names.size()> workers;
#ifdef EDEN_DEV_PROFILE
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
std::array<uintptr_t, 8192> sampled_pcs{};
std::atomic<unsigned> pc_count{};
// Guest core 0 host PCs (JIT code, HLE, memory callbacks), sampled with the GPU thread.
std::array<uintptr_t, 65536> sampled_core_pcs{};
std::atomic<unsigned> core_pc_count{};
unsigned core_pc_reported{};
pthread_t core_sample_thread{};
std::atomic<bool> core_sample_ready{};
unsigned pc_reported{};
bool pc_sampling{};
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
// Same firmware6.02 register offsets qualified by the C46 caller sampler.
constexpr size_t runtime_rsp_offset = 248, runtime_rbp_offset = 136;
using CallerChain = std::array<uintptr_t, 8>;
std::array<CallerChain, 8192> sampled_callers{};
// Core0 keeps a chain only for PCs outside our own code: the eboot and the JIT caches name
// themselves by address, and the rest measured at a tenth of its samples, so a bounded array
// covers the run without a 4 MB one.
std::array<CallerChain, 4096> sampled_core_callers{};
std::array<uintptr_t, 4096> core_caller_pcs{};
std::atomic<unsigned> core_caller_count{};
unsigned core_caller_reported{};

CallerChain CaptureCallerChain(uintptr_t rsp, uintptr_t frame, uintptr_t handler_stack) {
    CallerChain callers{};
    // Read only aligned frames in the interrupted stack-top page. No allocation,
    // symbolization or logging in the signal handler. Reject unknown/alternate stacks.
    if (rsp <= 0x10000 || rsp < handler_stack || rsp - handler_stack >= 65536 ||
        (rsp & 4095) > 4096 - 8 * sizeof(uintptr_t)) return callers;
    for (unsigned i = 0; i < callers.size(); ++i) {
        if (frame < rsp || (frame >> 12) != (rsp >> 12) ||
            (frame & (alignof(uintptr_t) - 1)) ||
            (frame & 4095) > 4096 - 2 * sizeof(uintptr_t)) break;
        const auto* chain = reinterpret_cast<const uintptr_t*>(frame);
        callers[i] = chain[1];
        if (chain[0] <= frame || chain[0] - frame > 4096) break;
        frame = chain[0];
    }
    return callers;
}
#endif
// Core 0's A32 blocks in emission order (P0): dynarmic's code cache grows linearly until a full
// clear, so entries stay sorted and a sampled host PC finds its guest block by binary search.
struct JitBlock {
    uintptr_t entry;
    unsigned long long size, location;
};
constexpr std::size_t jit_block_capacity = std::size_t{1} << 20;
JitBlock* core0_blocks = nullptr;
std::atomic<std::size_t> core0_block_count{0};

void PcSignal(int, siginfo_t*, void* context) {
    if (core_sample_ready.load(std::memory_order_acquire) &&
        pthread_equal(pthread_self(), core_sample_thread)) {
        const unsigned core_index = core_pc_count.load(std::memory_order_relaxed);
        if (core_index < sampled_core_pcs.size()) {
            sampled_core_pcs[core_index] = static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)];
            core_pc_count.store(core_index + 1, std::memory_order_release);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
            const uintptr_t pc = sampled_core_pcs[core_index];
            const unsigned caller_index = core_caller_count.load(std::memory_order_relaxed);
            // Library PCs only: eboot and the JIT caches identify themselves by address.
            if (caller_index < core_caller_pcs.size() &&
                (pc < 0x400000 || pc > 0x28c2b30) &&
                (pc < 0x1200000000 || pc >= 0x1300000000)) {
                const auto* words = static_cast<const uintptr_t*>(context);
                char handler_stack_marker;
                sampled_core_callers[caller_index] = CaptureCallerChain(
                    words[runtime_rsp_offset / sizeof(uintptr_t)],
                    words[runtime_rbp_offset / sizeof(uintptr_t)],
                    reinterpret_cast<uintptr_t>(&handler_stack_marker));
                core_caller_pcs[caller_index] = pc;
                core_caller_count.store(caller_index + 1, std::memory_order_release);
            }
#endif
        }
        return;
    }
    const unsigned index = pc_count.load(std::memory_order_relaxed);
    if (index >= sampled_pcs.size()) return;
    // Native firmware 6.02 ABI, qualified by the standalone PC-sampling oracle.
    // SDK FreeBSD ucontext.mc_rip points at the wrong field on this firmware.
    sampled_pcs[index] = static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)];
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    const auto* words = static_cast<const uintptr_t*>(context);
    char handler_stack_marker;
    sampled_callers[index] = CaptureCallerChain(words[runtime_rsp_offset / sizeof(uintptr_t)],
        words[runtime_rbp_offset / sizeof(uintptr_t)], reinterpret_cast<uintptr_t>(&handler_stack_marker));
#endif
    pc_count.store(index + 1, std::memory_order_release);
}
#endif
#ifdef PS5_NATIVE
// Whether logical CPUs 0 and 1 are one physical core, by timing. Four multiply chains on each of
// two CPUs at once: siblings take the core's one multiplier in turns and need about twice as long
// as two CPUs of different cores. The shortest of three runs of each pair, for whatever else runs
// meanwhile. (CheckWorkerTopology asks this where CPUID does not tell the cores apart.)
long long PairLoopNs(unsigned cpu_a, unsigned cpu_b) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::array<long long, 2> elapsed{-1, -1};
    const auto body = [&](unsigned cpu, unsigned slot) {
        cpuset_t one{};
        CPU_SET(cpu, &one);
        const bool placed = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one) == 0;
        sched_yield();
        ready.fetch_add(1, std::memory_order_release);
        if (!placed) return;
        // Bounded: threads of one priority are not time-sliced here.
        for (unsigned spins = 0; !go.load(std::memory_order_acquire) && spins < 200'000'000; ++spins) {}
        const long long begin = NowNs();
        unsigned long long a = cpu + 1, b = 3, c = 5, d = 7;
        for (unsigned i = 0; i < 1'500'000; ++i) {
            a = a * 6364136223846793005ULL + 1;
            b = b * 2862933555777941757ULL + 3;
            c = c * 3202034522624059733ULL + 5;
            d = d * 1442695040888963407ULL + 7;
        }
        volatile unsigned long long sink = a ^ b ^ c ^ d;
        (void)sink;
        elapsed[slot] = NowNs() - begin;
    };
    std::thread first(body, cpu_a, 0u), second(body, cpu_b, 1u);
    while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    return elapsed[0] < 0 || elapsed[1] < 0 ? -1 : std::max(elapsed[0], elapsed[1]);
}
bool AdjacentCpusShareCore() {
    long long together = -1, apart = -1;
    for (int run = 0; run < 3; ++run) {
        const long long pair = PairLoopNs(0, 1), others = PairLoopNs(0, 2);
        if (pair > 0 && (together < 0 || pair < together)) together = pair;
        if (others > 0 && (apart < 0 || others < apart)) apart = others;
    }
    const bool share = together > 0 && apart > 0 && together * 4 >= apart * 5;
    std::printf("EDEN_WORKER_SMT cpus_0_1_ns=%lld cpus_0_2_ns=%lld share=%d\n", together, apart, share);
    return share;
}
std::array<unsigned, 5> worker_cpus{};
bool worker_topology_ready = false;
// Physical core (x2APIC above the SMT shift) of each allowed CPU, -1 if unknown.
std::array<int, 64> cpu_core = [] { std::array<int, 64> c{}; c.fill(-1); return c; }();
// CPUs outside guest cores 0-2 and their SMT siblings, and not the GPU thread CPU.
unsigned long long secondary_cpus = 0;
// The process CPU set seen by the topology probe (threads may inherit narrower sets).
cpuset_t topology_allowed{};
bool topology_allowed_valid = false;
std::atomic<bool> placement_secondary{false};
std::atomic<unsigned> secondary_reports{};
void CheckWorkerTopology() {
    cpuset_t original{};
    if (__get_cpuid_max(0, nullptr) < 0xb ||
        cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original)) return;
    std::array<unsigned, 5> cores{};
    unsigned count = 0;
    bool changed = false, refused = false, measured = false;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        cpuset_t one{};
        CPU_SET(cpu, &one);
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) {
            refused = true;
            break;
        }
        changed = true;
        unsigned a, b, c, d;
        __cpuid_count(0xb, 0, a, b, c, d);
        // Architectural SMT level: x2APIC ID above the SMT shift identifies
        // the physical core. Do not assume OS CPU numbering matches APIC IDs.
        if (!b || ((c >> 8) & 0xff) != 1 || (a & 31) >= 16) break;
        const unsigned core = d >> (a & 31);
        cpu_core[cpu] = static_cast<int>(core);
        if (count == cores.size() ||
            std::find(cores.begin(), cores.begin() + count, core) != cores.begin() + count) continue;
        cores[count] = core;
        worker_cpus[count++] = cpu;
    }
    if (changed) {
        cpuset_t restored{};
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original) ||
            cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &restored) ||
            std::memcmp(&original, &restored, 8))
            throw std::runtime_error("Cannot restore topology-probe thread affinity");
    }
    // CPUID did not tell five physical cores apart: newer system software gives one answer for
    // every CPU (a console with it ran every thread unpinned: a large open-world game at 20
    // frames a second where a console with pinned threads made 26 to 29). Where CPUID does
    // answer, logical CPUs 2n and 2n+1 are one core; a timing of two pairs says whether that
    // holds here (AdjacentCpusShareCore), and then the same five CPUs are used.
    if (!refused && count != cores.size()) {
        unsigned even = 0;
        for (unsigned cpu = 0; cpu < 64; cpu += 2) even += CPU_ISSET(cpu, &original) ? 1 : 0;
        if (even >= cores.size() && CPU_ISSET(1, &original) && AdjacentCpusShareCore()) {
            count = 0;
            cpu_core.fill(-1);
            for (unsigned cpu = 0; cpu < 64; ++cpu) {
                if (!CPU_ISSET(cpu, &original)) continue;
                cpu_core[cpu] = static_cast<int>(cpu / 2);
                if ((cpu & 1) == 0 && count < cores.size()) {
                    cores[count] = cpu / 2;
                    worker_cpus[count++] = cpu;
                }
            }
            measured = true;
        }
    }
    worker_topology_ready = count == cores.size();
    topology_allowed = original;
    topology_allowed_valid = true;
    if (worker_topology_ready) {
        for (unsigned cpu = 0; cpu < 64; ++cpu) {
            if (!CPU_ISSET(cpu, &original) || cpu_core[cpu] < 0) continue;
            const bool guest_core = cpu_core[cpu] == static_cast<int>(cores[0]) ||
                cpu_core[cpu] == static_cast<int>(cores[1]) || cpu_core[cpu] == static_cast<int>(cores[2]);
            if (!guest_core && cpu != worker_cpus[3] && cpu != worker_cpus[4]) secondary_cpus |= 1ULL << cpu;
        }
        std::printf("EDEN_WORKER_SECONDARY mask=%llx\n", secondary_cpus);
    }
    std::printf("EDEN_WORKER_TOPOLOGY ready=%d distinct_cores=%u cpus=%u,%u,%u,%u,%u measured=%d\n",
        worker_topology_ready, count, worker_cpus[0], worker_cpus[1], worker_cpus[2], worker_cpus[3], worker_cpus[4],
        static_cast<int>(measured));
}
void PlaceSecondary(const char* name) {
    if (!placement_secondary.load(std::memory_order_relaxed) || !secondary_cpus) return;
    cpuset_t mask{};
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    if (secondary_reports.fetch_add(1, std::memory_order_relaxed) < 64)
        std::printf("EDEN_WORKER_PLACED name=%s mask=%llx error=%d\n", name, secondary_cpus, result ? errno : 0);
}
void PinWorker(unsigned index, cpuset_t& affinity) {
    if (!worker_topology_ready || index >= worker_cpus.size() ||
        !CPU_ISSET(worker_cpus[index], topology_allowed_valid ? &topology_allowed : &affinity)) return;
    cpuset_t one{}, verified{};
    CPU_SET(worker_cpus[index], &one);
    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) {
        std::printf("EDEN_WORKER_PIN name=%s error=%d\n", names[index], errno);
        return;
    }
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &verified) ||
        std::memcmp(&one, &verified, 8)) {
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &affinity))
            throw std::runtime_error("Cannot restore worker affinity");
        return;
    }
    affinity = verified;
    std::printf("EDEN_WORKER_PIN name=%s cpu=%u verified=1\n", names[index], worker_cpus[index]);
}
#endif
bool cpu_clocks_valid = false;
bool owner_cpu_clock_valid = false;
std::atomic<unsigned> sample_epoch{};
struct CpuSample {
    unsigned epoch = ~0u;
    long long mono_ns = 0, cpu_ns = -ENOTSUP;
    unsigned long long thread = 0, pc = 0;
    unsigned svc = ~0u, fpcr = 0;
    unsigned long long compilations = 0, compile_ns = 0;
};
std::array<CpuSample, 4> cpu_samples;
// Read/written only by the worker owning this core; samples use workers_mutex.
std::array<unsigned, 4> sampled_epoch{~0u, ~0u, ~0u, ~0u};
clockid_t process_clock{};
int CurrentCpuClock(clockid_t* clock, bool process = false) {
#ifdef PS5_NATIVE
    // Native titles must use the imported kernel APIs. Firmware rejects direct
    // syscalls from the title. Qualification below rejects unusable CPU clocks.
    if (process) { *clock = CLOCK_PROCESS_CPUTIME_ID; return 0; }
    return pthread_getcpuclockid(pthread_self(), clock);
#else
    return process ? clock_getcpuclockid(0, clock) : pthread_getcpuclockid(pthread_self(), clock);
#endif
}
long long ClockNs(clockid_t clock) {
    timespec time{};
    if (clock_gettime(clock, &time) != 0) return -errno;
    return static_cast<long long>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
void CheckCpuClocks() {
    clockid_t worker_clock{};
    int worker_error = ENOENT;
    const int process_error = CurrentCpuClock(&process_clock, true);
    long long owner_begin = -1, owner_busy = -1, owner_idle = -1;
    std::binary_semaphore ready{0}, proceed{0};
    std::jthread worker([&] {
        worker_error = CurrentCpuClock(&worker_clock);
        ready.release();
        proceed.acquire();
        owner_begin = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
        owner_busy = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        ready.release();
        proceed.acquire();
        owner_idle = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    });
    ready.acquire();
    const auto cpu_begin = worker_error ? -1 : ClockNs(worker_clock);
    proceed.release();
    ready.acquire();
    const auto cpu_busy = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_idle = process_error ? -1 : ClockNs(process_clock);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto cpu_idle = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_end = process_error ? -1 : ClockNs(process_clock);
    proceed.release();
    worker.join();
    const auto own_busy = owner_busy - owner_begin;
    const auto own_idle = owner_idle - owner_busy;
    owner_cpu_clock_valid = owner_begin >= 0 && own_busy >= 5'000'000 &&
        own_busy < 30'000'000 && own_idle >= 0 && own_idle < own_busy / 4 + 1'000'000;
    std::printf("EDEN_PERF_OWNER_CLOCK_CHECK valid=%d busy_ns=%lld idle_ns=%lld\n",
                owner_cpu_clock_valid, own_busy, own_idle);
    const auto busy = cpu_busy - cpu_begin;
    const auto idle = cpu_idle - cpu_busy;
    const auto process_sleep = process_end - process_idle;
    cpu_clocks_valid = !worker_error && !process_error && cpu_begin >= 0 && process_idle >= 0 &&
        busy >= 1'000'000 && idle >= 0 && idle < busy / 4 + 1'000'000 &&
        process_sleep >= 0 && process_sleep < 6'000'000;
    std::printf("EDEN_PERF_CPU_CLOCK_CHECK valid=%d worker_error=%d process_error=%d "
                "worker_clock=%d process_clock=%d busy_ns=%lld idle_ns=%lld process_sleep_ns=%lld\n",
                cpu_clocks_valid, worker_error, process_error, int(worker_clock), int(process_clock),
                busy, idle, process_sleep);
}
}

void SetSecondaryPlacement(bool enabled) {
#ifdef PS5_NATIVE
    placement_secondary.store(enabled, std::memory_order_relaxed);
    if (!secondary_cpus || !topology_allowed_valid) return;
    cpuset_t mask{};
    if (enabled) {
        for (unsigned cpu = 0; cpu < 64; ++cpu)
            if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    } else {
        mask = topology_allowed;
    }
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    std::printf("EDEN_WORKER_INHERIT enabled=%d mask=%llx error=%d\n", enabled,
                enabled ? secondary_cpus : 0ULL, result ? errno : 0);
#else
    (void)enabled;
#endif
}

void SampleGpuFrame(unsigned frame) {
    std::printf("EDEN_PERF_GPU_FRAME frame=%u mono_ns=%lld cpu_ns=%lld\n", frame,
                ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP));
}

extern "C" void ps5_opengl_heap_snapshot(const char* phase, unsigned iteration);
extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));
extern "C" std::size_t eden_heap_committed(void) __attribute__((weak));
extern "C" std::size_t eden_heap_large_held(unsigned* blocks) __attribute__((weak));
#ifdef PS5_NATIVE
extern "C" std::int64_t sceKernelGetDirectMemorySize();
extern "C" std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t*,
                                                           std::size_t*);
// The allocated region at or after an offset (flag 1: find the next one).
struct DirectMemoryRegion { std::int64_t start; std::int64_t end; std::int32_t type; };
extern "C" std::int32_t sceKernelDirectMemoryQuery(std::int64_t, int, DirectMemoryRegion*, std::size_t);
#endif

#ifdef PS5_NATIVE
// The largest free block of direct memory is what the next graphics allocation needs. Looked at
// again every 100 ms; called on the GPU thread by the texture collector (KeepDirtyTextures).
static std::atomic<unsigned long long> largest_free_block{0};
static void RefreshFreeMemory() {
    static std::atomic<long long> checked_ns{0};
    const long long now = NowNs();
    if (const long long last = checked_ns.load(std::memory_order_relaxed); last != 0 && now - last < 100'000'000)
        return;
    checked_ns.store(now, std::memory_order_relaxed);
    std::int64_t start = 0;
    std::size_t largest = 0;
    const std::int64_t total = sceKernelGetDirectMemorySize();
    const bool known = total > 0 && sceKernelAvailableDirectMemorySize(0, total, 0x4000, &start, &largest) == 0;
    largest_free_block.store(known ? largest : 0, std::memory_order_relaxed);
    graphics_memory_short.store(!known || largest < kShortMemory, std::memory_order_relaxed);
}
static bool GraphicsMemoryShort() {
    RefreshFreeMemory();
    return graphics_memory_short.load(std::memory_order_relaxed);
}
// What the caches' "memory in use" is measured against (performance.h, graphics_memory_free).
static unsigned long long GraphicsMemoryFree() {
    RefreshFreeMemory();
    return largest_free_block.load(std::memory_order_relaxed);
}
[[maybe_unused]] static const bool graphics_memory_probe_installed =
    (graphics_memory_probe.store(&GraphicsMemoryShort, std::memory_order_relaxed),
     graphics_memory_free.store(&GraphicsMemoryFree, std::memory_order_relaxed), true);
#endif

namespace {
std::mutex hle_mutex;
// Keyed by the service's name pointer (one per service object) and command id.
std::map<std::pair<const char*, unsigned>, std::pair<unsigned long long, unsigned long long>> hle_calls;
} // namespace

void RecordHle(const char* service, unsigned command, long long ns) {
    std::lock_guard lock(hle_mutex);
    auto& entry = hle_calls[{service, command}];
    ++entry.first;
    entry.second += static_cast<unsigned long long>(ns > 0 ? ns : 0);
}

void ReportSessionMemory(const char* phase, unsigned session) {
    unsigned large_blocks = 0;
    const std::size_t large = eden_heap_large_held ? eden_heap_large_held(&large_blocks) : 0;
    std::size_t table_span = 0, table_held = 0;
    ::Common::SparseUsage(&table_span, &table_held);
    long long total = 0, free_bytes = -1;
    std::size_t largest = 0;
    unsigned regions = 0;
#ifdef PS5_NATIVE
    total = sceKernelGetDirectMemorySize();
    if (total > 0) {
        std::int64_t start = 0, taken = 0, offset = 0;
        (void)sceKernelAvailableDirectMemorySize(0, total, 0x4000, &start, &largest);
        DirectMemoryRegion region{};
        while (offset < total && regions < 8192 &&
               sceKernelDirectMemoryQuery(offset, 1, &region, sizeof(region)) == 0 && region.end > offset) {
            taken += region.end - region.start;
            offset = region.end;
            ++regions;
        }
        if (regions != 0) free_bytes = total - taken;
    }
#endif
    std::printf("EDEN_SESSION_MEMORY phase=%s session=%u total=%lld free=%lld largest_free=%zu regions=%u "
                "heap_pieces=%zu heap_large=%zu large_blocks=%u tables=%zu\n",
                phase, session, total, free_bytes, largest, regions,
                eden_heap_committed ? eden_heap_committed() : std::size_t{0}, large, large_blocks, table_held);
    std::fflush(stdout);
}

void ReportGpuThread(unsigned frame) {
    // Heap growth and arena use over the run (allocation failures abort the title).
    ps5_opengl_heap_snapshot("vulkan_report", frame);
    {
        // What the heap and Eden's large tables hold of the memory the CPU shares with the GPU:
        // the heap's pieces (they only grow), its large blocks alive now, and the tables' own
        // slots out of the address range they span.
        unsigned large_blocks = 0;
        const std::size_t large = eden_heap_large_held ? eden_heap_large_held(&large_blocks) : 0;
        std::size_t table_span = 0, table_held = 0;
        ::Common::SparseUsage(&table_span, &table_held);
        std::printf("EDEN_PERF_HEAP arenas=%u pieces=%zu large=%zu large_blocks=%u tables=%zu table_span=%zu\n",
                    eden_heap_arenas_created ? eden_heap_arenas_created() : 1u,
                    eden_heap_committed ? eden_heap_committed() : std::size_t{0}, large, large_blocks, table_held,
                    table_span);
    }
    {
        // Cumulative HLE handling time of every service command that has cost at least 1 ms.
        std::lock_guard lock(hle_mutex);
        for (const auto& [key, value] : hle_calls)
            if (value.second >= 1'000'000)
                std::printf("EDEN_DEV_HLE service=%s cmd=%u calls=%llu ns=%llu\n", key.first ? key.first : "?",
                            key.second, value.first, value.second);
    }
#ifdef PS5_NATIVE
    // Direct memory headroom (guest RAM, JIT, RADV and the caches share the pool).
    if (const std::int64_t total = sceKernelGetDirectMemorySize(); total > 0) {
        std::int64_t start = 0;
        std::size_t largest = 0;
        if (sceKernelAvailableDirectMemorySize(0, total, 0x4000, &start, &largest) == 0) {
            // All that is free, against the largest block: how far apart they are tells how much
            // of the pool is holes between allocations. -1: the regions could not be read.
            std::int64_t taken = 0, offset = 0;
            unsigned regions = 0;
            DirectMemoryRegion region{};
            while (offset < total && regions < 8192 &&
                   sceKernelDirectMemoryQuery(offset, 1, &region, sizeof(region)) == 0 && region.end > offset) {
                taken += region.end - region.start;
                offset = region.end;
                ++regions;
            }
            std::printf("EDEN_PERF_DIRECT total=%lld largest_free=%zu free=%lld regions=%u short=%d\n",
                        static_cast<long long>(total), largest,
                        regions != 0 ? static_cast<long long>(total - taken) : -1LL, regions,
                        int(graphics_memory_short.load(std::memory_order_relaxed)));
        }
    }
#endif
    const auto load = [](const Totals& totals, bool calls) {
        return (calls ? totals.calls : totals.nanoseconds).load(std::memory_order_relaxed);
    };
    // Cumulative totals; the analyzer takes deltas between consecutive reports.
    std::printf("EDEN_DEV_GPU frame=%u mono_ns=%lld cpu_ns=%lld idle_ns=%llu dispatch_calls=%llu "
                "dispatch_ns=%llu drain_calls=%llu drain_ns=%llu present_calls=%llu present_ns=%llu "
                "full_calls=%llu full_ns=%llu draws=%llu\n", frame, ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP),
                load(gpu_queue_wait, false), load(gpu_dispatch, true), load(gpu_dispatch, false),
                load(gpu_fence_drain, true), load(gpu_fence_drain, false),
                load(gpu_present_wait, true), load(gpu_present_wait, false),
                load(gpu_queue_full, true), load(gpu_queue_full, false), load(rasterizer_draw, true));
    // What the dispatch is made of: the draw path itself (RasterizerVulkan::PrepareDraw - both
    // cache locks, pipeline configure, dynamic state, the record) against the guest method
    // decoding that surrounds it, plus the dispatch's own three phases. A draw whose
    // EDEN_DEV_DRAW ns is close to EDEN_GPU dispatch_ns is being charged to the rasterizer; one
    // far below it is being charged to the dma pusher.
    std::printf("EDEN_DEV_DRAW calls=%llu ns=%llu\n", load(rasterizer_draw, true),
                load(rasterizer_draw, false));
    std::printf("EDEN_DEV_PUSH lock_ns=%llu push_ns=%llu dispatch_ns=%llu\n",
                load(gpu_sched_lock, false), load(gpu_dma_push, false),
                load(gpu_dma_dispatch, false));
    // Where that draw time went, in order. flush is FlushWork plus the memory flush; pipeline is
    // taking the current pipeline; lock_wait is the wait for both cache mutexes (the hold is what
    // the later phases measure); config is SetEngine plus Configure; state is UpdateDynamicStates
    // and the query, transform-feedback and counter calls; record is draw_func. unattributed is
    // the early-return tails the outer Timer charged but no phase marked, so it should be small,
    // and the _calls columns say how often each phase ran - record below flush is draws that bailed.
    std::printf("EDEN_DEV_RAST flush_ns=%llu pipeline_ns=%llu lock_wait_ns=%llu config_ns=%llu "
                "state_ns=%llu record_ns=%llu unattributed_ns=%llu flush_calls=%llu "
                "pipeline_calls=%llu lock_wait_calls=%llu config_calls=%llu state_calls=%llu "
                "record_calls=%llu\n",
                load(draw_flush, false), load(draw_pipeline, false),
                load(draw_lock_wait, false), load(draw_config, false), load(draw_state, false),
                load(draw_record, false),
                load(rasterizer_draw, false) - (load(draw_flush, false) + load(draw_pipeline, false) +
                                                load(draw_lock_wait, false) +
                                                load(draw_config, false) + load(draw_state, false) +
                                                load(draw_record, false)),
                load(draw_flush, true), load(draw_pipeline, true), load(draw_lock_wait, true),
                load(draw_config, true), load(draw_state, true), load(draw_record, true));
    // Where config's time went. sync is the sampler and image table sync plus the uniform-buffer
    // state; read is the guest-memory handle reads and the view fill; bind is the buffer binds;
    // desc is the descriptor queue, the pushes and the upload barrier; rt is render targets and
    // feedback; draw is ConfigureDraw's compare-and-record. These nest inside config, so
    // unattributed (config less their sum) is the entry clock, SetEngine, the configure_func
    // pointer call and any early return that left the tail unmarked - a negative value means the
    // marks themselves cost more than that.
    std::printf("EDEN_DEV_CFG sync_ns=%llu read_ns=%llu bind_ns=%llu desc_ns=%llu rt_ns=%llu "
                "draw_ns=%llu unattributed_ns=%llu sync_calls=%llu read_calls=%llu "
                "bind_calls=%llu desc_calls=%llu rt_calls=%llu draw_calls=%llu\n",
                load(cfg_sync, false), load(cfg_read, false), load(cfg_bind, false),
                load(cfg_desc, false), load(cfg_rt, false), load(cfg_draw, false),
                load(draw_config, false) - (load(cfg_sync, false) + load(cfg_read, false) +
                                            load(cfg_bind, false) + load(cfg_desc, false) +
                                            load(cfg_rt, false) + load(cfg_draw, false)),
                load(cfg_sync, true), load(cfg_read, true), load(cfg_bind, true),
                load(cfg_desc, true), load(cfg_rt, true), load(cfg_draw, true));
    // Where cfg_bind's time went: tbo is the texture-buffer loop over the enabled stages, update
    // is the index, vertex, transform-feedback and per-stage buffer updates, geometry is the host
    // binds after them. They nest inside cfg_bind, so unattributed (cfg_bind less their sum) is
    // the transform-feedback check, the two pointer-store statements and the clock itself.
    std::printf("EDEN_DEV_BIND tbo_ns=%llu update_ns=%llu geometry_ns=%llu "
                "unattributed_ns=%llu tbo_calls=%llu update_calls=%llu geometry_calls=%llu\n",
                load(bind_tbo, false), load(bind_update, false), load(bind_geometry, false),
                load(cfg_bind, false) - (load(bind_tbo, false) + load(bind_update, false) +
                                         load(bind_geometry, false)),
                load(bind_tbo, true), load(bind_update, true), load(bind_geometry, true));
    // The four calls inside cfg_bind's geometry phase: the index buffer (or the quad substitute),
    // the vertex buffers, transform feedback, and the indirect draw buffers when there is one.
    // They nest inside cfg_bind's geometry, so unattributed is the clock and the two pointer
    // stores around it - not the SynchronizeBuffer calls, which are inside the phases themselves.
    std::printf("EDEN_DEV_GEO index_ns=%llu vertex_ns=%llu xf_ns=%llu indirect_ns=%llu "
                "unattributed_ns=%llu index_calls=%llu vertex_calls=%llu xf_calls=%llu "
                "indirect_calls=%llu\n",
                load(geo_index, false), load(geo_vertex, false), load(geo_xf, false),
                load(geo_indirect, false),
                load(bind_geometry, false) - (load(geo_index, false) + load(geo_vertex, false) +
                                              load(geo_xf, false) + load(geo_indirect, false)),
                load(geo_index, true), load(geo_vertex, true), load(geo_xf, true),
                load(geo_indirect, true));
    // The three inside cfg_bind's update phase: index, vertex and transform-feedback updates; the
    // six-stage uniform, storage and texture loop; and the indirect update when there is one.
    std::printf("EDEN_DEV_UPD state_ns=%llu stage_ns=%llu indirect_ns=%llu unattributed_ns=%llu "
                "state_calls=%llu stage_calls=%llu indirect_calls=%llu\n",
                load(upd_state, false), load(upd_stage, false), load(upd_indirect, false),
                load(bind_update, false) - (load(upd_state, false) + load(upd_stage, false) +
                                            load(upd_indirect, false)),
                load(upd_state, true), load(upd_stage, true), load(upd_indirect, true));
    // Texture-cache GC GPU drains since the last report. The fix these counters watch is the
    // prefetch lookahead (vulkan_gc_downloads.inc): batched drains should cover many images and
    // unbatched ones should be rare, as each unbatched drain is a full pipeline wait on the GPU
    // thread for one eviction.
    std::printf("EDEN_DEV_GC_DRAIN batched_calls=%llu batched_ns=%llu batched_images=%llu "
                "unbatched_calls=%llu unbatched_ns=%llu\n",
                load(gc_batched_drain, true), load(gc_batched_drain, false),
                gc_batched_drain.requested_bytes.load(std::memory_order_relaxed),
                load(gc_unbatched_drain, true), load(gc_unbatched_drain, false));
    std::printf("EDEN_DEV_GUEST cpu_write_calls=%llu cpu_write_ns=%llu cpu_read_calls=%llu cpu_read_ns=%llu "
                "sync_calls=%llu sync_ns=%llu dequeue_calls=%llu dequeue_ns=%llu",
                load(guest_cpu_write, true), load(guest_cpu_write, false),
                load(guest_cpu_read, true), load(guest_cpu_read, false),
                load(guest_sync_wait, true), load(guest_sync_wait, false),
                load(guest_dequeue_wait, true), load(guest_dequeue_wait, false));
    std::printf(" ipc_calls=%llu ipc_ns=%llu", load(guest_ipc_wait, true), load(guest_ipc_wait, false));
    std::printf(" cache_lock_contended=%llu cache_lock_blocked=%llu",
                cache_lock_contended.load(std::memory_order_relaxed), cache_lock_blocked.load(std::memory_order_relaxed));
    std::printf(" fs_file_calls=%llu fs_file_ns=%llu fs_file_bytes=%llu fs_storage_calls=%llu fs_storage_ns=%llu "
                "fs_storage_bytes=%llu",
                load(guest_fs_file, true), load(guest_fs_file, false),
                guest_fs_file_bytes.load(std::memory_order_relaxed),
                load(guest_fs_storage, true), load(guest_fs_storage, false),
                guest_fs_storage_bytes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < jit_callbacks.size(); ++core)
        std::printf(" r%u=%llu w%u=%llu x%u=%llu", core, jit_callbacks[core].reads.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].writes.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].exclusive_writes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < 3; ++core)
        std::printf(" idle%u=%llu/%llu/%llu", core, guest_idle[core].calls.load(std::memory_order_relaxed),
                    guest_idle[core].nanoseconds.load(std::memory_order_relaxed) / 1000000,
                    guest_idle[core].sleeps.load(std::memory_order_relaxed));
    std::printf(" cond=");
    for (unsigned slot = 0; slot < render_conditions.size(); ++slot)
        std::printf("%s%llu", slot ? "," : "", render_conditions[slot].load(std::memory_order_relaxed));
    std::printf("\n");
    for (unsigned slot = 0; slot < ipc_threads.size(); ++slot) {
        const auto calls = ipc_threads[slot].calls.load(std::memory_order_relaxed);
        if (calls == 0) continue;
        std::printf("EDEN_DEV_IPC_THREAD slot=%u tid=%llu name=%.23s calls=%llu ns=%llu\n", slot,
                    ipc_threads[slot].tid.load(std::memory_order_relaxed), ipc_threads[slot].name, calls,
                    ipc_threads[slot].ns.load(std::memory_order_relaxed));
    }
    for (unsigned slot = 0; slot < wait_threads.size(); ++slot) {
        const auto calls = wait_threads[slot].calls.load(std::memory_order_relaxed);
        if (calls == 0) continue;
        const auto tid = wait_threads[slot].tid.load(std::memory_order_relaxed);
        const char* name = "";
        for (unsigned islot = 0; islot < ipc_thread_slots; ++islot)
            if (ipc_threads[islot].tid.load(std::memory_order_relaxed) == tid) {
                name = ipc_threads[islot].name;
                break;
            }
        // rsn= is this slot's own wait_ns per ThreadWaitReasonForDebugging, same index order as
        // the histogram line, so a thread's split reads without joining anything by hand.
        std::printf("EDEN_DEV_WAIT_THREAD tid=%llu name=%.23s calls=%llu ns=%llu waker=%llu rsn=", tid,
                    name, calls, wait_threads[slot].ns.load(std::memory_order_relaxed),
                    wait_threads[slot].waker.load(std::memory_order_relaxed));
        for (unsigned reason = 0; reason < wait_reason_slots; ++reason)
            std::printf("%s%llu", reason ? "," : "",
                        wait_threads[slot].reason_ns[reason].load(std::memory_order_relaxed));
        std::printf("\n");
    }
    // tid is the slot index here (performance.h indexes the dispatch table directly by tid).
    for (unsigned tid = 0; tid < dispatch_slots; ++tid) {
        const auto dcalls = dispatch_calls[tid].load(std::memory_order_relaxed);
        if (dcalls == 0) continue;
        std::printf("EDEN_DEV_DISPATCH tid=%u calls=%llu ns=%llu\n", tid, dcalls,
                    dispatch_ns[tid].load(std::memory_order_relaxed));
    }
    std::printf("EDEN_DEV_DISPATCH overflow_calls=%llu\n",
                dispatch_overflow_calls.load(std::memory_order_relaxed));
    // Reason indices are ThreadWaitReasonForDebugging (k_thread.h); named here so a log reads
    // without the source.
    std::printf("EDEN_DEV_WAIT reasons=none,sleep,ipc,sync,condvar,arbitration,suspended,other "
                "overflow_calls=%llu calls=",
                wait_overflow_calls.load(std::memory_order_relaxed));
    for (unsigned reason = 0; reason < wait_reason_slots; ++reason)
        std::printf("%s%llu", reason ? "," : "", wait_reason_calls[reason].load(std::memory_order_relaxed));
    std::printf(" ns=");
    for (unsigned reason = 0; reason < wait_reason_slots; ++reason)
        std::printf("%s%llu", reason ? "," : "", wait_reason_ns[reason].load(std::memory_order_relaxed));
    std::printf("\n");
    std::printf("EDEN_DEV_IPCHIST lt50us=%llu lt500us=%llu lt5ms=%llu ge5ms=%llu overflow_calls=%llu "
                "overflow_ns=%llu\n",
                ipc_hist[0].load(std::memory_order_relaxed), ipc_hist[1].load(std::memory_order_relaxed),
                ipc_hist[2].load(std::memory_order_relaxed), ipc_hist[3].load(std::memory_order_relaxed),
                load(ipc_thread_overflow, true), load(ipc_thread_overflow, false));
    const auto window = Eden::Fastmem::WindowStats();
    std::printf("EDEN_FASTMEM window=%llx pages=%llu chunks=%llu direct_reads=%llu direct_writes=%llu "
                "faults=%llu maps=%llu unmaps=%llu protects=%llu kernel_calls=%llu kernel_ns=%llu failures=%llu\n",
                static_cast<unsigned long long>(window.window), static_cast<unsigned long long>(window.mapped_pages),
                static_cast<unsigned long long>(window.aliased_chunks),
                static_cast<unsigned long long>(window.direct_reads),
                static_cast<unsigned long long>(window.direct_writes),
                static_cast<unsigned long long>(Eden::Fastmem::Faults()),
                static_cast<unsigned long long>(window.map_calls), static_cast<unsigned long long>(window.unmap_calls),
                static_cast<unsigned long long>(window.protect_calls),
                static_cast<unsigned long long>(window.kernel_calls), static_cast<unsigned long long>(window.kernel_ns),
                static_cast<unsigned long long>(window.failures));
    // Guest cores record their owner CPU clocks at their next JIT exit.
    Snapshot();
}

void RegisterWorker(const char* name) {
    Eden::Crash::NameThread(name);  // a crash report names the thread
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
    Eden::Stall::NoteThread(name);
#endif
    for (unsigned i = 0; i < names.size(); ++i) {
        if (std::strcmp(name, names[i])) continue;
        Worker worker;
        worker.thread = pthread_self();
#ifdef EDEN_DEV_PROFILE
        if (pc_sampling && (i == 4 || i == pc_sample_core.load())) {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGUSR2);
            if (pthread_sigmask(SIG_UNBLOCK, &mask, nullptr))
                throw std::runtime_error("Cannot enable development PC sampler");
            if (i == pc_sample_core.load()) {
                core_sample_thread = pthread_self();
                core_sample_ready.store(true, std::memory_order_release);
                // dev-settings pc_fast=on: ~500 Hz from 45 s after registration instead of the
                // frontend's 20 Hz polls (from 90 s), for compilation bursts at load. Signals during
                // the boot's first seconds hung it (run radv-zb5-20260930-015220).
                if (pc_fast.load()) {
                    std::thread([] {
                        std::this_thread::sleep_for(std::chrono::seconds(45));
                        while (core_pc_count.load() < sampled_core_pcs.size() &&
                               pthread_kill(core_sample_thread, SIGUSR2) == 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }).detach();
                }
            }
        }
#endif
        worker.clock_error = CurrentCpuClock(&worker.clock);
        worker.wall_ns = Common::g_wall_clock.GetTimeNS().count();
        worker.mono_ns = ClockNs(CLOCK_MONOTONIC);
#ifdef PS5_NATIVE
        cpuset_t affinity{};
        const int result = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
                                             sizeof(unsigned long long), &affinity);
#else
        cpu_set_t affinity{};
        const int result = sched_getaffinity(0, sizeof(affinity), &affinity);
#endif
#ifdef PS5_NATIVE
        if (result == 0) PinWorker(i, affinity);
#endif
        worker.affinity_error = result == 0 ? 0 : errno;
        if (!worker.affinity_error) {
            for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (!CPU_ISSET(cpu, &affinity)) continue;
                ++worker.allowed;
                if (cpu < 64) worker.mask |= 1ULL << cpu;
            }
        }
        worker.registered = true;
        std::lock_guard lock(workers_mutex);
        workers[i] = worker;
        return;
    }
#ifdef PS5_NATIVE
    PlaceSecondary(name);
#endif
}

void SampleCpu(unsigned core, unsigned long long thread, unsigned long long pc, unsigned svc, unsigned fpcr) {
    const auto epoch = sample_epoch.load(std::memory_order_relaxed);
    if (sampled_epoch.at(core) == epoch) return;
    sampled_epoch[core] = epoch;
    const CpuSample sample{epoch, ClockNs(CLOCK_MONOTONIC),
        owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -ENOTSUP, thread, pc, svc, fpcr,
        compilation[core].calls.load(std::memory_order_relaxed),
        compilation[core].nanoseconds.load(std::memory_order_relaxed)};
    std::lock_guard lock(workers_mutex);
    cpu_samples[core] = sample;
}

void Snapshot() {
    // Request a fresh owner-written sample; report the last completed sample with
    // its own timestamp. An idle core may remain stale; never infer zero CPU use.
    sample_epoch.fetch_add(1, std::memory_order_relaxed);
    const auto mono = ClockNs(CLOCK_MONOTONIC);
    std::printf("EDEN_PERF_SAMPLE mono_ns=%lld process_cpu_ns=%lld wall_ns=%lld\n",
                mono, cpu_clocks_valid ? ClockNs(process_clock) : -static_cast<long long>(ENOTSUP),
                static_cast<long long>(Common::g_wall_clock.GetTimeNS().count()));
    std::lock_guard lock(workers_mutex);
#ifdef EDEN_DEV_PROFILE
    std::map<uintptr_t, unsigned> counts;
    const unsigned end = pc_count.load(std::memory_order_acquire);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::map<std::array<uintptr_t, 9>, unsigned> caller_counts;
#endif
    while (pc_reported < end) {
        const unsigned index = pc_reported++;
        ++counts[sampled_pcs[index]];
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
        std::array<uintptr_t, 9> key{sampled_pcs[index]};
        std::copy(sampled_callers[index].begin(), sampled_callers[index].end(), key.begin() + 1);
        ++caller_counts[key];
#endif
    }
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    for (const auto& [chain, count] : caller_counts) {
        std::printf("EDEN_PERF_NATIVE_CALLERS mono_ns=%lld pc=%llx count=%u callers=", mono,
                    static_cast<unsigned long long>(chain[0]), count);
        for (unsigned i = 1; i < chain.size(); ++i)
            std::printf("%s%llx", i == 1 ? "" : ",", static_cast<unsigned long long>(chain[i]));
        std::printf("\n");
    }
#endif
    for (const auto& [pc, count] : counts)
        std::printf("EDEN_PERF_NATIVE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
    std::map<uintptr_t, unsigned> core_counts;
    const unsigned core_end = core_pc_count.load(std::memory_order_acquire);
    while (core_pc_reported < core_end) ++core_counts[sampled_core_pcs[core_pc_reported++]];
    for (const auto& [pc, count] : core_counts)
        std::printf("EDEN_PERF_CORE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::map<std::array<unsigned long long, 9>, unsigned> core_caller_counts;
    const unsigned core_caller_end = core_caller_count.load(std::memory_order_acquire);
    while (core_caller_reported < core_caller_end) {
        const unsigned index = core_caller_reported++;
        std::array<unsigned long long, 9> key{static_cast<unsigned long long>(core_caller_pcs[index])};
        for (unsigned i = 0; i < 8; ++i)
            key[i + 1] = static_cast<unsigned long long>(sampled_core_callers[index][i]);
        ++core_caller_counts[key];
    }
    for (const auto& [chain, count] : core_caller_counts) {
        std::printf("EDEN_PERF_CORE_CALLERS mono_ns=%lld pc=%llx count=%u callers=", mono,
                    chain[0], count);
        for (unsigned i = 1; i < chain.size(); ++i)
            std::printf("%s%llx", i == 1 ? "" : ",", chain[i]);
        std::printf("\n");
    }
#endif
    // The same samples by guest block: location (A32 PC in the low word), block offset, count.
    if (const std::size_t blocks = core0_blocks ? core0_block_count.load(std::memory_order_acquire) : 0) {
        std::map<std::pair<unsigned long long, unsigned long long>, unsigned> guest_counts;
        for (const auto& [pc, count] : core_counts) {
            const JitBlock* begin = core0_blocks;
            const JitBlock* it = std::upper_bound(begin, begin + blocks, pc,
                                                  [](uintptr_t value, const JitBlock& block) { return value < block.entry; });
            if (it == begin) continue;
            --it;
            if (pc - it->entry < it->size) guest_counts[{it->location, pc - it->entry}] += count;
        }
        for (const auto& [key, count] : guest_counts)
            std::printf("EDEN_PERF_GUEST_PC mono_ns=%lld location=%llx host_offset=%llx count=%u\n", mono,
                        key.first, key.second, count);
        // P1: guest and host code of this window's six hottest blocks not dumped before.
        std::map<unsigned long long, unsigned> block_counts;
        for (const auto& [key, count] : guest_counts) block_counts[key.first] += count;
        std::vector<std::pair<unsigned, unsigned long long>> ranked;
        for (const auto& [location, count] : block_counts) ranked.emplace_back(count, location);
        std::sort(ranked.rbegin(), ranked.rend());
        static std::set<unsigned long long> dumped;
        unsigned printed = 0;
        for (const auto& [count, location] : ranked) {
            if (printed == 6) break;
            if (!dumped.insert(location).second) continue;
            ++printed;
            const JitBlock* block = nullptr;
            for (std::size_t i = blocks; i-- > 0;)
                if (core0_blocks[i].location == location) { block = &core0_blocks[i]; break; }
            std::string guest;
            const unsigned long long pc = location & 0xffffffffull;
            for (unsigned long long at = pc; at < pc + 256 && guest_read32; at += 4) {
                unsigned word = 0;
                if (!guest_read32(at, word)) break;
                char text[12];
                std::snprintf(text, sizeof(text), "%08x", word);
                guest += text;
            }
            std::printf("EDEN_PERF_BLOCK_GUEST location=%llx count=%u words=%s\n", location, count, guest.c_str());
            if (block) {
                std::string host;
                const auto* bytes = reinterpret_cast<const unsigned char*>(block->entry);
                for (unsigned long long i = 0; i < block->size && i < 4096; ++i) {
                    char text[4];
                    std::snprintf(text, sizeof(text), "%02x", bytes[i]);
                    host += text;
                }
                std::printf("EDEN_PERF_BLOCK_HOST location=%llx entry=%llx size=%llu bytes=%s\n", location,
                            static_cast<unsigned long long>(block->entry), block->size, host.c_str());
            }
        }
    }
#endif
    for (unsigned i = 0; i < workers.size(); ++i) {
        const auto& worker = workers[i];
        sched_param priority{};
        int policy = -1;
        const int priority_error = worker.registered ?
            pthread_getschedparam(worker.thread, &policy, &priority) : ENOENT;
        const auto cpu = !cpu_clocks_valid ? -static_cast<long long>(ENOTSUP) :
                        worker.clock_error ? -static_cast<long long>(worker.clock_error) :
                                             ClockNs(worker.clock);
        std::printf("EDEN_PERF_WORKER mono_ns=%lld name=%s cpu_ns=%lld clock_id=%d affinity_error=%d "
                    "allowed=%d mask_low64=%llx priority_error=%d policy=%d priority=%d "
                    "registered_wall_ns=%lld registered_mono_ns=%lld\n",
                    mono, names[i], cpu, int(worker.clock), worker.affinity_error, worker.allowed, worker.mask,
                    priority_error, policy, priority.sched_priority, worker.wall_ns, worker.mono_ns);
        if (i < compilation.size()) {
            const auto& sample = cpu_samples[i];
            std::printf("EDEN_PERF_CPU_POINT core=%u phase=%u epoch=%u mono_ns=%lld cpu_ns=%lld thread=%llu pc=%llx svc=%x fpcr=%u compilations=%llu compile_ns=%llu\n",
                        i, unsigned(cpu_state[i].phase.load(std::memory_order_relaxed)), sample.epoch,
                        sample.mono_ns, sample.cpu_ns, sample.thread, sample.pc, sample.svc, sample.fpcr,
                        sample.compilations, sample.compile_ns);
#ifdef EDEN_DEV_PROFILE
            const auto& dups = jit_duplicates[i];
            std::printf("EDEN_PERF_DUPLICATES mono_ns=%lld core=%u first=%llu again=%llu lead_1ms=%llu lead_10ms=%llu "
                        "lead_100ms=%llu lead_1s=%llu lead_10s=%llu lead_more=%llu\n", mono, i,
                        dups[0].load(std::memory_order_relaxed), dups[1].load(std::memory_order_relaxed),
                        dups[2].load(std::memory_order_relaxed), dups[3].load(std::memory_order_relaxed),
                        dups[4].load(std::memory_order_relaxed), dups[5].load(std::memory_order_relaxed),
                        dups[6].load(std::memory_order_relaxed), dups[7].load(std::memory_order_relaxed));
            if (eden_jit_path_counters) {
                unsigned long long path[8]{};
                eden_jit_path_counters(i, path);
                std::printf("EDEN_PERF_JITPATH mono_ns=%lld core=%u runs=%llu thunks=%llu lookups=%llu hits=%llu "
                            "contended=%llu flush_all=%llu flush_locations=%llu far_links=%llu\n", mono, i,
                            path[0], path[1], path[2], path[3], path[4], path[5], path[6], path[7]);
            }
            std::printf("EDEN_PERF_PROGRESS mono_ns=%lld core=%u compilations=%llu compile_ns=%llu evacuations=%llu "
                        "translate_ns=%llu optimize_ns=%llu emit_ns=%llu ranges_ns=%llu\n",
                        mono, i, compilation[i].calls.load(std::memory_order_relaxed),
                        compilation[i].nanoseconds.load(std::memory_order_relaxed),
                        evacuations[i].load(std::memory_order_relaxed),
                        jit_phase_ns[i][0].load(std::memory_order_relaxed), jit_phase_ns[i][1].load(std::memory_order_relaxed),
                        jit_phase_ns[i][2].load(std::memory_order_relaxed), jit_phase_ns[i][3].load(std::memory_order_relaxed));
#else
            std::printf("EDEN_PERF_PROGRESS mono_ns=%lld core=%u compilations=%llu compile_ns=%llu evacuations=%llu\n",
                        mono, i, compilation[i].calls.load(std::memory_order_relaxed),
                        compilation[i].nanoseconds.load(std::memory_order_relaxed),
                        evacuations[i].load(std::memory_order_relaxed));
#endif
        }
    }
    std::fflush(stdout);
}

#ifndef EDEN_DEV_PROFILE
// Release builds: the JIT's block hook (derived a32_interface.cpp) resolves here and does nothing,
// as does the shared JIT's compile-phase hook (headless/dynarmic/jit_impl.inc). Native packages
// cannot import an undefined weak symbol.
extern "C" void eden_jit_block(unsigned, unsigned long long, const void*, unsigned long long) {}
extern "C" void eden_jit_phases(unsigned, unsigned long long, unsigned long long, unsigned long long,
                                unsigned long long) {}
#endif

#ifdef EDEN_DEV_PROFILE
extern "C" void eden_jit_block(unsigned core, unsigned long long location, const void* entry,
                               unsigned long long size) {
    if (core != 0) return;
    if (!core0_blocks) core0_blocks = new JitBlock[jit_block_capacity];
    std::size_t count = core0_block_count.load(std::memory_order_relaxed);
    const auto address = reinterpret_cast<uintptr_t>(entry);
    if (count && address < core0_blocks[count - 1].entry) count = 0; // the cache was cleared
    if (count >= jit_block_capacity) return;
    core0_blocks[count] = JitBlock{address, size, location};
    core0_block_count.store(count + 1, std::memory_order_release);
}

void BeginPcSampling() {
    struct sigaction previous{}, action{};
    if (sigaction(SIGUSR2, nullptr, &previous) || previous.sa_handler != SIG_DFL)
        throw std::runtime_error("Development sampler signal already in use");
    action.sa_sigaction = PcSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, nullptr))
        throw std::runtime_error("Cannot install development PC sampler");
    // Development process lifetime; polling stops before guest-worker shutdown.
    pc_sampling = true;
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::printf("EDEN_PERF_CALLER_MODE enabled=1 firmware=6.02 depth=8 page=4096\n");
#endif
    std::printf("EDEN_PERF_PC_ANCHOR address=%llx capacity=%zu\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&PcSignal)), sampled_pcs.size());
    std::printf("EDEN_PERF_LIBC memcpy=%p memset=%p memmove=%p memcmp=%p\n",
                reinterpret_cast<void*>(&std::memcpy), reinterpret_cast<void*>(&std::memset),
                reinterpret_cast<void*>(&std::memmove), reinterpret_cast<void*>(&std::memcmp));
    // Anchors for a sampled PC that falls in a library we hold no symbols for: these bracket
    // the wait primitives the guest cores and the render thread block in.
    std::printf("EDEN_PERF_LIBC2 mutex_lock=%p mutex_unlock=%p cond_wait=%p cond_timedwait=%p "
                "cond_signal=%p cond_broadcast=%p nanosleep=%p clock_gettime=%p sched_yield=%p\n",
                reinterpret_cast<void*>(&pthread_mutex_lock), reinterpret_cast<void*>(&pthread_mutex_unlock),
                reinterpret_cast<void*>(&pthread_cond_wait), reinterpret_cast<void*>(&pthread_cond_timedwait),
                reinterpret_cast<void*>(&pthread_cond_signal), reinterpret_cast<void*>(&pthread_cond_broadcast),
                reinterpret_cast<void*>(&nanosleep), reinterpret_cast<void*>(&clock_gettime),
                reinterpret_cast<void*>(&sched_yield));
}

void PollGpuPc() {
    std::lock_guard lock(workers_mutex);
    if (pc_sampling && workers[4].registered && pc_count.load() < sampled_pcs.size()) {
        const int error = pthread_kill(workers[4].thread, SIGUSR2);
        if (error) throw std::runtime_error("Cannot sample development render thread");
    }
    if (pc_sampling && core_sample_ready.load(std::memory_order_acquire) &&
        core_pc_count.load() < sampled_core_pcs.size())
        pthread_kill(core_sample_thread, SIGUSR2);
}
#endif

extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));

void AllocatorCheck() {
    std::printf("EDEN_PERF_HEAP arenas=%u\n", eden_heap_arenas_created ? eden_heap_arenas_created() : 1u);
    constexpr unsigned ops = 50000, live_slots = 1024;
    const auto run = [](unsigned seed) {
        std::vector<void*> live(live_slots, nullptr);
        unsigned x = seed;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < ops; ++i) {
            x = x * 1664525u + 1013904223u;
            void*& slot = live[x % live_slots];
            std::free(slot);
            slot = std::malloc(16 + ((x >> 16) & 511));
        }
        const auto end = std::chrono::steady_clock::now();
        for (void* pointer : live) std::free(pointer);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    };
    std::printf("EDEN_PERF_ALLOC threads=1 pairs=%u ns_per_pair=%.1f\n", ops, run(1) / double(ops));
    for (unsigned threads : {2u, 4u, 8u}) {
        std::vector<long long> elapsed(threads);
        std::latch start{static_cast<std::ptrdiff_t>(threads)};
        std::vector<std::thread> pool;
        for (unsigned index = 0; index < threads; ++index)
            pool.emplace_back([&, index] { start.arrive_and_wait(); elapsed[index] = run(index + 7); });
        for (auto& thread : pool) thread.join();
        long long worst = 0, sum = 0;
        for (auto value : elapsed) { worst = std::max(worst, value); sum += value; }
        std::printf("EDEN_PERF_ALLOC threads=%u pairs=%u ns_per_pair_mean=%.1f ns_per_pair_worst=%.1f\n",
                    threads, ops, sum / double(threads) / ops, worst / double(ops));
    }
}

void PlatformChecks() {
#ifdef PS5_NATIVE
    CheckWorkerTopology();
#endif
    CheckCpuClocks();
#ifdef EDEN_DEV_PROFILE
    AllocatorCheck();
#endif
    using clock = std::chrono::steady_clock;
    const auto ns = [](auto start) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    };
    std::printf("EDEN_PERF_CLOCK leaf_max=%x invariant=%d tsc_hz=%llu native=%d\n",
                __get_cpuid_max(0, nullptr), Common::g_cpu_caps.invariant_tsc,
                static_cast<unsigned long long>(Common::g_cpu_caps.tsc_frequency),
                Common::g_wall_clock.IsNative());
    unsigned long long sum = 0;
    auto start = clock::now();
    for (unsigned i = 0; i < 100'000; ++i) sum += Common::g_wall_clock.GetCNTPCT();
    std::printf("EDEN_PERF_CLOCK_READ calls=100000 elapsed_ns=%lld checksum=%llu\n",
                static_cast<long long>(ns(start)), sum);
    // Bound the diagnostic overhead comparison; per-block timers use the
    // already-qualified fenced counter, with the upstream steady-clock fallback.
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += clock::now().time_since_epoch().count();
    const auto system_clock_ns = ns(start);
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += Common::g_wall_clock.GetTimeNS().count();
    std::printf("EDEN_PERF_DIAGNOSTIC_CLOCK calls=1000 system_ns=%lld counter_ns=%lld checksum=%llu\n",
                static_cast<long long>(system_clock_ns), static_cast<long long>(ns(start)), sum);
    for (const unsigned requested_us : {1000u, 16667u, 50000u}) {
        const auto wall = Common::g_wall_clock.GetTimeNS();
        start = clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(requested_us));
        const auto mono_delta = ns(start);
        const auto wall_delta = (Common::g_wall_clock.GetTimeNS() - wall).count();
        if (mono_delta <= 0 || wall_delta <= 0) throw std::runtime_error("Nonmonotonic clock");
        if (std::abs(wall_delta - mono_delta) > std::max<decltype(mono_delta)>(2'000'000, mono_delta / 20))
            throw std::runtime_error("Guest clock disagrees with monotonic time");
        std::printf("EDEN_PERF_SLEEP requested_us=%u mono_ns=%lld wall_ns=%lld\n",
                    requested_us, static_cast<long long>(mono_delta), static_cast<long long>(wall_delta));
    }
    constexpr std::size_t bytes = 16 * 1024 * 1024;
    auto* memory = static_cast<unsigned long long*>(Common::AllocateMemoryPages(bytes));
    if (!memory) throw std::bad_alloc{};
    struct Release {
        void* memory;
        ~Release() { Common::FreeMemoryPages(memory, bytes); }
    } release{memory};
    constexpr std::size_t words = bytes / sizeof(*memory);
    for (std::size_t i = 0; i < words; ++i) memory[i] = i;
    const volatile auto* reads = memory;
    sum = 0;
    start = clock::now();
    for (unsigned repeat = 0; repeat < 4; ++repeat)
        for (std::size_t i = 0; i < words; ++i) sum += reads[i];
    if (sum != 4ULL * words * (words - 1) / 2) throw std::runtime_error("Memory read checksum");
    std::printf("EDEN_PERF_MEMORY pattern=stream bytes=%zu elapsed_ns=%lld checksum=%llu\n",
                bytes * 4, static_cast<long long>(ns(start)), sum);
    // Full-cycle LCG, one node per cache line. Compare a small warm working set
    // with the same dependent-load loop across the larger owned mapping.
    for (const std::size_t nodes : {std::size_t{512}, bytes / 64}) {
        for (std::size_t i = 0; i < nodes; ++i) memory[i * 8] = (i * 5 + 1) & (nodes - 1);
        std::size_t index = 0;
        constexpr unsigned steps = 1'048'576;
        start = clock::now();
        for (unsigned i = 0; i < steps; ++i) index = reads[index * 8];
        if (index != 0) throw std::runtime_error("Memory chain checksum");
        std::printf("EDEN_PERF_MEMORY pattern=dependent working_bytes=%zu steps=%u elapsed_ns=%lld checksum=%zu\n",
                    nodes * 64, steps, static_cast<long long>(ns(start)), index);
    }
    std::fflush(stdout);
}
} // namespace Eden::Performance

// dynarmic A64 hooks (weak there): whole-cache evacuations and block compilation time.
extern "C" void eden_jit_evacuation(unsigned core) {
    if (core < Eden::Performance::evacuations.size())
        Eden::Performance::evacuations[core].fetch_add(1, std::memory_order_relaxed);
}
#ifdef EDEN_DEV_PROFILE
// Development builds only (the per-phase and duplicate counters exist there).
extern "C" void eden_jit_phases(unsigned core, unsigned long long translate, unsigned long long optimize,
                                unsigned long long emit, unsigned long long location) {
    if (core >= Eden::Performance::jit_phase_ns.size()) return;
    auto& phases = Eden::Performance::jit_phase_ns[core];
    phases[0].fetch_add(translate, std::memory_order_relaxed);
    phases[1].fetch_add(optimize, std::memory_order_relaxed);
    phases[2].fetch_add(emit, std::memory_order_relaxed);
    if (!Eden::Performance::jit_duplicate_tracking.load(std::memory_order_relaxed)) return;
    // Which cores compiled each location, and when the first one did.
    struct First { unsigned cores; long long ns; };
    static std::mutex mutex;
    static auto* compiled = [] {
        auto* map = new std::unordered_map<unsigned long long, First>;
        map->reserve(1u << 21);
        return map;
    }();
    const long long now = Common::g_wall_clock.GetTimeNS().count();
    unsigned index = 0;
    {
        std::lock_guard lock{mutex};
        auto [it, inserted] = compiled->try_emplace(location, First{1u << core, now});
        if (!inserted) {
            if (it->second.cores & (1u << core)) {
                index = 1;
            } else {
                it->second.cores |= 1u << core;
                const long long lead = now - it->second.ns;
                index = 2;
                for (long long bound = 1'000'000; index < 7 && lead >= bound; bound *= 10) ++index;
            }
        }
    }
    Eden::Performance::jit_duplicates[core][index].fetch_add(1, std::memory_order_relaxed);
}
extern "C" void eden_jit_ranges(unsigned core, unsigned long long ns) {
    if (core < Eden::Performance::jit_phase_ns.size())
        Eden::Performance::jit_phase_ns[core][3].fetch_add(ns, std::memory_order_relaxed);
}
#endif
// The shared JIT's compile timers (headless/dynarmic/jit_impl.inc): the qualified invariant-TSC
// clock, about 26 ns per read against about 0.9 µs for the steady clock's system call.
extern "C" unsigned long long eden_jit_clock_ns() {
    return static_cast<unsigned long long>(Common::g_wall_clock.GetTimeNS().count());
}
extern "C" void eden_jit_compile(unsigned core, unsigned long long ns) {
    if (core >= Eden::Performance::compilation.size()) return;
    Eden::Performance::compilation[core].calls.fetch_add(1, std::memory_order_relaxed);
    Eden::Performance::compilation[core].nanoseconds.fetch_add(ns, std::memory_order_relaxed);
}
