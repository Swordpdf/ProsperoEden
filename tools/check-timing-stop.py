#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Force a consumed stop wake in the real generated CoreTiming::Reset method."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
source = build.parent / 'source/src'
original = (source / 'core/core_timing.cpp').read_text()
generated = (build / 'headless/core_timing.cpp').read_text()
# Use the pinned Event's actual latch/wait implementation, with a scheduling barrier around Set.
event = (source / 'common/thread.h').read_text().split('class Event {', 1)[1].split('\n};', 1)[0]
program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace Common { class Event { EVENT
}; }
struct CoordinatedEvent {
    Common::Event value;
    std::function<void()> after_set;
    void Set() { value.Set(); if (after_set) after_set(); }
    void Wait() { value.Wait(); }
};
struct CoreTiming {
    std::atomic<bool> paused{true};
    CoordinatedEvent pause_event, event;
    std::jthread timer_thread;
    bool has_started = false;
    void Reset();
};
METHOD
int main() {
    CoreTiming timing;
    timing.Reset(); // No thread has been started yet.
    for (unsigned iteration = 0; iteration < 20; ++iteration) {
        std::atomic<unsigned> parked{0};
        timing.timer_thread = std::jthread([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                parked.fetch_add(1, std::memory_order_release);
                timing.pause_event.Wait();
            }
        });
        while (!parked.load(std::memory_order_acquire)) std::this_thread::yield();
        timing.pause_event.after_set = [&] {
            // If Set happens before request_stop, let the worker consume this wake and
            // pass its stop check BEFORE Reset continues. The old method then hangs.
            if (!timing.timer_thread.get_stop_token().stop_requested()) {
                while (parked.load(std::memory_order_acquire) < 2) std::this_thread::yield();
            }
        };
        timing.Reset();
        assert(!timing.timer_thread.joinable() && !timing.has_started);
        timing.pause_event.after_set = {};
    }
}
'''.replace('EVENT', event)
with tempfile.TemporaryDirectory(prefix='eden-timing-stop-') as temporary:
    for name, text in (('original', original), ('generated', generated)):
        method = re.search(r'void CoreTiming::Reset\(\) \{.*?^}', text, re.M | re.S)
        assert method, name
        cpp = Path(temporary) / f'{name}.cpp'
        binary = Path(temporary) / name
        cpp.write_text(program.replace('METHOD', method.group()))
        subprocess.run(['c++', '-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                        str(cpp), '-o', str(binary)], check=True)
        try:
            subprocess.run([str(binary)], check=True, timeout=2)
        except subprocess.TimeoutExpired:
            if name != 'original':
                raise SystemExit('Generated timing reset lost its stop wake')
        else:
            if name == 'original':
                raise SystemExit('Control failed to reproduce the pinned timing stop race')
print('Timing stop PASS: pinned control loses wake; generated Reset joins in 20 forced schedules')
