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
    if (text.loading != 0u) {
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
    // The overlay: x and y are where its text starts, width the picture's height. The text is
    // laid out for a picture 1080 rows high (a glyph's cell is 4 pixels there).
    ivec2 p = ivec2(gl_FragCoord.xy) - ivec2(text.x, text.y);
    float cell_size = 4.0 * float(text.width) / 1080.0;
    bool ink = false;
    if (p.x >= 0 && p.y >= 0) {
        ivec2 cell = ivec2(vec2(p) / cell_size);
        int i = cell.x / 4;
        int x = cell.x % 4;
        if (i < 28 && x < 3 && cell.y < 5)
            ink = ((glyphs[i] >> uint((4 - cell.y) * 3 + 2 - x)) & 1u) != 0u;
    }
    color = ink ? vec4(0.9, 0.95, 1.0, 1.0) : vec4(0.025, 0.04, 0.075, 0.5);
}
