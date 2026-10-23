#!/usr/bin/env python3
"""Exercise actual diagnostics, live thread clocks and unavailable-clock reporting."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
assert not re.search(r'\b(?:__syscall|syscall)\s*\(', (root / 'headless/performance.cpp').read_text()), 'Native titles reject direct syscalls; use imported APIs'
cache = Path((root / '.local/headless-cache').read_text().strip())
source = cache / 'source/src'
original = (source / 'common/thread.cpp').read_text()
derived = (cache / 'native-local/headless/thread.cpp').read_text()
# The include names the port folder as the build was configured (a symlinked checkout differs).
include, derived = derived.split('\n', 1)
assert include.startswith('#include "') and Path(include[10:-1]).resolve() == root / 'headless/performance.h', include
assert derived == original.replace(
    'void SetCurrentThreadName(const char* name) {',
    'void SetCurrentThreadName(const char* name) {\n    ::Eden::Performance::RegisterWorker(name);')
main = (root / 'headless/main.cpp').read_text()
assert main.index('Performance::PlatformChecks()') < main.index('Common::Log::Initialize()')
# Periodic sampling is allowed only in the explicit development profile branch.
development_wait = main.split('#ifdef EDEN_DEV_PROFILE\n                        for ', 1)[1].split('#elif defined(EDEN_DEV_ROM_ID)', 1)[0]
assert main.count('Performance::Snapshot()') == development_wait.count('Performance::Snapshot()') == 1
assert 'completion->wake.wait(lock,' in main
assert 'completion->wake.wait(lock, completed);' in main
assert 'completion->return_to_menu' in main
code = r'''
#include "performance.h"
#include "fastmem.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <semaphore>
#include <thread>
#include <time.h>
static bool fail_clock;
static bool wall_as_cpu;
extern "C" int __real_clock_gettime(clockid_t, timespec*);
extern "C" int __wrap_clock_gettime(clockid_t id, timespec* out) {
    if (fail_clock && id != CLOCK_MONOTONIC && id != CLOCK_REALTIME) {
        errno = EINVAL; return -1;
    }
    if (wall_as_cpu && id == CLOCK_THREAD_CPUTIME_ID) id = CLOCK_MONOTONIC;
    return __real_clock_gettime(id, out);
}
// Link-time providers that performance.cpp reports from; not exercised here.
extern "C" void ps5_opengl_heap_snapshot(const char*, unsigned) {}
namespace Eden::Fastmem {
Stats WindowStats() noexcept { return {}; }
std::uint64_t Faults() noexcept { return 0; }
std::uint64_t Demotions() noexcept { return 0; }
}
int main() {
    using namespace Eden::Performance;
    PlatformChecks();
    // A Timer counts only while timers are on (development builds and cost runs).
    Totals untimed;
    for (unsigned i = 0; i < 1000; ++i) { Timer timer(untimed, 16); }
    assert(untimed.calls == 0 && untimed.requested_bytes == 0 && untimed.nanoseconds == 0);
    timers_on = true;
    Totals timed;
    for (unsigned i = 0; i < 1000; ++i) { Timer timer(timed, 16); }
    assert(timed.calls == 1000 && timed.requested_bytes == 16000 && timed.nanoseconds > 0);
    Totals slept;
    { Timer timer(slept); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    assert(slept.calls == 1 && slept.nanoseconds >= 15000000 && slept.nanoseconds < 500000000);
    RegisterWorker("irrelevant");
    RegisterWorker("CPUCore_0");
    SampleCpu(0, 123, 0xabcd, 0x18, 0x3000000);
    SampleCpu(0, 999, 0xffff, 0, 1); // Same epoch must not publish a second sample.
    std::binary_semaphore ready{0}, release{0};
    std::jthread worker([&] {
        RegisterWorker("CPUCore_1");
        { Timer timer(compilation[1]);
          const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
          while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        SampleCpu(1, 456, 0x1234, ~0u, 0);
        ready.release(); release.acquire();
    });
    ready.acquire();
    Snapshot();
    fail_clock = true;
    SampleCpu(0, 123, 0xabce, ~0u, 0);
    Snapshot();
    fail_clock = false;
    SampleCpu(0, 123, 0xabcf, ~0u, 0);
    Snapshot();
    wall_as_cpu = true;
    PlatformChecks(); // A wall clock is not a usable owner CPU clock.
    SampleCpu(0, 123, 0xabd0, ~0u, 0);
    Snapshot();
    wall_as_cpu = false;
    fail_clock = true;
    PlatformChecks(); // An API returning unusable clocks must disable CPU metrics.
    SampleCpu(0, 123, 0xabd1, ~0u, 0);
    Snapshot();
    release.release();
    worker.join(); // No sampling after shutdown or join.
}
'''
# The overlay's per-core load leaves the core's idle polling out (GuestCoreLoads): a core that
# burns 300 ms of CPU, 225 ms of it polling while idle, reads about 75 ms of work, not 300.
loads_code = r'''
#include "performance.h"
#include "fastmem.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
extern "C" void ps5_opengl_heap_snapshot(const char*, unsigned) {}
namespace Eden::Fastmem {
Stats WindowStats() noexcept { return {}; }
std::uint64_t Faults() noexcept { return 0; }
std::uint64_t Demotions() noexcept { return 0; }
}
static void Burn(std::chrono::milliseconds length) {
    const auto end = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
}
int main() {
    using namespace Eden::Performance;
    using namespace std::chrono_literals;
    PlatformChecks();
    RegisterWorker("CPUCore_2");
    unsigned loads[4];
    GuestCoreLoads(loads);                 // asks for readings
    SampleCoreCpu(2);                      // the first one
    std::this_thread::sleep_for(260ms);
    GuestCoreLoads(loads);                 // takes it as the start
    Burn(300ms);
    CountIdle(2, 300'000'000, false, 225'000'000);
    SampleCoreCpu(2);
    GuestCoreLoads(loads);
    std::printf("core 2 load %u%%\n", loads[2]);
    // 75 ms of work over ~560 ms: about 13%; counting the polling as work would read about 54%.
    assert(loads[2] != ~0u && loads[2] >= 4 && loads[2] <= 30);
}
'''
with tempfile.TemporaryDirectory(prefix='eden-startup-') as directory:
    directory = Path(directory)
    (directory / 'check.cpp').write_text(code)
    executable = directory / 'check'
    subprocess.run(['clang++-18', '-std=c++20', '-O2', '-pthread', '-DARCHITECTURE_x86_64',
                    '-I', str(root / 'headless'), '-I', str(root / 'src'), '-I', str(source),
                    '-I', str(root / '.deps/fmt-12.1.0/include'),
                    str(directory / 'check.cpp'), str(root / 'headless/performance.cpp'),
                    str(root / 'src/memory_pages.cpp'), str(source / 'common/cpu_features.cpp'),
                    str(source / 'common/x64/rdtsc.cpp'), str(source / 'common/steady_clock.cpp'),
                    '-Wl,--wrap=clock_gettime', '-o', str(executable)], check=True)
    output = subprocess.check_output([str(executable)], text=True, timeout=15)
    (directory / 'loads.cpp').write_text(loads_code)
    loads_executable = directory / 'loads'
    subprocess.run(['clang++-18', '-std=c++20', '-O2', '-pthread', '-DARCHITECTURE_x86_64',
                    '-I', str(root / 'headless'), '-I', str(root / 'src'), '-I', str(source),
                    '-I', str(root / '.deps/fmt-12.1.0/include'),
                    str(directory / 'loads.cpp'), str(root / 'headless/performance.cpp'),
                    str(root / 'src/memory_pages.cpp'), str(source / 'common/cpu_features.cpp'),
                    str(source / 'common/x64/rdtsc.cpp'), str(source / 'common/steady_clock.cpp'),
                    '-o', str(loads_executable)], check=True)
    print(subprocess.check_output([str(loads_executable)], text=True, timeout=15).strip())
    assert 'EDEN_PERF_CPU_CLOCK_CHECK valid=1 ' in output
    assert 'EDEN_PERF_CPU_CLOCK_CHECK valid=0 ' in output
    assert output.count('EDEN_PERF_OWNER_CLOCK_CHECK valid=1 ') == 1
    assert output.count('EDEN_PERF_OWNER_CLOCK_CHECK valid=0 ') == 2
    assert output.count('EDEN_PERF_MEMORY ') == 9 and output.count('EDEN_PERF_SLEEP ') == 9
    snapshots = output.split('EDEN_PERF_SAMPLE ')[1:]
    assert len(snapshots) == 5
    for sample in snapshots:
        assert sample.count('EDEN_PERF_WORKER ') == 7 and sample.count('EDEN_PERF_PROGRESS ') == 4
        assert 'core=1 compilations=1 ' in sample
        assert re.search(r'name=CPUCore_2 cpu_ns=-\d+', sample)
    assert int(re.search(r'name=CPUCore_1 cpu_ns=(\d+)', snapshots[0])[1]) > 0
    assert 'process_cpu_ns=-22 ' in snapshots[1]
    assert re.search(r'name=CPUCore_1 cpu_ns=-22 ', snapshots[1])
    assert int(re.search(r'name=CPUCore_1 cpu_ns=(\d+)', snapshots[2])[1]) > 0
    assert 'process_cpu_ns=-95 ' in snapshots[4] and 'name=CPUCore_1 cpu_ns=-95 ' in snapshots[4]
    assert 'thread=123 pc=abcd svc=18 fpcr=50331648' in snapshots[0] and 'pc=ffff' not in snapshots[0]
    for index, expected in ((1, '-22'), (3, '-95'), (4, '-95')):
        assert re.search(r'EDEN_PERF_CPU_POINT core=0 .*cpu_ns=' + expected + ' ', snapshots[index])
    assert re.search(r'EDEN_PERF_CPU_POINT core=1 .*cpu_ns=\d+ thread=456 pc=1234 svc=ffffffff', snapshots[0])
print('Startup checks, owner CPU qualification, unavailable-clock controls and per-core load without idle polling PASS')
