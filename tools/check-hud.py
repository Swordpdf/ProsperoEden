#!/usr/bin/env python3
"""FPS interval math, bounded overlay text and startup-only queue polling."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
graphics = (root/'headless/graphics.cpp').read_text()
draw_hud = graphics.split('    void DrawHud(', 1)[1].split('    unsigned presented_frames', 1)[0]
assert 'glReadPixels' not in draw_hud
assert 'eglSwapBuffers' not in draw_hud
present_loading = graphics.split('    void PresentLoading()', 1)[1].split('    bool LoadingTick(bool idle)', 1)[0]
# The loading scene, or plain text when its shader does not build; then one swap.
assert (present_loading.index('DrawLoading(') < present_loading.index('DrawHud(text, true)')
        < present_loading.index('eglSwapBuffers'))
draw_loading = graphics.split('    bool DrawLoading(', 1)[1].split('    void DrawHud(', 1)[0]
assert 'eglSwapBuffers' not in draw_loading and 'loading_failed = true' in draw_loading
# The line is picked out of the panel behind it, in the one ink the overlay draws with.
assert 'hud_pick(pick, glyphs, gl_FragCoord.xy, at, cap)' in draw_hud
assert 'hud_panel(' in draw_hud and 'kHudInk' in draw_hud
assert 'vec3(0.0196,0.0392,0.0039)' in draw_hud  # loading stays a full screen of its own ink
assert 'glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)' in draw_hud
# The box covers only the line: its place at one top corner, as wide as the text, laid out for a
# picture 1080 rows high and scaled to the surface's. A large chip takes every number twice.
assert 'glViewport(box_x, height - box_y - box_h, box_w, box_h)' in draw_hud
assert 'HudTextWidth(text, kHudCapUnits * hud_scale) + 2 * kHudPadUnits * hud_scale' in draw_hud
assert 'units(kHudBoxUnits * hud_scale)' in draw_hud and 'units(kHudBaselineUnits * hud_scale)' in draw_hud
assert 'static_cast<float>(height) / 1080.0f' in draw_hud
assert all(name in draw_hud for name in ('kHudBoxX', 'kHudBoxY', 'kHudBoxUnits', 'kHudBaselineUnits'))
# The chip stands against whichever of the four corners was named, the same distance in from
# each of its two edges (Preferences::hud_corner, hud.h).
assert 'width - box_w - units(kHudBoxX)' in draw_hud
assert 'height - box_h - units(kHudBoxY)' in draw_hud
assert 'hud_size.load' in draw_hud and 'hud_corner.load' in draw_hud
assert 'hud_colors.load' in draw_hud and 'hud_fields.load' in graphics
# The line in its inks when colours are on, in the one ink when they are off.
assert 'colors ? hud_ink(pick.ink) : kHudInk' in draw_hud
# The Vulkan overlay draws that same line with that same font (vulkan_hud.frag, which the build
# compiles to SPIR-V, so a mistake there is a build failure).
frag = (root/'headless/vulkan_hud.frag').read_text()
assert 'hud_pick(pick, glyphs, pixel, at,' in frag and 'pick_cover(pick)' in frag
assert 'hud_panel(' in frag and 'kHudInk' in frag
# The chip's width is walked out here rather than pushed: Vulkan's push constants are all used up
# by the line itself, so the box has to agree with the C++ one by construction.
assert 'hud_line_width(glyphs,' in frag
assert all(name in frag for name in ('kHudCapUnits', 'kHudPadUnits', 'kHudBaselineUnits'))
assert 'float(text.width) - gl_FragCoord.y' in frag  # Vulkan counts rows from the top
assert 'float((text.loading >> 28) & 7u) * 0.5' in frag  # the chip's scale, from the size field
# The overlay's snapshot marks itself in the field the loading screen owns, and the two are told
# apart by a bit the loading path never sets. The shader and the draw path carry the literals,
# so they have to be the ones hud.h defines.
hud_h = (root/'headless/hud.h').read_text()
draw = (root/'headless/vulkan_hud_draw.inc').read_text()
assert 'kHudOverlayFlag = 0x80000000u' in hud_h and '0x80000000u' in frag
# The chip's size is the scale times two in three bits (2 = 1x, 3 = 1.5x, 4 = 2x), and the
# colour bit says the line is drawn in its inks; both ride in the field the loading screen owns.
assert 'kHudSizeShift = 28' in hud_h and 'kHudSizeMask = 0x70000000u' in hud_h
assert 'kHudColorsFlag = 0x08000000u' in hud_h and '0x08000000u' in frag
assert 'kHudSizeMask' in draw and 'kHudSizeShift' in draw  # the box's height from that field
assert 'Eden::HudLoading(hud)' in draw
# C++ and GLSL size the box from the same generated numbers, so the box never drifts apart. The
# digit advance is among them: the C++ width and the shader's own walk over the line have to land
# on the same edge, or the last figure would stand outside the chip.
header = (root/'headless/hud_text_advances.h').read_text()
for name in ('kHudCapUnits', 'kHudPadUnits', 'kHudBoxUnits', 'kHudBaselineUnits', 'kHudDigitAdvance'):
    value = re.search(rf'const float {name} = ([0-9.]+);', (root/'headless/loading_text.glsl').read_text()).group(1)
    assert re.search(rf'inline constexpr float {name} = {value}f;', header), name

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'hud.cpp'
    binary = Path(directory) / 'hud'
    source.write_text(r'''#include "hud.h"
#include <cassert>
#include <cmath>
#include <string>
int main() {
    Eden::HudClock clock;
    clock.Present(80.0); // Long startup must not count as a slow game frame.
    assert(clock.fps < 0);
    clock.Present(80.5);
    clock.Present(81.0);
    assert(clock.fps == 2.0);
    clock.Present(83.0);
    assert(clock.fps < 0); // A scene-loading gap is not a one-frame FPS sample.
    for (int i=1;i<=60;++i) clock.Present(83.0+i/60.0);
    assert(std::abs(clock.fps-60.0)<0.001);
    assert(std::abs(clock.worst_ms-1000.0/60.0)<0.001);
    clock.Present(84.1);
    assert(clock.window_worst_ms >= 99.9);
    Eden::StartupGate gate;
    assert(!gate.Ready(0.0));
    assert(!gate.Ready(0.03));
    assert(!gate.Ready(3.0)); // A startup stall restarts stabilization.
    for (int i=1;i<8;++i) assert(!gate.Ready(3.0+i/30.0));
    assert(gate.Ready(3.0+8.0/30.0));
    Eden::StartupGate slow;
    for (int i = 0; i < 5; ++i) assert(!slow.Ready(i));
    assert(slow.Ready(5)); // Slow rendering must not remain hidden forever.

    // The line is the font's own characters: four to a word, and a zero after it.
    Eden::HudSnapshot packed{};
    Eden::PutHudText(packed, "VLK 60FPS");
    assert(std::string_view(reinterpret_cast<const char*>(packed.glyphs.data())) == "VLK 60FPS");
    Eden::PutHudText(packed, "");
    assert(std::string_view(reinterpret_cast<const char*>(packed.glyphs.data())).empty());
    Eden::HudSnapshot full{};
    Eden::PutHudText(full, std::string(Eden::kHudTextMax + 40, 'A'));
    assert(std::string_view(reinterpret_cast<const char*>(full.glyphs.data())).size() ==
           Eden::kHudTextMax);
    static_assert(sizeof(Eden::HudSnapshot) == 128); // Vulkan's guaranteed push-constant bytes.

    // The box the line sits in, and the text's baseline in it, come from one set of numbers.
    assert(Eden::kHudBaselineUnits == Eden::kHudPadUnits + Eden::kHudCapUnits);
    assert(Eden::kHudBoxUnits == Eden::kHudCapUnits + 2 * Eden::kHudPadUnits);

    // What the machine could not say is shown as "--", until it can say it.
    Eden::HudMachine unknown;
    assert(unknown.cpu == ~0u && unknown.gpu == ~0u && unknown.memory_total == 0);
    clock.fps = 60;
    clock.worst_ms = 17;
    assert(std::string_view(Eden::FormatHudText(clock, 100, "OGL", unknown).data())
           == "OGL 60FPS 17.0MS CPU --% GPU --% MEM --/--");
    Eden::HudMachine known;
    known.cpu = 55;
    known.gpu = 43;
    known.memory_free = 4187593114ULL;   // 4.1 GiB of the 8.0 are taken
    known.memory_total = 8589934592ULL;
    // The four emulated cores take the place of the one share when they are known.
    {
        Eden::HudMachine by_core = known;
        by_core.cores[0] = 92; by_core.cores[1] = 45; by_core.cores[2] = 38; by_core.cores[3] = 3;
        assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", by_core).data())
               == "VLK 60FPS 17.0MS CPU 92% 45% 38% 3% GPU 43% MEM 4.1/8.0G");
        // With that switch off, only the busiest of them.
        assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", by_core, -1,
                                                    Eden::kHudFieldAll | Eden::kHudFieldOneCore).data())
               == "VLK 60FPS 17.0MS CPU 92% GPU 43% MEM 4.1/8.0G");
        by_core.cores[3] = ~0u;
        assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", by_core).data())
               == "VLK 60FPS 17.0MS CPU 92% 45% 38% --% GPU 43% MEM 4.1/8.0G");
    }
    // The graphics' part follows the pool when it is known.
    {
        Eden::HudMachine with_graphics = known;
        with_graphics.graphics = 2684354560ULL;  // 2.5 GiB
        assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", with_graphics).data())
               == "VLK 60FPS 17.0MS CPU 55% GPU 43% MEM 4.1/8.0G GFX 2.5G");
    }
    // A full 100% speed is not worth saying; anything else is.
    assert(std::string_view(Eden::FormatHudText(clock, 98, "VLK", known).data())
           == "VLK 60FPS 17.0MS 98% CPU 55% GPU 43% MEM 4.1/8.0G");
    assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", known).data())
           == "VLK 60FPS 17.0MS CPU 55% GPU 43% MEM 4.1/8.0G");
    clock.fps = -1;
    assert(std::string_view(Eden::FormatHudText(clock, 100, "OGL", known).data())
           == "OGL --FPS --MS CPU 55% GPU 43% MEM 4.1/8.0G");
    clock.fps = 60;
    assert(std::string_view(Eden::FormatHudText(clock, 100, "OGL", known, 42).data())
           == "OGL 60FPS 17.0MS CPU 55% GPU 43% MEM 4.1/8.0G P42");
    assert(std::string_view(Eden::FormatHudText(clock, 100, "OGL", known, -1).data())
           == "OGL 60FPS 17.0MS CPU 55% GPU 43% MEM 4.1/8.0G");

    // With frame generation the display is given frames the guest never made: both rates are
    // shown then, and the display's is never above its own refresh rate.
    Eden::HudClock guest = clock, shown = clock;
    guest.fps = 30;
    guest.worst_ms = 33;
    shown.fps = 60;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", unknown, 59.94).data())
           == "VLK 30/60FPS 33.0MS CPU --% GPU --% MEM --/--");
    shown.fps = 120;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", unknown, 59.94).data())
           == "VLK 30/60FPS 33.0MS CPU --% GPU --% MEM --/--");
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", unknown, 119.88).data())
           == "VLK 30/120FPS 33.0MS CPU --% GPU --% MEM --/--");
    guest.fps = 60;
    guest.worst_ms = 17;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", unknown, 59.94).data())
           == "VLK 60FPS 17.0MS CPU --% GPU --% MEM --/--");

    // The box is as wide as that line needs, with a margin either side, at the corner it sits in.
    auto snapshot = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999);
    const std::string_view line{reinterpret_cast<const char*>(snapshot.glyphs.data())};
    assert(line == "VLK 60/120FPS 17.0MS CPU 55% GPU 43% MEM 4.1/8.0G P9999");
    assert(snapshot.x == Eden::kHudBoxX && snapshot.y == Eden::kHudBoxY);
    assert(snapshot.width == static_cast<uint32_t>(std::lround(
               Eden::HudTextWidth(line, Eden::kHudCapUnits) + 2 * Eden::kHudPadUnits)));
    assert(snapshot.width < 1920); // One line, in a picture 1080 rows high.
    // The chip's size: regular 1x, medium 1.5x, large 2x, stored as the scale times two in bits
    // 28-30 (hud.h, HudSizeField). A large chip is twice every one of the box's numbers.
    const auto regular = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999, 0, 0);
    const auto medium = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999, 1, 0);
    const auto large = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999, 2, 0);
    assert(((regular.loading & Eden::kHudSizeMask) >> Eden::kHudSizeShift) == 2);
    assert(((medium.loading & Eden::kHudSizeMask) >> Eden::kHudSizeShift) == 3);
    assert(((large.loading & Eden::kHudSizeMask) >> Eden::kHudSizeShift) == 4);
    assert(Eden::HudSizeScale(regular.loading) == 1.0f);
    assert(Eden::HudSizeScale(medium.loading) == 1.5f);
    assert(Eden::HudSizeScale(large.loading) == 2.0f);
    assert(large.width == static_cast<uint32_t>(std::lround(
               Eden::HudTextWidth(line, Eden::kHudCapUnits * 2.0f) + 2 * Eden::kHudPadUnits * 2.0f)));
    assert(large.x == Eden::kHudBoxX && large.y == Eden::kHudBoxY);
    assert((large.loading & Eden::kHudOverlayFlag) != 0);
    assert(!Eden::HudLoading(large)); // the overlay is never mistaken for the loading screen
    // The colour bit, and the pieces of the line the user may turn off.
    const auto coloured = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999, 0, 0, true);
    assert((coloured.loading & Eden::kHudColorsFlag) != 0);
    assert((snapshot.loading & Eden::kHudColorsFlag) == 0); // off by default
    // Only the pieces the mask names reach the line; the rest are dropped, and their separators
    // with them, so a kept piece never opens with a stray space.
    assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", known, -1,
                                                Eden::kHudFieldBackend | Eden::kHudFieldFps |
                                                    Eden::kHudFieldMem).data())
           == "VLK 60FPS MEM 4.1/8.0G");
    assert(std::string_view(Eden::FormatHudText(clock, 100, "VLK", known, -1, 0).data()).empty());
    // All four corners: each the same distance in from the two edges it stands against.
    for (int corner = 0; corner < Eden::kHudCornerCount; ++corner)
    {
        const auto placed = Eden::MakeHudSnapshot(guest, shown, 100, known, 119.88, 9999, 0, corner);
        const uint32_t box = static_cast<uint32_t>(std::lround(Eden::kHudBoxUnits));
        const bool right = corner == 1 || corner == 3, bottom = corner == 2 || corner == 3;
        assert(placed.x == (right ? Eden::kHudPictureWidth - placed.width - Eden::kHudBoxX
                                  : Eden::kHudBoxX));
        assert(placed.y == (bottom ? Eden::kHudPictureHeight - box - Eden::kHudBoxY : Eden::kHudBoxY));
        assert(((placed.loading & Eden::kHudSizeMask) >> Eden::kHudSizeShift) == 2 && !Eden::HudLoading(placed));
    }
    assert(Eden::HudLoading(Eden::MakeLoadingSnapshot(0)));
    // Nothing it can say reaches past what the words of a snapshot hold.
    clock.fps = 1000000;
    clock.worst_ms = 1000000;
    const auto bounded = Eden::MakeHudSnapshot(clock, clock, 1000000, known, 0, 1000000);
    assert(std::string_view(reinterpret_cast<const char*>(bounded.glyphs.data())).size()
           <= Eden::kHudTextMax);
    assert(bounded.width < 1920);

    // The loading scene is drawn from the time alone: milliseconds since it began, plus one.
    assert(Eden::MakeLoadingSnapshot(0).loading == 1 && Eden::MakeLoadingSnapshot(0).width != 0);
    assert(Eden::MakeLoadingSnapshot(2.5).loading == 2501);
    assert(Eden::MakeLoadingSnapshot(-1).loading == 1);
    // A picture per display refresh while the GPU thread is idle, ten a second while it works.
    Eden::LoadingPace pace;
    assert(pace.Due(10.0, false) && !pace.Due(10.05, false) && pace.Due(10.11, false));
    assert(!pace.Due(10.12, true) && pace.Due(10.13, true));
    int pictures = 0;
    for (int ms = 0; ms < 1000; ++ms) pictures += pace.Due(11.0 + ms / 1000.0, true);
    assert(pictures >= 59 && pictures <= 61);
}
''')
    subprocess.run(['c++', '-std=c++20', '-I'+str(root/'headless'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('HUD interval math, startup exclusion and bounded overlay line PASS')
