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
#include <cstdio>
#include <string_view>
using namespace Eden::GuestClock;
static void At(long long ns) { Common::g_wall_clock.ns = ns; Common::g_wall_clock.ticks = ns * 192 / 10000; }
static int CounterPaced() {
    // The game waits for 1/30 second on the counter it sees. Only the first frame is late,
    // by 0.2 ms; later frames have no extra work. Feeding the longer waits caused by a slower
    // counter back into FrameQueued used to drive it to 85%, or 25.5 FPS, indefinitely.
    constexpr std::uint64_t target_ns = 33'333'334;
    long long now = 1'000'000'000, sample_start = 0;
    At(now); FrameQueued(2);
    for (int frame = 0; frame < 300; ++frame) {
        // Wait on Scale itself, as a client would, rather than assuming how rate is applied.
        const auto deadline = Scale(Common::g_wall_clock.ticks) + 640'000;  // 19.2 MHz / 30
        long long first = now, last = now + target_ns * 2;
        while (first < last) {
            const long long middle = first + (last - first) / 2;
            At(middle);
            if (Scale(Common::g_wall_clock.ticks) >= deadline) last = middle;
            else first = middle + 1;
        }
        now = first;
        if (frame == 0) now += 200'000;
        At(now); FrameQueued(2);
        if (frame == 59) sample_start = now;
    }
    const double fps = 240e9 / (now - sample_start);
    std::printf("Counter-paced 30 FPS game: %.6f FPS, clock rate %.6f\n", fps, double(rate.load()) / kOne);
    std::fflush(stdout);
    assert(rate.load() == kOne && fps > 29.99 && fps < 30.01);
    assert(Scale(Common::g_wall_clock.ticks) == std::uint64_t(Common::g_wall_clock.ticks));
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--counter-paced") return CounterPaced();
    Reset(true);  // The old workaround is still available when explicitly chosen for a game.
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
    // One opted-in game's slower clock and fastest swap interval do not leak into another.
    Reset(true);
    At(now); FrameQueued(1);
    for (int i = 0; i < 6; ++i) { now += 22'000'000; At(now); FrameQueued(1); }
    assert(rate.load() == kLowest);
    Reset(true);
    At(now); FrameQueued(2);
    for (int i = 0; i < 8; ++i) { now += 33'333'333; At(now); FrameQueued(2); }
    assert(rate.load() == kOne);
    Reset(false);
    return CounterPaced();  // Returning to normal timing also discards the previous clock offset.
}
"""
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp) / 'common').mkdir()
    (Path(tmp) / 'common' / 'cpu_features.h').write_text('// the test defines the clock itself\n')
    exe = Path(tmp) / 'clock'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + tmp, '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe), '--counter-paced'], check=True)
    subprocess.run([str(exe)], check=True)
print('Guest clock: PASS default counter-paced 30 FPS, explicit opt-in, late frames, step down and back up, and session reset')
