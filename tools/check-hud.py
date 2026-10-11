#!/usr/bin/env python3
"""FPS interval math, bounded bitmap text and startup-only queue polling."""
from pathlib import Path
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
assert 'vec4(0.025,0.04,0.075,0.5)' in graphics
assert 'glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)' in graphics
# As wide as its text, laid out for a picture 1080 rows high and scaled to the surface's.
assert 'const int text_width = int(std::strlen(text)) * 16;' in draw_hud
assert 'units(text_width + 24)' in draw_hud
assert 'static_cast<float>(height) / 1080.0f' in draw_hud

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'hud.cpp'
    binary = Path(directory) / 'hud'
    source.write_text(r'''#include "hud.h"
#include <cassert>
#include <cmath>
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
    assert(Eden::HudGlyph(' ') == 0 && Eden::HudGlyph('F') != 0 && Eden::HudGlyph('W') != 0);
    auto text = Eden::HudText("FPS 12.3");
    assert(text[0] == Eden::HudGlyph('F') && text[8] == 0);
    assert(Eden::HudText("01234567890123456789")[15] == Eden::HudGlyph('5'));
    assert(std::string_view(Eden::FormatHudText(clock, 100, "OGL").data()) == "OGL F60 S100 W17");
    assert(Eden::HudGlyph('V') != 0 && Eden::HudGlyph('K') != 0);
    auto snapshot = Eden::MakeHudSnapshot(clock, 100);
    assert(snapshot.glyphs == Eden::HudText("VLK F60 S100 W17"));
    assert(snapshot.width == std::string_view("VLK F60 S100 W17").size() * 16 + 24);
    // With frame generation the display is given more frames than the guest makes: both rates
    // are shown then, and the output's never above its refresh rate.
    Eden::HudClock guest = clock, shown = clock;
    guest.fps = 30;
    guest.worst_ms = 33;
    shown.fps = 60;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", 59.94).data()) == "VLK N30 F60 S100 W33");
    shown.fps = 120;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", 59.94).data()) == "VLK N30 F60 S100 W33");
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", 119.88).data()) == "VLK N30 F120 S100 W33");
    guest.fps = 60;
    guest.worst_ms = 17;
    assert(std::string_view(Eden::FormatHudText(guest, shown, 100, "VLK", 59.94).data()) == "VLK F60 S100 W17");
    assert(Eden::MakeHudSnapshot(guest, shown, 100, 119.88).glyphs == Eden::HudText("VLK N60 F120 S100 W17"));
    clock.fps = -1;
    assert(Eden::MakeHudSnapshot(clock, 0).glyphs == Eden::HudText("VLK F-- S-- W--"));
    clock.fps = 1000000;
    clock.worst_ms = 1000000;
    assert(Eden::MakeHudSnapshot(clock, 1000000).width <= 28 * 16 + 24);
    static_assert(sizeof(Eden::HudSnapshot) == 128);
    clock.fps = 60;
    clock.worst_ms = 17;
    for (const char* backend : {"OGL", "VLK"}) {
        const auto online = Eden::FormatHudText(clock, 100, backend, 42);
        assert(std::string_view(online.data()).ends_with(" W17 P42"));
        assert(std::string_view(Eden::FormatHudText(clock, 100, backend, 0).data()).ends_with(" P0"));
        assert(std::string_view(Eden::FormatHudText(clock, 100, backend, -1).data()).ends_with(" W17"));
    }
    const auto online = Eden::MakeHudSnapshot(guest, shown, 100, 119.88, 9999);
    assert(online.glyphs == Eden::HudText("VLK N60 F120 S100 W17 P9999"));
    assert(online.width == std::string_view("VLK N60 F120 S100 W17 P9999").size() * 16 + 24);
    clock.fps = clock.worst_ms = 1000000;
    const auto bounded = Eden::FormatHudText(clock, 1000000, "VLK", 1000000);
    assert(std::string_view(bounded.data()).size() <= 28);
    assert(std::string_view(bounded.data()).ends_with(" P9999"));
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
print('HUD interval math, startup exclusion and bounded glyph text PASS')
