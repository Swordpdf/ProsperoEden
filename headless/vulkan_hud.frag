// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : enable
layout(push_constant) uniform Text { uint glyphs[28]; uint width; uint x; uint y; uint loading; } text;
// The loading screen's picture (loading_art.h); the overlay does not read it.
layout(set = 0, binding = 0) uniform sampler2D loading_art;
layout(location = 0) out vec4 color;
#include "loading_text.glsl"
#include "loading_scene.glsl"
void main() {
    // Vulkan push-constant arrays require dynamically uniform indices. Load
    // each word at a constant index before the per-pixel local-array lookup.
    uint glyphs[28] = uint[28](
        text.glyphs[0], text.glyphs[1], text.glyphs[2], text.glyphs[3],
        text.glyphs[4], text.glyphs[5], text.glyphs[6], text.glyphs[7],
        text.glyphs[8], text.glyphs[9], text.glyphs[10], text.glyphs[11],
        text.glyphs[12], text.glyphs[13], text.glyphs[14], text.glyphs[15],
        text.glyphs[16], text.glyphs[17], text.glyphs[18], text.glyphs[19],
        text.glyphs[20], text.glyphs[21], text.glyphs[22], text.glyphs[23],
        text.glyphs[24], text.glyphs[25], text.glyphs[26], text.glyphs[27]);
    // The loading screen owns the small numbers; the overlay marks its snapshot with a bit the
    // loading path never sets, so the two are told apart here (hud.h, kHudOverlayFlag).
    if (text.loading != 0u && (text.loading & 0x80000000u) == 0u) {
        // The loading screen: x and y carry the picture's size, loading the milliseconds since
        // it began (plus one), the glyphs' words what the start is doing (hud.h). Vulkan counts
        // rows from the top.
        vec2 size = vec2(float(text.x), float(text.y));
        uint state[24];
        for (int i = 0; i < 24; ++i) state[i] = glyphs[i];
        color = vec4(loading_scene(vec2(gl_FragCoord.x, size.y - gl_FragCoord.y), size,
                                   float(text.loading - 1u) * 0.001, state), 1.0);
        return;
    }
    // The overlay: x and y are its box's top left corner in this picture, width the picture's
    // height (vulkan_hud_draw.inc sets all three). Everything is laid out for a picture 1080
    // rows high; unit is how far one of those rows reaches here. The chip's scale comes from the
    // size field in `loading` (2, 3 or 4 for 1x, 1.5x or 2x; hud.h, HudSizeField), so every size
    // below is taken that many times over. The colour bit says the line is drawn in its inks.
    const float unit = float(text.width) / 1080.0;
    const float scale = float((text.loading >> 28) & 7u) * 0.5;
    const bool colors = (text.loading & 0x08000000u) != 0u;
    // Vulkan counts rows from the top, hud_pick (like loading_scene) from the bottom left.
    const vec2 pixel = vec2(gl_FragCoord.x, float(text.width) - gl_FragCoord.y);
    const vec2 at = vec2(float(text.x) + kHudPadUnits * unit * scale,
                         float(text.width) - (float(text.y) + kHudBaselineUnits * unit * scale));
    Pick pick = Pick(-1, 0, vec2(0.0), 1.0);
    hud_pick(pick, glyphs, pixel, at, kHudCapUnits * unit * scale);
    const float cover = pick_cover(pick);
    // The chip the line stands in, walked out here because Vulkan's push constants have no room
    // left to carry it: this pixel's place in the box, and the box's own size in this picture.
    const vec2 box = vec2(hud_line_width(glyphs, kHudCapUnits * unit * scale) +
                              2.0 * kHudPadUnits * unit * scale,
                          kHudBoxUnits * unit * scale);
    const vec4 panel = hud_panel(gl_FragCoord.xy - vec2(float(text.x), float(text.y)), box, colors);
    // The line in its inks when colours are on, in the one ink when they are off.
    const vec3 ink = colors ? hud_ink(pick.ink) : kHudInk;
    color = vec4(mix(panel.rgb, ink, cover), mix(panel.a, 1.0, cover));
}
