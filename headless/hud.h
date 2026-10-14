// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

// The overlay's baked font: how far each character moves the pen, and how large its box is.
// Outside the namespace below, which opens its own.
#include "hud_text_advances.h"

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
// How many characters of the overlay's line fit in a HudSnapshot's words: four go to a word, and
// the line is NUL-terminated, so the shader stops at the first byte that is 0.
inline constexpr size_t kHudTextMax = 111;
// Where the overlay's box stands in that picture: against a corner, so it never gets in the way
// of the middle of the screen. Any corner is that many units in from its two edges, so all four
// read the same way once the picture is scaled.
inline constexpr uint32_t kHudBoxX = 6, kHudBoxY = 4;
inline constexpr uint32_t kHudPictureWidth = 1920, kHudPictureHeight = 1080;
// The corners the caller can name, top left through bottom right (kHudCornerKeys, the launchers'
// own order in settings_store.h).
inline constexpr int kHudCornerCount = 4;
// What the line says about the machine: a percentage that could not be read shows as "--", and a
// memory pool that could not be read as a size of 0.
struct HudMachine {
    unsigned cpu = ~0u;
    // The four emulated cores, each as a share of one CPU: shown instead of `cpu` when known.
    unsigned cores[4] = {~0u, ~0u, ~0u, ~0u};
    unsigned gpu = ~0u;
    unsigned long long memory_free = 0;
    unsigned long long memory_total = 0;
    unsigned long long graphics = 0;  // of what is taken, what the graphics hold; 0: not known
};
// The overlay's line and where its box goes, in a picture 1080 rows high. Where it is drawn they
// are scaled to the picture, and `width` then carries the picture's height to the shader instead
// (vulkan_hud_draw.inc): the box's height and the baseline in it come from kHudBoxUnits and
// kHudBaselineUnits, which hud_text_advances.h and loading_text.glsl are both generated with.
// `glyphs` is the line itself, four characters to a word.
struct HudSnapshot {
    std::array<uint32_t, 28> glyphs{};
    uint32_t width{};
    uint32_t x{kHudBoxX}, y{kHudBoxY}, loading{};
};
static_assert(sizeof(HudSnapshot) == 128); // Vulkan guarantees at least 128 push-constant bytes.
// The overlay's snapshot carries how and where its chip is drawn in `loading`, which the loading
// screen leaves to the milliseconds since it began: bit 31 marks the overlay, so the shader can
// tell the two apart, and the bits below it say the rest. The loading path never sets bit 31, so
// a snapshot from it still reads as the loading screen (vulkan_hud_draw.inc, vulkan_hud.frag).
inline constexpr uint32_t kHudOverlayFlag = 0x80000000u;
// The overlay's chip carries its size and whether it is coloured in `loading`, beside the bit
// that marks it. The size is the chip's scale times two, in three bits (2 = 1x, 3 = 1.5x,
// 4 = 2x, so a large chip keeps the pattern the old kHudLargeFlag had); the colour bit says the
// line is drawn in its inks rather than one (bake-loading-text.py, vulkan_hud.frag).
inline constexpr uint32_t kHudSizeShift = 28, kHudSizeMask = 0x70000000u;
inline constexpr uint32_t kHudColorsFlag = 0x08000000u;
// The chip's size, as the field carries it: the launcher's size index 0, 1, 2 is a scale of
// 1x, 1.5x, 2x, stored as that scale times two (2, 3, 4).
inline uint32_t HudSizeField(int size) {
    return (2u + (static_cast<uint32_t>(size) & 3u)) << kHudSizeShift;
}
// The scale the field names, for the box and the line alike (vulkan_hud_draw.inc, DrawHud).
inline float HudSizeScale(uint32_t field) {
    return static_cast<float>((field >> kHudSizeShift) & 7u) * 0.5f;
}
// What the overlay's line may say, bit by bit: the pieces the user can turn off (the launcher's
// hud_fields, settings_store.h). All on by default.
inline constexpr uint32_t kHudFieldBackend = 1u << 0;
inline constexpr uint32_t kHudFieldFps = 1u << 1;
inline constexpr uint32_t kHudFieldMs = 1u << 2;
inline constexpr uint32_t kHudFieldSpeed = 1u << 3;
inline constexpr uint32_t kHudFieldCpu = 1u << 4;
inline constexpr uint32_t kHudFieldGpu = 1u << 5;
inline constexpr uint32_t kHudFieldMem = 1u << 6;
inline constexpr uint32_t kHudFieldPing = 1u << 7;
// Not a piece of its own but how the CPU piece reads: set, only the busiest of the four emulated
// cores (the one that holds a game back); clear, as in every setting saved before there was a
// choice, each of the four.
inline constexpr uint32_t kHudFieldOneCore = 1u << 8;
inline constexpr uint32_t kHudFieldAll = 0xffu;
// True while the snapshot is the loading screen rather than the overlay's line.
inline bool HudLoading(const HudSnapshot& hud) {
    return hud.loading != 0 && (hud.loading & kHudOverlayFlag) == 0;
}
// The line in a snapshot's words, four characters to a word, and a zero after it.
inline void PutHudText(HudSnapshot& hud, std::string_view text) {
    auto* bytes = reinterpret_cast<char*>(hud.glyphs.data());
    const size_t length = std::min(text.size(), kHudTextMax);
    std::memcpy(bytes, text.data(), length);
    std::memset(bytes + length, 0, sizeof(hud.glyphs) - length);
}
// What a game's start is doing, for the loading screen (loading_scene.glsl draws it: the game's
// cover with a line of light going round it, and the step under it). main.cpp says which step the
// start is at; each step's share of the line is an estimate, except while shaders are built, where
// it is counted. Within a step of unknown length the line creeps towards the step's end and never
// reaches it.
namespace Loading {
enum class Step : uint32_t { none, game, graphics, shaders, starting };
inline std::atomic<uint32_t> step{0}, built{0}, total{0};
inline std::atomic<int64_t> since{0};  // when the step began, in milliseconds
// The cover in the screen's picture: its width, and its height 16 bits up; 0 without one
// (loading_art.h).
inline std::atomic<uint32_t> art_size{0};
// The launcher's theme for the screen: its accent, pale accent, deep accent and base colours as
// 0x01RRGGBB. 0: the shader's own, which are Canopy's.
inline std::array<std::atomic<uint32_t>, 4> palette{};
inline void Palette(uint32_t accent, uint32_t pale, uint32_t deep, uint32_t base) {
    const uint32_t colours[]{accent, pale, deep, base};
    for (size_t i = 0; i < palette.size(); ++i)
        palette[i].store(0x01000000u | (colours[i] & 0xffffffu), std::memory_order_relaxed);
}
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
// How far the start should be shown to be, 0 to 1 (loading_scene.glsl knows each step's share).
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
// the shaders built and their total, the seconds and then the milliseconds the step has lasted,
// the cover's size, the progress in millionths, and from the ninth word the theme's colours. The
// progress moves towards its target a little with every picture and never back within one start.
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
    const int64_t lasted = std::max<int64_t>(now - since.load(std::memory_order_relaxed), 0);
    state[4] = static_cast<uint32_t>(std::min<int64_t>(lasted / 1000, 99999));
    state[5] = static_cast<uint32_t>(std::min<int64_t>(lasted, 600000));
    state[6] = art_size.load(std::memory_order_relaxed);
    state[7] = static_cast<uint32_t>(std::clamp(shown, 0.0, 1.0) * 1000000.0);
    for (size_t i = 0; i < palette.size(); ++i) state[8 + i] = palette[i].load(std::memory_order_relaxed);
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
// The line the overlay shows: "VLK 27FPS 37.0MS CPU 55% GPU 43% MEM 4.1/8.0G GFX 2.5G". The rate the
// guest itself produces, and the rate the display shows when they differ (with frame generation
// the display is handed frames the guest never made, and never more than `refresh`, its own
// rate; refresh 0: not known, no limit); then the worst frame of the last second, how far behind
// 100% the emulation is when it is behind at all, and the machine's own load. Where a number
// cannot be read it shows as "--".
inline std::array<char, kHudTextMax + 1> FormatHudText(const HudClock& clock, const HudClock& output,
                                                       double speed, const char* backend,
                                                       const HudMachine& machine, double refresh = 0,
                                                       int ping_ms = -1,
                                                       uint32_t fields = kHudFieldAll) {
    std::array<char, kHudTextMax + 1> text{};
    char line[192]{};
    char piece[64]{};
    size_t used = 0;
    // Every piece goes in as it is written, up to what the line can hold. A piece opens with a
    // space, which is dropped where the line is still empty.
    const auto put = [&](const char* value) {
        const char* v = used == 0 && *value == ' ' ? value + 1 : value;
        const size_t take = std::min(std::strlen(v), text.size() - 1 - used);
        std::memcpy(line + used, v, take);
        used += take;
        line[used] = '\0';
    };
    if (fields & kHudFieldBackend) {
        std::snprintf(piece, sizeof(piece), "%s", backend);
        put(piece);
    }
    if (clock.fps < 0) {
        if (fields & kHudFieldFps) put(" --FPS");
        if (fields & kHudFieldMs) put(" --MS");
    } else {
        const double shown = output.fps < 0 ? clock.fps
                                            : refresh > 0 ? std::min(output.fps, refresh) : output.fps;
        if (fields & kHudFieldFps) {
            if (shown < clock.fps + 1.5)
                std::snprintf(piece, sizeof(piece), " %.0fFPS", clock.fps);
            else
                std::snprintf(piece, sizeof(piece), " %.0f/%.0fFPS", clock.fps, shown);
            put(piece);
        }
        if (fields & kHudFieldMs) {
            std::snprintf(piece, sizeof(piece), " %.1fMS", clock.worst_ms);
            put(piece);
        }
    }
    // Speed only when it is not a full 100: the rates on the line already say the rest.
    if ((fields & kHudFieldSpeed) && std::fabs(speed - 100.0) > 0.5) {
        std::snprintf(piece, sizeof(piece), " %.0f%%", speed);
        put(piece);
    }
    char cpu[8] = "--";
    char gpu[8] = "--";
    if (machine.cpu != ~0u) std::snprintf(cpu, sizeof(cpu), "%u", machine.cpu);
    if (machine.gpu != ~0u) std::snprintf(gpu, sizeof(gpu), "%u", machine.gpu);
    if (fields & kHudFieldCpu) {
        const bool by_core = machine.cores[0] != ~0u || machine.cores[1] != ~0u || machine.cores[2] != ~0u ||
                             machine.cores[3] != ~0u;
        if (by_core && (fields & kHudFieldOneCore) != 0) {
            // Only the busiest core.
            unsigned busiest = 0;
            for (const unsigned load : machine.cores)
                if (load != ~0u && load > busiest) busiest = load;
            std::snprintf(piece, sizeof(piece), " CPU %u%%", busiest);
            put(piece);
        } else if (by_core) {
            // Each core with its own sign, so each is coloured by its own load.
            put(" CPU");
            for (const unsigned load : machine.cores) {
                if (load != ~0u) std::snprintf(piece, sizeof(piece), " %u%%", load);
                else std::snprintf(piece, sizeof(piece), " --%%");
                put(piece);
            }
        } else {
            std::snprintf(piece, sizeof(piece), " CPU %s%%", cpu);
            put(piece);
        }
    }
    if (fields & kHudFieldGpu) {
        std::snprintf(piece, sizeof(piece), " GPU %s%%", gpu);
        put(piece);
    }
    if (fields & kHudFieldMem) {
        char memory[40] = "--/--";
        if (machine.memory_total != 0)
            // What is taken of the pool, then the pool: the order a player expects ("4.1 of
            // 8.0"), in the binary gigabytes the rest of the app counts the pool in.
            std::snprintf(memory, sizeof(memory), "%.1f/%.1fG",
                          static_cast<double>(machine.memory_total - std::min(machine.memory_free, machine.memory_total)) /
                              1073741824.0,
                          static_cast<double>(machine.memory_total) / 1073741824.0);
        std::snprintf(piece, sizeof(piece), " MEM %s", memory);
        put(piece);
        // The graphics' part of it: the one that grows as scenery loads.
        if (machine.graphics != 0) {
            std::snprintf(piece, sizeof(piece), " GFX %.1fG", static_cast<double>(machine.graphics) / 1073741824.0);
            put(piece);
        }
    }
    if ((fields & kHudFieldPing) && ping_ms >= 0) {
        std::snprintf(piece, sizeof(piece), " P%d", std::min(ping_ms, 9999));
        put(piece);
    }
    std::memcpy(text.data(), line, used);
    return text;
}
inline HudSnapshot MakeHudSnapshot(const HudClock& clock, const HudClock& output, double speed,
                                   const HudMachine& machine, double refresh = 0, int ping_ms = -1,
                                   int size = 0, int corner = 0, bool colors = false,
                                   uint32_t fields = kHudFieldAll) {
    const auto text = FormatHudText(clock, output, speed, "VLK", machine, refresh, ping_ms, fields);
    const std::string_view line{text.data()};
    // The chip's scale, from the size the launcher named: 1x, 1.5x or 2x. Every one of the
    // box's numbers is taken that many times over (vulkan_hud_draw.inc and DrawHud scale alike).
    const float scale = HudSizeScale(HudSizeField(size));
    HudSnapshot hud{};
    PutHudText(hud, line);
    // The box holds the line and a margin of kHudPadUnits either side of it.
    hud.width = static_cast<uint32_t>(
        std::lround(HudTextWidth(line, kHudCapUnits * scale) + 2 * kHudPadUnits * scale));
    const uint32_t box_h = static_cast<uint32_t>(std::lround(kHudBoxUnits * scale));
    // Against whichever corner was named, at the same distance in from each of its two edges; the
    // two coordinates say which, so nothing else has to (vulkan_hud_draw.inc, DrawHud).
    const bool right = corner == 1 || corner == 3, bottom = corner == 2 || corner == 3;
    hud.x = right ? kHudPictureWidth - hud.width - kHudBoxX : kHudBoxX;
    hud.y = bottom ? kHudPictureHeight - box_h - kHudBoxY : kHudBoxY;
    hud.loading = kHudOverlayFlag | HudSizeField(size) | (colors ? kHudColorsFlag : 0u);
    return hud;
}
// The OpenGL renderer has no frame generation: what it presents is the guest's own rate, so both
// numbers come from one clock. Callers that cannot have an output clock use this.
inline std::array<char, kHudTextMax + 1> FormatHudText(const HudClock& clock, double speed,
                                                       const char* backend, const HudMachine& machine,
                                                       int ping_ms = -1,
                                                       uint32_t fields = kHudFieldAll) {
    return FormatHudText(clock, clock, speed, backend, machine, 0, ping_ms, fields);
}
// Every frame handed to the present manager, the guest's own and each one frame generation adds,
// so the HUD's rate measures the output and not just the guest (graphics.cpp).
void CountPresentedFrame();
// Read on the renderer thread; the scheduler captures the returned value per frame.
HudSnapshot GetVulkanHud();
} // namespace Eden
