#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the crash report (headless/crash_report.cpp).

Builds the report's source with a small program that crashes on purpose, twice: as the host
sees it, and with the console's calls stood in (the code that ships). Each case runs in its own
process and logs folder:

  a bad write on a named thread, three calls deep   the report names the signal, the address, the
                                                    thread, what was running, and the calls
  abort, an illegal instruction, an exception       each is reported; the exception with its text
  four threads crashing at once                     one report
  a crash right after a restart                     the app closes instead of starting again,
                                                    unless a game was started since
  the system refusing the restart                   the signal takes its usual course
  memory that cannot be read                        answered, not faulted on
  what the next start does (TakeLast)               the logs move beside the report, the notice
                                                    comes once, five reports stay

and the functions are named from the report with tools/symbolize-crash.py (--show prints that
report).
"""
import datetime
import pathlib
import re
import runpy
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include "crash_report.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// The app's two ways out (src/lifecycle.c on the console).
static bool refuse;
extern "C" int eden_restart_app(void) {
    if (refuse) return -1;
    (void)!write(1, "RESTART\n", 8);
    _exit(42);
}
extern "C" int eden_exit_app(void) {
    if (refuse) return -1;
    (void)!write(1, "EXIT\n", 5);
    _exit(43);
}
#ifdef PS5_NATIVE
// The console calls the report makes.
extern "C" int sceKernelDebugOutText(int, const char* text) { return static_cast<int>(write(2, text, std::strlen(text))); }
extern "C" int sceKernelUsleep(unsigned microseconds) { return usleep(microseconds); }
extern "C" std::int64_t sceKernelGetDirectMemorySize() { return std::int64_t{12} << 30; }
extern "C" std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t* start,
                                                           std::size_t* size) {
    *start = 0;
    *size = std::size_t{1234} << 20;
    return 0;
}
extern "C" std::size_t eden_heap_committed(void) { return std::size_t{256} << 20; }
extern "C" std::size_t eden_heap_large_held(unsigned* blocks) {
    *blocks = 3;
    return std::size_t{96} << 20;
}
extern "C" std::size_t eden_heap_last_failure(unsigned* why, std::size_t* count) {
    *why = 2;
    *count = 1;
    return std::size_t{134217728};
}
#endif

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "check failed, line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

// Three calls deep, none inlined and none a tail call: the report must find the two callers.
static volatile int sink;
static volatile std::uintptr_t bad = 16;
[[gnu::noinline]] void Innermost(volatile int* target) {
    *target = 7;
    sink = 1;
}
[[gnu::noinline]] void Middle(volatile int* target) {
    Innermost(target);
    sink = 2;
}
[[gnu::noinline]] void Outer(volatile int* target) {
    Middle(target);
    sink = 3;
}

static bool Exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}
static void Put(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

static int TakeLastCheck(const std::string& logs) {
    using Eden::Crash::TakeLast;
    const std::string note = logs + "/crash-note.txt", eden = logs + "/eden_log.txt";
    // Nothing left by the previous run.
    CHECK(TakeLast(logs, eden).report.empty());
    // A note that does not name a report is dropped.
    Put(note, "../../elsewhere.txt 1\n");
    CHECK(TakeLast(logs, eden).report.empty() && !Exists(note));
    // So is one whose report is not there.
    Put(note, "crash-20260101-000000.txt 1\n");
    CHECK(TakeLast(logs, eden).report.empty() && !Exists(note));
    for (int run = 1; run <= 7; ++run) {
        const std::string stem = logs + "/crash-2026010" + std::to_string(run) + "-120000";
        Put(stem + ".txt", "report\n");
        Put(logs + "/stderr.prev.log", "that run's messages\n");
        Put(logs + "/heap.prev.log", "that run's stages\n");
        Put(eden, "that run's emulator log\n");
        Put(note, stem.substr(logs.size() + 1) + ".txt " + (run % 2 ? "1" : "0") + "\n");
        const Eden::Crash::Last last = TakeLast(logs, eden);
        CHECK(last.report == stem + ".txt" && last.restarted == (run % 2 == 1));
        CHECK(!Exists(note) && !Exists(logs + "/stderr.prev.log") && !Exists(logs + "/heap.prev.log") && !Exists(eden));
        CHECK(Exists(stem + "-stderr.log") && Exists(stem + "-heap.log") && Exists(stem + "-eden_log.txt"));
        CHECK(TakeLast(logs, eden).report.empty());  // announced once
    }
    for (int run = 1; run <= 7; ++run) {
        const std::string stem = logs + "/crash-2026010" + std::to_string(run) + "-120000";
        for (const char* part : {".txt", "-stderr.log", "-heap.log", "-eden_log.txt"})
            CHECK(Exists(stem + part) == (run >= 3));  // the five newest stay
    }
    std::puts("TAKELAST_OK");
    return 0;
}

static int ReadableCheck() {
    using Eden::Crash::Readable;
    char local[64] = "here";
    CHECK(Readable(local, sizeof(local)));
    CHECK(!Readable(reinterpret_cast<const void*>(std::uintptr_t{16}), 8));
    char* region = static_cast<char*>(mmap(nullptr, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(region != MAP_FAILED);
    CHECK(!Readable(region, 4096));
    CHECK(mprotect(region, 4096, PROT_READ | PROT_WRITE) == 0);
    CHECK(Readable(region, 4096));
    CHECK(!Readable(region + 4000, 200));  // runs into the page that cannot be read
    CHECK(Readable(local, sizeof(local)) && Readable(region, 4096));  // and nothing is left behind
    std::puts("READABLE_OK");
    return 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::string logs = argc > 2 ? argv[2] : ".";
    if (mode == "takelast") return TakeLastCheck(logs);
    refuse = mode == "refused";
    // As headless/main.cpp does: the exception's text goes into the report.
    std::set_terminate([] {
        const char* detail = "no active exception";
        if (const auto current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& error) {
                detail = error.what();
            } catch (...) {
                detail = "non-standard exception";
            }
        }
        Eden::Crash::Fail(detail);
    });
    Eden::Crash::Install(logs, "test-version", mode == "again" || mode == "again-game");
    if (mode == "readable") return ReadableCheck();
    if (mode != "again")
        Eden::Crash::SetSession("game Test.nsp (0100000000000000), Vulkan, resolution 2x, AMD FSR", true);
    if (mode == "abort") std::abort();
    if (mode == "ill") __builtin_trap();
    if (mode == "throw")
        std::thread([] {
            Eden::Crash::NameThread("Thrower");
            throw std::runtime_error("test exception: nothing caught it");
        }).join();
    const auto crash = [] {
        Eden::Crash::NameThread("CPUCore_1");
        Outer(reinterpret_cast<volatile int*>(bad));
    };
    if (mode == "many") {
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back(crash);
        for (auto& thread : threads) thread.join();
    }
    std::thread(crash).join();
    return 1;
}
'''


def build(work, name, flags):
    source = work / 'crash-check.cpp'
    source.write_text(PROGRAM)
    binary = work / name
    # The app's linker script names the code's bounds; the host's linker takes it as an input.
    subprocess.run(['clang++-18', '-std=c++20', '-O2', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fno-rtti', '-fuse-ld=lld', *flags, '-I', str(root / 'headless'),
                    str(source), str(root / 'headless/crash_report.cpp'), str(root / 'tools/unwind.ld'),
                    '-o', str(binary)], check=True)
    return binary


def functions(binary):
    """name -> (offset into the code, size) of the three functions on the crashing thread's stack."""
    text_start, _offset, _size = \
        runpy.run_path(str(root / 'tools/symbolize-crash.py'))['sections'](binary.read_bytes())['.text']
    found = {}
    listing = subprocess.run(['llvm-nm-18', '-S', '--defined-only', str(binary)], capture_output=True, text=True,
                             check=True).stdout
    for line in listing.splitlines():
        parts = line.split()
        for name in ('Innermost', 'Middle', 'Outer'):
            if len(parts) == 4 and re.fullmatch(rf'_Z\d+{name}PVi', parts[3]):
                found[name] = (int(parts[0], 16) - text_start, int(parts[1], 16))
    assert set(found) == {'Innermost', 'Middle', 'Outer'}, listing[:400]
    return found


