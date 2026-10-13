// Eden PS5 port: a guest tick counter that gives when a frame is a little late.
//
// Some games time their own frames with the system tick counter and choose their frame rate by
// it: a large open-world game shows a frame every two display refreshes while its frames take
// 33.3 ms, and goes to three refreshes (20 frames a second) after a few frames that took longer.
// On the console its heavy scenes need 34 to 37 ms a frame: a few percent over, for which the
// game gave up a third of its frame rate, tried again a second later, and gave up again.
//
// The counter the guest reads follows the clock at a rate of its own (Scale). While the game
// hands its frames in on time the rate is 1. When frames come later than the interval the game
// asked for, the rate is lowered, by up to 15%, so that the game times them at just under that
// interval: it keeps its frame rate, and the frames are shown as they come (27 to 28 a second
// in those scenes instead of 20 to 22). Frames later than that read late even so, and the game
// goes down a step as it would on its own. Only what the guest reads from the counter
// stretches; the kernel's timeouts, the display and the audio keep the clock.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include "common/cpu_features.h"

namespace Eden::GuestClock {
inline constexpr std::uint32_t kOne = 1u << 16;                  // rates are 16.16
inline constexpr std::uint32_t kLowest = (kOne * 85 + 50) / 100; // 15% is the most it gives
inline std::atomic<std::uint32_t> sequence{0};                   // odd while the three below change
inline std::atomic<std::uint64_t> host_base{0}, guest_base{0};
inline std::atomic<std::uint32_t> rate{kOne};
inline std::atomic_flag writing = ATOMIC_FLAG_INIT;

// The guest's reading for a reading of the clock's counter.
inline std::uint64_t Scale(std::uint64_t host) noexcept {
    for (;;) {
        const std::uint32_t before = sequence.load(std::memory_order_acquire);
        const std::uint64_t from = host_base.load(std::memory_order_relaxed);
        const std::uint64_t base = guest_base.load(std::memory_order_relaxed);
        const std::uint32_t now = rate.load(std::memory_order_relaxed);
        if ((before & 1) || sequence.load(std::memory_order_acquire) != before) continue;
        // A reading taken just before the rate changed: the counter never goes back.
        return host <= from ? base : base + (((host - from) * now) >> 16);
    }
}

// The counter goes on from where it is, at a new rate.
inline void SetRate(std::uint32_t next) noexcept {
    while (writing.test_and_set(std::memory_order_acquire)) {}
    if (rate.load(std::memory_order_relaxed) != next) {
        const std::uint64_t host = static_cast<std::uint64_t>(Common::g_wall_clock.GetCNTPCT());
        const std::uint64_t guest = Scale(host);
        sequence.fetch_add(1, std::memory_order_acq_rel);
        host_base.store(host, std::memory_order_relaxed);
        guest_base.store(guest, std::memory_order_relaxed);
        rate.store(next, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_acq_rel);
    }
    writing.clear(std::memory_order_release);
}

// The rate for frames that came `period_ns` apart when the game asked for `target_ns`: 1 on
// time, otherwise the share that makes the period read 2% under the target, and no lower than
// the most the counter gives.
constexpr std::uint32_t RateFor(std::uint64_t period_ns, std::uint64_t target_ns) noexcept {
    if (target_ns == 0 || period_ns * 1000 <= target_ns * 1003) return kOne;
    const std::uint64_t wanted = target_ns * 98 / 100 * kOne / period_ns;
    return wanted < kLowest ? kLowest : static_cast<std::uint32_t>(wanted);
}

// The game hands a frame in (BufferQueueProducer::QueueBuffer), asking for it to be shown
// `swap_interval` refreshes of the guest's 60 Hz display after the last. One thread calls this.
//
// While the game shows frames at the fastest interval it has used lately, the rate follows how
// late they come. When it has just gone down a step (a longer interval than three seconds ago),
// the counter gives all it can: timed honestly there, the game would wait for frames that take
// less than the shorter interval before it comes back up, and stay down in scenes that need a
// few percent more. When it comes back up the rate stays as it is until four frames tell.
inline void FrameQueued(int swap_interval) noexcept {
    static std::uint64_t last_ns = 0, fast_seen_ns = 0;
    static int last_interval = 0, fast_interval = 0;
    static std::array<std::uint64_t, 4> periods{};
    static unsigned filled = 0, at = 0;
    const std::uint64_t now_ns = static_cast<std::uint64_t>(Common::g_wall_clock.GetTimeNS().count());
    const std::uint64_t period = now_ns - last_ns;
    last_ns = now_ns;
    if (swap_interval < 1 || swap_interval > 4) {
        SetRate(kOne);
        return;
    }
    const std::uint64_t target = static_cast<std::uint64_t>(swap_interval) * 16'666'667;
    if (fast_interval == 0 || swap_interval <= fast_interval || now_ns - fast_seen_ns > 3'000'000'000ull) {
        fast_interval = swap_interval;
        fast_seen_ns = now_ns;
    }
    if (swap_interval != last_interval) {
        last_interval = swap_interval;
        filled = at = 0;
    }
    if (swap_interval > fast_interval) {
        SetRate(kLowest);
        return;
    }
    // A hitch (loading, a shader being built) is not what the game's frames take: it is left out.
    if (period > target * 16 / 10) return;
    periods[at] = period;
    at = (at + 1) % periods.size();
    filled = (std::min)(filled + 1, static_cast<unsigned>(periods.size()));
    const std::uint32_t current = rate.load(std::memory_order_relaxed);
    if (filled < periods.size() && current != kOne) return;
    const std::uint64_t worst = *std::max_element(periods.begin(), periods.begin() + filled);
    const std::uint32_t next = RateFor(worst, target);
    // Small changes are not worth a new base for every reader.
    if (next == kOne ? current != kOne : (next > current ? next - current : current - next) > kOne / 400) SetRate(next);
}
} // namespace Eden::GuestClock
