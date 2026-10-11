#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The limit on how long a game may take to stop (headless/stop_limit.cpp).

On the PC, with a short limit and an action that only counts: a stop that finishes in time does
nothing, one that does not runs the action once, a second Begin keeps the first deadline, and a
new stop gets a new one. Under the thread sanitizer. Then the source: the app arms the limit when
the player asks to leave (also while a game loads) and when a game ends by itself, disarms it before every launcher, and
the restart itself uses no stdio (the watcher may run beside a thread that is stuck holding a
stdio lock).
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
assert main.count('Eden::StopLimit::Begin();') == 3, 'the shortcut (while loading and in the game) and the stop path arm the limit'
# While the game loads: the limit is armed at the press, and the session then ends at once.
loading = main.index('if (pad->TakeReturnToMenu() || asked) {')
assert main.index('Eden::StopLimit::Begin();', loading) < main.index('left_while_loading = true;', loading) < main.index('loaded = system.Load(')
assert main.index('system.Run();') < main.index('load_watch.request_stop();') < main.index('if (left_while_loading) {') < main.index('std::jthread input_worker;')
shortcut = main.index('if (pad->TakeReturnToMenu() || room_lost) {')
assert main.index('room_lost = rooms.GameConnectionLost();') < shortcut
assert main.index('Eden::StopLimit::Begin();', shortcut) < main.index('completion->return_to_menu = true;', shortcut)
stop = main.index('Eden::StopLimit::Begin();', shortcut + 200)
assert stop < main.index('jit_list.Finish();') < main.index('system.ShutdownMainProcess();', stop)
assert 'for (;;) {\n        Eden::StopLimit::End();' in main, 'every way back to the launcher disarms the limit'
assert 'Eden::StopLimit::Start(note);' in main
lifecycle = (root / 'src/lifecycle.c').read_text()
restart = lifecycle.split('int eden_restart_app(void) {', 1)[1].split('\n}\n', 1)[0]
assert 'sceSystemServiceLoadExec("/app0/eboot.bin", NULL)' in restart
assert not re.search(r'\b(fflush|fprintf|printf|fputs|puts)\(', restart), 'no stdio in the restart'
watcher = (root / 'headless/stop_limit.cpp').read_text().split('void RestartNow() noexcept {', 1)[1].split('\n}\n', 1)[0]
assert not re.search(r'\b(fflush|fprintf|printf|fputs|puts)\(', watcher), 'no stdio in the watcher action'

TEST = r'''
#include "stop_limit.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace std::chrono_literals;
static std::atomic<int> fired{0};
static void Fire() noexcept { ++fired; }
#define REQUIRE(what) do { if (!(what)) { std::fprintf(stderr, "stop limit check failed at line %d: %s\n", __LINE__, #what); std::abort(); } } while (0)
int main() {
    using namespace Eden::StopLimit;
    Configure(&Fire, 300ms);
    // No watcher yet (a build that never starts one): nothing happens.
    Begin(); std::this_thread::sleep_for(500ms); REQUIRE(fired == 0); End();
    Start("");
    // A stop that finishes in time.
    Begin(); std::this_thread::sleep_for(100ms); End(); std::this_thread::sleep_for(450ms); REQUIRE(fired == 0);
    // One that does not: the action runs once, however long the stop goes on.
    Begin(); std::this_thread::sleep_for(500ms); REQUIRE(fired == 1);
    std::this_thread::sleep_for(500ms); REQUIRE(fired == 1);
    End();
    // A second Begin during the same stop keeps the first deadline.
    Begin(); std::this_thread::sleep_for(200ms); Begin(); std::this_thread::sleep_for(200ms); REQUIRE(fired == 2);
    End();
    // A new stop has a new deadline.
    Begin(); std::this_thread::sleep_for(200ms); End();
    Begin(); std::this_thread::sleep_for(200ms); REQUIRE(fired == 2);
    std::this_thread::sleep_for(250ms); REQUIRE(fired == 3);
    End();
    // Many stops in a row, each in time.
    for (int i = 0; i < 500; ++i) { Begin(); End(); }
    std::this_thread::sleep_for(450ms); REQUIRE(fired == 3);
    // Starting again adds no second watcher: one stop, one action.
    Start("note");
    Begin(); std::this_thread::sleep_for(500ms); REQUIRE(fired == 4);
    End();
    std::puts("ok");
}
'''
with tempfile.TemporaryDirectory(prefix='eden-stop-') as work:
    work = pathlib.Path(work)
    (work / 'check.cpp').write_text(TEST)
    binary = work / 'check'
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=thread', '-I', str(root / 'headless'),
                    str(work / 'check.cpp'), str(root / 'headless/stop_limit.cpp'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    if result.returncode != 0 or 'ok' not in result.stdout:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'stop limit check failed: exit {result.returncode}')
print('Stop limit PASS: in time, past the limit once, first deadline kept, a new stop a new deadline; armed at the '
      'shortcut and at the stop, disarmed before every launcher; no stdio in the restart')