def run(binary, mode, console):
    """Runs one case in a fresh logs folder; returns (exit status, stdout, stderr, reports, note)."""
    logs = pathlib.Path(tempfile.mkdtemp(prefix=f'crash-{mode}-', dir=binary.parent))
    result = subprocess.run([str(binary), mode, str(logs)], capture_output=True, text=True, timeout=60)
    reports = sorted(logs.glob('crash-2*.txt'))
    note = (logs / 'crash-note.txt').read_text() if (logs / 'crash-note.txt').exists() else ''
    return result.returncode, result.stdout, result.stderr, reports, note


def offsets(text):
    return [int(value, 16) for value in re.findall(r'eboot\+0x([0-9a-f]+)', text)]


def check(binary, console):
    where = functions(binary)
    inside = lambda offset, name: where[name][0] <= offset < where[name][0] + where[name][1]

    # A bad write on a named thread, three calls deep.
    status, out, err, reports, note = run(binary, 'thread', console)
    assert status == 42 and out == 'RESTART\n' and len(reports) == 1, (status, out, err[-600:])
    report = reports[0].read_text()
    assert report.startswith('ProsperoEden crash report\nversion: test-version (code size 0x'), report[:200]
    stamp = re.search(r'^time: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d), (\d+) s after the app started$', report, re.M)
    assert stamp, report
    written = datetime.datetime.strptime(stamp.group(1), '%Y-%m-%d %H:%M:%S')
    assert abs((datetime.datetime.now() - written).total_seconds()) < 120, stamp.group(1)   # local time
    assert reports[0].name == written.strftime('crash-%Y%m%d-%H%M%S.txt'), reports[0].name
    assert re.search(r'^what: SIGSEGV \(invalid memory access\), code \d+, address 0x0000000000000010$', report, re.M)
    fault = re.search(r'^where: eboot\+0x([0-9a-f]+)$', report, re.M)
    assert fault and inside(int(fault.group(1), 16), 'Innermost'), report
    assert re.search(r'^thread: CPUCore_1 \(0x[0-9a-f]+\)$', report, re.M), report
    assert 'running: game Test.nsp (0100000000000000), Vulkan, resolution 2x, AMD FSR\n' in report
    assert re.search(r'^  rip [0-9a-f]{16}  rsp [0-9a-f]{16}  rbp [0-9a-f]{16}$', report, re.M)
    assert re.search(r'^  r14 [0-9a-f]{16}  r15 [0-9a-f]{16}  flags [0-9a-f]{16}$', report, re.M)
    calls = offsets(report.split('calls found on the stack', 1)[1])
    middle = [i for i, offset in enumerate(calls) if inside(offset, 'Middle')]
    outer = [i for i, offset in enumerate(calls) if inside(offset, 'Outer')]
    assert middle and outer and middle[0] < outer[0], (calls, where)   # the callers, innermost first
    assert not any(inside(offset, 'Innermost') for offset in calls)    # the fault itself is "where"
    assert report.endswith('then: the app starts again\n')
    assert note == reports[0].name + ' 1\n', note
    assert 'EDEN_CRASH_REPORT installed=1 signals=7 ' in err and 'EDEN_CRASH what=SIGSEGV where=eboot+0x' in err
    assert f'thread=CPUCore_1 report={reports[0]} restart=1' in err, err[-400:]
    assert ('memory: largest free block 1234 MiB of 12288 MiB, heap 256 MiB and 96 MiB in 3 large blocks\n' in report) \
        == console, report
    assert ('heap refused: 1 request, the last of 134217728 bytes: the console had no memory for a new piece\n'
            in report) == console, report

    # The functions, named from the report.
    named = subprocess.run([sys.executable, str(root / 'tools/symbolize-crash.py'), str(reports[0]), str(binary)],
                           capture_output=True, text=True, check=True).stdout
    assert re.search(r'^where: eboot\+0x[0-9a-f]+   Innermost\(', named, re.M), named
    assert re.search(r'^  eboot\+0x[0-9a-f]+   Middle\(', named, re.M) and \
        re.search(r'^  eboot\+0x[0-9a-f]+   Outer\(', named, re.M), named
    assert 'WARNING' not in named
    if '--show' in sys.argv and not console:
        print(named)
    # As a console writes it: its code cannot be read there, so every value that points into the
    # code is listed with a "?". One that is no return address (an address inside the faulting
    # function that no call precedes) is left out when the functions are named.
    tool = runpy.run_path(str(root / 'tools/symbolize-crash.py'))
    data = binary.read_bytes()
    _start, at, _size = tool['sections'](data)['.text']
    begin, length = where['Innermost']
    stray = next(offset for offset in range(begin + 1, begin + length)
                 if not tool['after_call'](data[at + offset - 8:at + offset]))
    head, listed = report.split('calls found on the stack', 1)
    first, rest = listed.split('\n', 1)
    unchecked = reports[0].with_name('unchecked.txt')
    unchecked.write_text(head + 'calls found on the stack' + first + f'\n  eboot+0x{stray:x} ?\n' +
                         ''.join(line + (' ?\n' if line.startswith('  eboot+') else '\n') for line in rest.splitlines()))
    named = subprocess.run([sys.executable, str(root / 'tools/symbolize-crash.py'), str(unchecked), str(binary)],
                           capture_output=True, text=True, check=True).stdout
    assert re.search(r'^  eboot\+0x[0-9a-f]+   Middle\(', named, re.M) and \
        re.search(r'^  eboot\+0x[0-9a-f]+   Outer\(', named, re.M) and ' ?' not in named, named
    assert '\n1 stack values that point into the code but do not follow a call were left out.' in named, named

    # Other ways to end.
    status, out, err, reports, note = run(binary, 'abort', console)
    assert status == 42 and len(reports) == 1 and 'what: SIGABRT (abort)' in reports[0].read_text(), (status, err[-400:])
    assert '\nthread: main (0x' in reports[0].read_text()
    status, out, err, reports, note = run(binary, 'ill', console)
    assert status == 42 and len(reports) == 1 and 'what: SIGILL (illegal instruction)' in reports[0].read_text()
    status, out, err, reports, note = run(binary, 'throw', console)
    report = reports[0].read_text() if reports else ''
    assert status == 42 and 'what: the app stopped itself\nreason: test exception: nothing caught it\n' in report, \
        (status, report[:400], err[-400:])
    assert '\nthread: Thrower (0x' in report and note.endswith(' 1\n')

    # Four threads at once: one report, one restart.
    status, out, err, reports, note = run(binary, 'many', console)
    assert status == 42 and out == 'RESTART\n' and len(reports) == 1, (status, out, len(reports))

    # Right after a restart the app closes; once a game was started it starts again.
    status, out, err, reports, note = run(binary, 'again', console)
    report = reports[0].read_text()
    assert status == 43 and out == 'EXIT\n' and note == reports[0].name + ' 0\n', (status, out, note)
    assert 'running: starting\n' in report and report.endswith('(it had just started again after a crash)\n')
    status, out, err, reports, note = run(binary, 'again-game', console)
    assert status == 42 and note.endswith(' 1\n'), (status, note)

    # The system refuses: the report is there, and the signal ends the process as it always did.
    status, out, err, reports, note = run(binary, 'refused', console)
    assert status == -11 and out == '' and len(reports) == 1, (status, out)
    assert 'EDEN_CRASH the app did not start again; the system handles the crash' in err

    for mode, answer in (('readable', 'READABLE_OK\n'), ('takelast', 'TAKELAST_OK\n')):
        status, out, err, reports, note = run(binary, mode, console)
        assert status == 0 and out == answer, (mode, status, err[-600:])


with tempfile.TemporaryDirectory(prefix='eden-crash-') as work:
    work = pathlib.Path(work)
    check(build(work, 'host', []), False)
    check(build(work, 'console-calls', ['-DPS5_NATIVE']), True)
print('Crash report: signal, address, thread, session, registers and calls; abort, illegal instruction and '
      'uncaught exception; one report for many threads; no restart loop; refusal falls back to the system; '
      'unreadable memory; logs kept beside five reports; functions named from the report PASS')
