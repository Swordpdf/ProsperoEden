// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace Eden {
// Counts contiguous presentation intervals; loading gaps are never reported as FPS.
struct HudClock {
    double start{-1}, last{-1}, fps{-1}, worst_ms{}, window_worst_ms{};
    unsigned frames{};
    void Present(double now) {
        if (last >= 0 && now - last > 0.5) {
            start = last = now;
            fps = -1;
            worst_ms = 0;
            window_worst_ms = 0;
            frames = 0;
            return;
        }
        if (last >= 0) window_worst_ms = std::max(window_worst_ms, (now - last) * 1000.0);
        last = now;
        if (start < 0) { start = now; return; }
        ++frames;
        if (now - start >= 1.0) {
            fps = frames / (now - start);
            worst_ms = window_worst_ms;
            window_worst_ms = 0;
            frames = 0;
            start = now;
        }
    }
};

// Keep startup work behind the loading screen until presentation is sustained.
struct StartupGate {
    double last{-1}, first{-1};
    unsigned smooth_frames{};
    bool Ready(double now) {
        if (first < 0) first = now;
        smooth_frames = last >= 0 && now > last && now - last <= 0.1 ? smooth_frames + 1 : 0;
        last = now;
        return smooth_frames >= 8 || now - first >= 5.0;
    }
};
inline uint32_t HudGlyph(char c) {
    switch (c) {
    case '0': return 0x7b6f;
    case '1': return 0x2c97;
    case '2': return 0x73e7;
    case '3': return 0x73cf;
    case '4': return 0x5bc9;
    case '5': return 0x79cf;
    case '6': return 0x79ef;
    case '7': return 0x7292;
    case '8': return 0x7bef;
    case '9': return 0x7bcf;
    case 'F': return 0x79a4;
    case 'P': return 0x6ba4;
    case 'S': return 0x79cf;
    case 'L': return 0x4927;
    case 'O': return 0x7b6f;
    case 'A': return 0x2bed;
    case 'D': return 0x6b6e;
    case 'I': return 0x7497;
    case 'N': return 0x5ffd;
    case 'G': return 0x796f;
    case 'V': return 0x5b6a;
    case 'K': return 0x5bad;
    case 'W': return 0x5fed;
    case '.': return 0x0002;
    case '-': return 0x01c0;
    default: return 0;
    }
}
inline std::array<uint32_t, 28> HudText(std::string_view text) {
    std::array<uint32_t, 28> glyphs{};
    for (size_t i = 0; i < text.size() && i < glyphs.size(); ++i)
        glyphs[i] = HudGlyph(text[i]);
    return glyphs;
}
// The overlay's width and where its text starts, in a picture 1080 rows high. Where it is drawn
// they are scaled to the picture, and `width` then carries the picture's height to the shader
// (vulkan_hud_draw.inc).
struct HudSnapshot {
    std::array<uint32_t, 28> glyphs{};
    uint32_t width{};
    uint32_t x{28}, y{30}, loading{};
};
static_assert(sizeof(HudSnapshot) == 128); // Vulkan guarantees at least 128 push-constant bytes.
// What a game's start is doing, for the loading screen (loading_scene.glsl draws it: the steps
// one under the other and a progress bar). main.cpp says which step the start is at; the bar's
// share of each step is an estimate, except while shaders are built, where it is counted. Within
// a step of unknown length the bar creeps towards the step's end and never reaches it.
namespace Loading {
enum class Step : uint32_t { none, game, graphics, shaders, starting };
inline std::atomic<uint32_t> step{0}, built{0}, total{0};
inline std::atomic<int64_t> since{0};  // when the step began, in milliseconds
inline int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void Set(Step next) {
    if (next <= Step::game) built = total = 0;
    since.store(Now(), std::memory_order_relaxed);
    step.store(static_cast<uint32_t>(next), std::memory_order_relaxed);
}
inline void Shaders(size_t done, size_t of) {
    built.store(static_cast<uint32_t>(done), std::memory_order_relaxed);
    total.store(static_cast<uint32_t>(of), std::memory_order_relaxed);
}
// Where the bar should stand, 0 to 1.
inline double Target(int64_t now) {
    static constexpr double kFrom[] = {0.0, 0.02, 0.25, 0.40, 0.90}, kTo[] = {0.02, 0.25, 0.40, 0.90, 0.99};
    const uint32_t at = std::min<uint32_t>(step.load(std::memory_order_relaxed), 4);
    const double seconds = static_cast<double>(now - since.load(std::memory_order_relaxed)) / 1000.0;
    double part = 1.0 - std::exp(-std::max(seconds, 0.0) / (at == 4 ? 12.0 : 4.0));
    if (at == 3) {
        const uint32_t of = total.load(std::memory_order_relaxed);
        part = of ? std::min(1.0, static_cast<double>(built.load(std::memory_order_relaxed)) / of) : part * 0.1;
    }
    return kFrom[at] + (kTo[at] - kFrom[at]) * part;
}
// What the shader is given (the overlay's 24 words): the step, the progress in thousandths,
// the shaders built and their total, the seconds the step has lasted. The bar moves towards
// its target a little with every picture and never back within one start.
inline std::array<uint32_t, 24> State() {
    static double shown = 0;
    static int64_t last = 0;
    static uint32_t last_step = 0;
    const int64_t now = Now();
    const uint32_t at = step.load(std::memory_order_relaxed);
    if (at < last_step || at == 0) shown = 0;  // a new start
    last_step = at;
    const double elapsed = std::clamp(static_cast<double>(now - last) / 1000.0, 0.0, 0.25);
    last = now;
    shown = std::max(shown, shown + (Target(now) - shown) * (1.0 - std::exp(-elapsed * 6.0)));
    std::array<uint32_t, 24> state{};
    state[0] = at;
    state[1] = static_cast<uint32_t>(std::clamp(shown, 0.0, 1.0) * 1000.0);
    state[2] = built.load(std::memory_order_relaxed);
    state[3] = total.load(std::memory_order_relaxed);
    state[4] = static_cast<uint32_t>(std::clamp<int64_t>((now - since.load(std::memory_order_relaxed)) / 1000, 0, 99999));
    return state;
}
} // namespace Loading
// The loading screen: the shader draws it from the time and that state (loading_scene.glsl).
// `loading` carries the milliseconds since loading began, plus one; x and y carry the size of the
// picture, set where it is drawn (vulkan_hud_draw.inc); the glyphs' words carry the state.
inline HudSnapshot MakeLoadingSnapshot(double seconds) {
    HudSnapshot snapshot{};
    const auto state = Loading::State();
    std::copy(state.begin(), state.end(), snapshot.glyphs.begin());
    snapshot.width = 1920;
    snapshot.x = 1920;
    snapshot.y = 1080;
    snapshot.loading = 1 + static_cast<uint32_t>(std::max(0.0, seconds) * 1000.0);
    return snapshot;
}
// When the next picture of the loading screen is due. With nothing else to do (idle) the GPU
// thread draws one for every display refresh; while it works on the game's own commands it draws
// only a few a second, so the animation never slows the game's start.
struct LoadingPace {
    double next{-1}, last{-1};
    bool Due(double now, bool idle) {
        if (last >= 0 && (idle ? now < next : now - last < 0.1)) return false;
        next = last < 0 || now - next > 0.05 ? now + 1.0 / 60.0 : next + 1.0 / 60.0;
        last = now;
        return true;
    }
};
// N is the rate the guest itself produces; F is the rate the display shows. They are the same
// until frame generation is on, and the overlay then keeps its usual text (F alone). With frame
// generation F is counted from every frame handed to the present manager (CountPresentedFrame():
// the frames it adds do not go through OnFrameDisplayed(), which runs once per guest frame), and
// never reads above `refresh`, the output's own rate: a display shows no more than that, however
// many frames it is handed. refresh 0: not known, no limit.
inline std::array<char, 29> FormatHudText(const HudClock& clock, const HudClock& output,
                                          double speed, const char* backend, double refresh = 0,
                                          int ping_ms = -1) {
    std::array<char, 29> text{};
    const size_t limit = ping_ms < 0 ? text.size() : text.size() - 6;
    if (clock.fps < 0) {
        std::snprintf(text.data(), limit, "%s F-- S-- W--", backend);
    } else {
        const double shown = output.fps < 0 ? clock.fps : refresh > 0 ? std::min(output.fps, refresh) : output.fps;
        if (shown < clock.fps + 1.5)
            std::snprintf(text.data(), limit, "%s F%.0f S%.0f W%.0f", backend, clock.fps, speed, clock.worst_ms);
        else
            std::snprintf(text.data(), limit, "%s N%.0f F%.0f S%.0f W%.0f", backend, clock.fps, shown, speed,
                          clock.worst_ms);
    }
    if (ping_ms >= 0) {
        const auto length = std::string_view{text.data()}.size();
        std::snprintf(text.data() + length, text.size() - length, " P%d", std::min(ping_ms, 9999));
    }
    return text;
}
inline HudSnapshot MakeHudSnapshot(const HudClock& clock, const HudClock& output, double speed,
                                   double refresh = 0, int ping_ms = -1) {
    const auto text = FormatHudText(clock, output, speed, "VLK", refresh, ping_ms);
    const std::string_view value{text.data()};
    return {HudText(value), static_cast<uint32_t>(value.size() * 16 + 24)};
}
// The OpenGL renderer has no frame generation: what it presents is the guest's own rate, so both
// numbers come from one clock. Callers that cannot have an output clock use these.
inline std::array<char, 29> FormatHudText(const HudClock& clock, double speed, const char* backend,
                                        int ping_ms = -1) {
    return FormatHudText(clock, clock, speed, backend, 0, ping_ms);
}
inline HudSnapshot MakeHudSnapshot(const HudClock& clock, double speed) {
    return MakeHudSnapshot(clock, clock, speed);
}
// Every frame handed to the present manager, the guest's own and each one frame generation adds,
// so the HUD's F measures the output and not just the guest (graphics.cpp).
void CountPresentedFrame();
// Read on the renderer thread; the scheduler captures the returned value per frame.
HudSnapshot GetVulkanHud();
} // namespace Eden
