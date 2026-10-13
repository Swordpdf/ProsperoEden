#!/usr/bin/env python3
"""Host check of the guest tick counter that gives with late frames (headless/guest_clock.h)."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r"""
#include <cstdint>
// The clock the header reads, as the test sets it.
namespace Common {
struct Nanoseconds { long long value; long long count() const { return value; } };
struct WallClock {
    long long GetCNTPCT() const { return ticks; }
    Nanoseconds GetTimeNS() const { return {ns}; }
    mutable long long ticks = 0, ns = 0;
};
inline WallClock g_wall_clock;
}
#define EDEN_GUEST_CLOCK_TEST 1
#include "guest_clock.h"
#include <cassert>
using namespace Eden::GuestClock;
static void At(long long ns) { Common::g_wall_clock.ns = ns; Common::g_wall_clock.ticks = ns * 192 / 10000; }
int main() {
    // The rate for a period: on time, a little late, too late.
    static_assert(RateFor(33'333'333, 33'333'334) == kOne && RateFor(33'400'000, 33'333'334) == kOne);
    static_assert(RateFor(35'000'000, 33'333'334) == 33'333'334ull * 98 / 100 * kOne / 35'000'000);
    static_assert(RateFor(38'000'000, 33'333'334) > kLowest && RateFor(39'500'000, 33'333'334) == kLowest);
    static_assert(RateFor(52'000'000, 50'000'001) < kOne && RateFor(1, 0) == kOne);
    // The counter is the clock until frames are late.
    assert(Scale(0) == 0 && Scale(123456789) == 123456789);
    long long now = 1'000'000'000;
    At(now); FrameQueued(2);
    for (int i = 0; i < 8; ++i) { now += 33'333'333; At(now); FrameQueued(2); }
    assert(rate.load() == kOne);
    const std::uint64_t before = Scale(Common::g_wall_clock.ticks);
    assert(before == std::uint64_t(Common::g_wall_clock.ticks));
    // Frames 36 ms apart: the rate drops, the counter goes on from where it was and never back,
    // and a frame reads about 2% under two refreshes.
    for (int i = 0; i < 6; ++i) { now += 36'000'000; At(now); FrameQueued(2); }
    const std::uint32_t slow = rate.load();
    assert(slow < kOne && slow > kLowest);
    assert(Scale(Common::g_wall_clock.ticks) >= before);
    const std::uint64_t a = Scale(Common::g_wall_clock.ticks);
    now += 36'000'000; At(now);
    const std::uint64_t b = Scale(Common::g_wall_clock.ticks);
    const double read_ms = double(b - a) / 19200.0;
    assert(read_ms > 32.0 && read_ms < 33.0);
    assert(Scale(Common::g_wall_clock.ticks - 1'000'000) <= b);
    // One hitch among them changes nothing; frames far too late get all the give and no more.
    FrameQueued(2);
    now += 90'000'000; At(now); FrameQueued(2);
    assert(rate.load() == slow);
    for (int i = 0; i < 6; ++i) { now += 44'000'000; At(now); FrameQueued(2); }
    assert(rate.load() == kLowest);
    // The game goes down to three refreshes: all the give at once, for as long as two refreshes
    // were seen within three seconds.
    now += 50'000'000; At(now); FrameQueued(3);
    assert(rate.load() == kLowest);
    for (int i = 0; i < 20; ++i) { now += 50'000'000; At(now); FrameQueued(3); }
    assert(rate.load() == kLowest);
    // It comes back up: the rate stays until four frames tell, then follows them.
    now += 33'333'333; At(now); FrameQueued(2);
    assert(rate.load() == kLowest);
    for (int i = 0; i < 2; ++i) { now += 35'000'000; At(now); FrameQueued(2); }
    assert(rate.load() == kLowest);
    now += 35'000'000; At(now); FrameQueued(2);
    assert(rate.load() > kLowest && rate.load() < kOne);
    for (int i = 0; i < 6; ++i) { now += 33'333'333; At(now); FrameQueued(2); }
    assert(rate.load() == kOne);
    // Three refreshes for more than three seconds is the game's own rate: the clock, unless late.
    now += 50'000'000; At(now); FrameQueued(3);
    assert(rate.load() == kLowest);
    for (int i = 0; i < 70; ++i) { now += 50'000'000; At(now); FrameQueued(3); }
    assert(rate.load() == kOne);
    for (int i = 0; i < 6; ++i) { now += 53'000'000; At(now); FrameQueued(3); }
    assert(rate.load() < kOne && rate.load() > kLowest);
    for (int i = 0; i < 6; ++i) { now += 50'000'000; At(now); FrameQueued(3); }
    assert(rate.load() == kOne);
    assert(rate.load() == kOne);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp) / 'common').mkdir()
    (Path(tmp) / 'common' / 'cpu_features.h').write_text('// the test defines the clock itself\n')
    exe = Path(tmp) / 'clock'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + tmp, '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('Guest clock: PASS on time, late, a hitch, too late, a step down and back up, the game\'s own rate; the counter never goes back')
