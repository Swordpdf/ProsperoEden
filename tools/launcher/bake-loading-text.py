#!/usr/bin/env python3
# ProsperoEden - Bake the loading screen's lettering into GLSL distance fields.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes headless/loading_text.glsl: capitals, digits and a few signs of Montserrat Medium as
small signed distance fields packed into constant arrays (one per glyph), the loading screen's lines of text
as indexes into them, and the routines that read the fields and walk a line of characters over them
(Pick, pick_glyph, pick_cover, hud_pick). The loading shader needs no texture and no font at run time, and
the overlay's line (hud.h, FormatHudText) reaches that same shader as characters, looked up in the
same table.

Writes headless/hud_text_advances.h beside it: the same advances in C++, for sizing the overlay's box
to its text before any of it is drawn.

A glyph's field is WIDTH x HEIGHT texels of four bits each (eight per uint, lowest bits first, top
row first): 7.5 on the outline, 15 SPREAD texels inside, 0 SPREAD texels outside. The fields are
kept this small on purpose: a console takes about a millisecond per word to compile them. The glyph stands on
a baseline BASE texels above the field's bottom, LEFT texels from its left edge, with capitals CAP
texels high.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / "third_party/fonts/Montserrat-Medium.ttf"
OUTPUT = ROOT / "headless/loading_text.glsl"
# The same advances in C++, so the overlay's box can be sized to its text before it is drawn
# (hud.h, HudTextWidth).
ADVANCES_OUTPUT = ROOT / "headless/hud_text_advances.h"

# The lines the screen shows, in the order the shader names them (loading_scene.glsl).
LINES = ["PROSPEROEDEN", "READING THE GAME", "STARTING GRAPHICS", "BUILDING SHADERS", "STARTING THE GAME",
         "LOADING", "S"]
# What those lines use, plus everything the overlay's line can say: the OGL/VLK backend,
# "27/60FPS 37.0MS CPU 55% GPU 43% MEM 4.1/8.0G" and a ping (hud.h, FormatHudText). One set:
# the loading shader and the overlay share one table, and one shader (vulkan_hud.frag) draws both.
GLYPHS = "".join(sorted(set("".join(LINES)) - {" "})) + "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.%/:-"
GLYPHS = "".join(sorted(set(GLYPHS)))
WIDTH, HEIGHT = 24, 20    # texels of one glyph's field
CAP, BASE, LEFT = 12, 4, 3
SPREAD = 2.0              # texels of distance stored each side of the outline
SCALE = 8                 # a glyph is drawn this much larger, then measured
# How far the pen moves for a space, which GLYPHS does not hold: what loading_scene.glsl's
# pen_step() gives a glyph index of -1.
SPACE_ADVANCE = 0.30 * CAP / 0.70
# The overlay's box in that picture: how tall a capital is in it, and the margin round the text.
# The height of one line of text, and so of the box, is CAP_UNITS + 2 * PAD_UNITS, and the
# baseline stands CAP_UNITS + PAD_UNITS below its top. hud.h sizes the box with these too.
CAP_UNITS, PAD_UNITS = 14.0, 3.0
# The chip that box draws (hud_panel): how far its corners are cut round, in the same picture as
# the rest of it, so they scale with it.
RADIUS_UNITS = 5.0
# The accent bar down that chip's left edge, when the line is drawn in its inks (hud_panel), in
# that same picture.
ACCENT_UNITS = 1.5
# The overlay's line, in characters (hud.h, kHudTextMax): four go to a word, and the line ends
# at a NUL, so this is what hud_pick stops at.
TEXT_MAX = 111

# The reading of a glyph's field, and the laying out of a line over it. The lettering is read in
# one place only, at the end of each shader that draws it: the console's compiler copes badly
# with the glyph tables being read from many, so the text is laid out first, which only decides
# which glyph, if any, this pixel falls in (a Pick), and what ink it gets.
HELPERS = """
float glyph_texel(int glyph, int x, int y)
{
    x = clamp(x, 0, kGlyphWidth - 1);
    y = clamp(y, 0, kGlyphHeight - 1);
    int at = y * kGlyphWidth + x;
    return float((glyph_word(glyph, at >> 3) >> uint((at & 7) * 4)) & 0xfu) / 15.0;
}

struct Pick
{
    int glyph;   // -1: no text here
    int ink;
    vec2 st;     // where in the glyph's field, in texels
    float texel; // pixels per texel
};

float pen_step(int glyph, float cap, float tracking)
{
    return (glyph < 0 ? 0.30 * kGlyphCap / 0.70 : kAdvance[glyph]) * cap / kGlyphCap + tracking * cap;
}

// Takes the pixel for this glyph when it lies between the glyph's pen and the next one, at a pen
// step the caller has already worked out.
// at is where the pen stands (left end of the baseline), cap the height of a capital in pixels.
void pick_glyph_step(inout Pick pick, int glyph, int ink, vec2 pixel, vec2 at, float step, float cap, float tracking)
{
    float texel = cap / kGlyphCap;
    if (pixel.x < at.x - 0.5 * tracking * cap || pixel.x >= at.x + step - 0.5 * tracking * cap)
        return;
    pick.glyph = glyph;
    pick.ink = ink;
    pick.st = vec2((pixel.x - at.x) / texel + kGlyphLeft,
                   float(kGlyphHeight) - ((pixel.y - at.y) / texel + kGlyphBase)) - 0.5;
    pick.texel = texel;
}

void pick_glyph(inout Pick pick, int glyph, int ink, vec2 pixel, vec2 at, float cap, float tracking)
{
    pick_glyph_step(pick, glyph, ink, pixel, at, pen_step(glyph, cap, tracking), cap, tracking);
}

// How much of that glyph covers this pixel, 0 where there is none: the field sampled at the
// pixel's place in it, its distance pushed across the outline at this size.
float pick_cover(Pick pick)
{
    if (pick.glyph < 0 || pick.st.x <= -1.0 || pick.st.y <= -1.0 ||
        pick.st.x >= float(kGlyphWidth) || pick.st.y >= float(kGlyphHeight))
        return 0.0;
    ivec2 i = ivec2(floor(pick.st));
    vec2 f = pick.st - vec2(i);
    float value = mix(mix(glyph_texel(pick.glyph, i.x, i.y), glyph_texel(pick.glyph, i.x + 1, i.y), f.x),
                      mix(glyph_texel(pick.glyph, i.x, i.y + 1), glyph_texel(pick.glyph, i.x + 1, i.y + 1), f.x),
                      f.y);
    return clamp((value - 0.5) * 2.0 * kGlyphSpread * pick.texel + 0.5, 0.0, 1.0);
}

// One character of the overlay's line (hud.h, HudSnapshot): four go to a word, so this is one
// of them.
int hud_byte(uint words[28], int at)
{
    return int((words[at >> 2] >> uint((at & 3) * 8)) & 0xffu);
}

// The glyph for a character of that line, -1 where the font has none (drawn as a space).
int hud_glyph(int code)
{
    return code >= 32 && code < 127 ? kCharIndex[code - 32] : -1;
}

// The one ink the overlay's line is drawn in when colours are off. It draws over whatever the
// game shows, and a single colour reads against all of them; the loading screen's own ink is its
// palette's (loading_scene.glsl), and the pick's ink field is left to it.
const vec3 kHudInk = vec3(0.93, 0.96, 1.0);
// The inks the line is drawn in when colours are on: labels dim, values bright, and the loads
// coloured by how far they run (hud_number_ink decides which of the three).
const int kHudInkLabel = 0;
const int kHudInkValue = 1;
const int kHudInkGood = 2;
const int kHudInkWarn = 3;
const int kHudInkBad = 4;
// One of those inks, as a colour.
vec3 hud_ink(int ink)
{
    if (ink == kHudInkLabel) return vec3(0.55, 0.62, 0.73);
    if (ink == kHudInkGood) return vec3(0.42, 0.87, 0.58);
    if (ink == kHudInkWarn) return vec3(0.97, 0.78, 0.33);
    if (ink == kHudInkBad) return vec3(0.96, 0.45, 0.42);
    return vec3(0.93, 0.96, 1.0); // kHudInkValue
}
// The characters the line is read by, as the bytes hud_byte returns them (ASCII). GLSL in this
// build has no character literals, so they are named here.
const int kAsciiZero = 48, kAsciiNine = 57;
const int kAsciiDot = 46, kAsciiSlash = 47, kAsciiPercent = 37;
const int kAsciiF = 70, kAsciiM = 77, kAsciiU = 85, kAsciiSpace = 32;
// The ink a run of digits takes, by the sign that ends it and the number before it: the rate the
// guest makes (F; of "30/60FPS" the first number), the worst frame's length (M), and a percentage.
// A percentage is a load after CPU, GPU or another load, where less is better; anywhere else it
// is the game's speed, where more is. A run with no sign after it — memory, a ping — is simply a
// value. The thresholds are the ones the launcher settled on.
int hud_number_ink(uint words[28], int at)
{
    // Walk past the digits and any '.' or '/' among them, to the sign that ends the run.
    float value = 0.0, frac = 0.0, scale = 0.1;
    bool point = false, have = false, second = false;
    int end = at;
    while (end < kHudTextMax)
    {
        int code = hud_byte(words, end);
        int digit = code - kAsciiZero;
        if (digit >= 0 && digit <= 9)
        {
            if (second) { }
            else if (point) { frac += float(digit) * scale; scale *= 0.1; }
            else value = value * 10.0 + float(digit);
            have = true;
        }
        else if (code == kAsciiDot && have) point = true;
        else if (code == kAsciiSlash && have) second = true; // the number after it is not the one judged
        else break;
        ++end;
    }
    if (!have) return kHudInkValue;
    int sign = end < kHudTextMax ? hud_byte(words, end) : 0;
    if (sign == kAsciiF)
        return value >= 55.0 ? kHudInkGood : value >= 30.0 ? kHudInkWarn : kHudInkBad;
    if (sign == kAsciiM)
        return value <= 18.5 ? kHudInkGood : value <= 33.5 ? kHudInkWarn : kHudInkBad;
    if (sign == kAsciiPercent)
    {
        int before = at >= 2 && hud_byte(words, at - 1) == kAsciiSpace ? hud_byte(words, at - 2) : 0;
        if (before != kAsciiU && before != kAsciiPercent)
            return value >= 95.0 ? kHudInkGood : value >= 80.0 ? kHudInkWarn : kHudInkBad; // the speed
        return value <= 50.0 ? kHudInkGood : value <= 80.0 ? kHudInkWarn : kHudInkBad;
    }
    return kHudInkValue;
}

// How far the pen moves in the overlay's line: every digit takes the one width, so a number that
// changes does not move what stands after it. Montserrat's own digits are proportional, which is
// right for a word and wrong for a figure that is rewritten several times a second.
float hud_step(int glyph, float cap)
{
    float advance = glyph >= kDigit && glyph <= kDigit + 9 ? kHudDigitAdvance
                    : glyph < 0 ? 0.30 * kGlyphCap / 0.70 : kAdvance[glyph];
    return advance * cap / kGlyphCap;
}

// How wide the whole line is, which is the chip's own width less its margin: hud.h sizes the box
// the same way, in C++, before any of this is drawn.
float hud_line_width(uint words[28], float cap)
{
    float width = 0.0;
    for (int i = 0; i < kHudTextMax; ++i)
    {
        int code = hud_byte(words, i);
        if (code == 0) break;
        width += hud_step(hud_glyph(code), cap);
    }
    return width;
}

// The chip the line stands in: a rounded panel, quiet enough to read the picture through, and in
// no colour of its own. When colours are on it takes an accent bar down its left edge. local is
// the pixel's place in the box, counted down and right from its top left corner; size is the
// box's own size in pixels.
vec4 hud_panel(vec2 local, vec2 size, bool colors)
{
    float unit = size.y / kHudBoxUnits;
    float radius = kHudRadiusUnits * unit;
    vec2 half_size = 0.5 * size;
    vec2 q = abs(local - half_size) - half_size + radius;
    float edge = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
    float inside = 1.0 - smoothstep(-0.75, 0.75, edge);
    vec4 panel = vec4(vec3(0.05, 0.06, 0.095), 0.78 * inside);
    if (colors && inside > 0.0)
    {
        // An accent bar down the left edge, the width of kHudAccentUnits, only where the panel
        // itself stands (the same rounded edge `inside` is cut from).
        float accent = kHudAccentUnits * unit;
        float bar = (1.0 - smoothstep(accent - 0.75, accent + 0.75, local.x)) * inside;
        panel = vec4(mix(panel.rgb, vec3(0.663, 0.859, 0.388), bar), mix(panel.a, 0.95, bar));
    }
    return panel;
}

// The line with the left end of its baseline at `at`, at a cap of `cap` pixels; pixel has its
// origin at the bottom left, as loading_scene's does. The line ends at its first NUL, and no
// glyph standing past this pixel can take it, so both end the walk early. Each glyph's ink is
// worked out here (labels dim, the numbers about them by the sign they end in) and left on the
// pick; whether the line is drawn in those inks or one is the caller's, not this one's.
void hud_pick(inout Pick pick, uint words[28], vec2 pixel, vec2 at, float cap)
{
    if (pixel.y < at.y - 0.5 * cap || pixel.y > at.y + 1.5 * cap || pixel.x < at.x - cap)
        return;
    float pen = at.x;
    int ink = kHudInkLabel;   // what the next glyph is drawn in
    int number = kHudInkValue; // the ink of the run of digits under the pen
    bool inside = false;      // between a digit and the sign that ends it
    for (int i = 0; i < kHudTextMax; ++i)
    {
        if (pen > pixel.x) break;
        int code = hud_byte(words, i);
        if (code == 0) break;
        int glyph = hud_glyph(code);
        bool digit = code >= kAsciiZero && code <= kAsciiNine;
        if (!inside && digit)
        {
            // A run of digits begins: its ink is the sign that ends it and the number itself.
            number = hud_number_ink(words, i);
            inside = true;
            ink = number;
        }
        else if (inside)
        {
            if (digit || code == kAsciiDot || code == kAsciiSlash)
                ink = number; // still within the number
            else
            {
                // The sign that ends it: a percent keeps the number's ink, a unit letter does not.
                inside = false;
                ink = code == kAsciiPercent ? number : kHudInkLabel;
            }
        }
        if (glyph >= 0)
        {
            // A digit stands in the middle of the width every digit takes: at its left edge a
            // narrow one (the 1) left a gap after it that read as a space ("1 0%").
            float inset = digit ? 0.5 * (hud_step(glyph, cap) - kAdvance[glyph] * cap / kGlyphCap) : 0.0;
            pick_glyph_step(pick, glyph, ink, pixel, vec2(pen + inset, at.y), hud_step(glyph, cap), cap, 0.0);
        }
        pen += hud_step(glyph, cap);
    }
}
"""


def draw_glyph(font, letter):
    """The glyph as a boolean mask SCALE times the field's size."""
    image = Image.new("L", (WIDTH * SCALE, HEIGHT * SCALE))
    ImageDraw.Draw(image).text((LEFT * SCALE, (HEIGHT - BASE) * SCALE), letter, font=font, fill=255, anchor="ls")
    mask = np.asarray(image) >= 128
    assert not mask[:, -SCALE:].any() and not mask[:SCALE].any(), f"{letter!r} needs a larger field"
    return mask


def distance_field(mask):
    """Signed distance to the outline in field texels, sampled at texel centres."""
    inside = np.argwhere(mask).astype(np.float32)
    outside = np.argwhere(~mask).astype(np.float32)
    ys = (np.arange(HEIGHT) + 0.5) * SCALE - 0.5
    xs = (np.arange(WIDTH) + 0.5) * SCALE - 0.5
    field = np.zeros((HEIGHT, WIDTH), np.float32)
    # Only pixels within the spread matter; the rest clamp to the ends of the range.
    step = 2  # every second pixel of the mask is near enough for a field this coarse
    inside, outside = inside[::step], outside[::step]
    for row, y in enumerate(ys):
        points = np.stack([np.full(WIDTH, y, np.float32), xs.astype(np.float32)], axis=1)
        to_inside = np.sqrt(((points[:, None, :] - inside[None, :, :]) ** 2).sum(-1)).min(1) if len(inside) else np.full(WIDTH, 1e9)
        to_outside = np.sqrt(((points[:, None, :] - outside[None, :, :]) ** 2).sum(-1)).min(1)
        is_inside = mask[int(round(y)), np.round(xs).astype(int)]
        field[row] = np.where(is_inside, to_outside, -to_inside) / SCALE
    return field


def main():
    font = ImageFont.truetype(str(FONT), round(CAP * SCALE / 0.70))  # Montserrat capitals are 0.70 em
    data, advances = [], []
    for letter in GLYPHS:
        field = distance_field(draw_glyph(font, letter))
        data.append(np.clip(np.round(7.5 + field / SPREAD * 7.5), 0, 15).astype(np.uint32).reshape(-1))
        advances.append(font.getlength(letter) / SCALE)
    # One table per glyph: a console compiles a shader with one large table very slowly (its time grows
    # much faster than the table), and several small ones quickly.
    tables = [sum(d[shift::8] << (4 * shift) for shift in range(8)) for d in data]
    codes, starts = [], []
    for line in LINES:
        starts.append((len(codes), len(line)))
        codes += [GLYPHS.index(letter) if letter != " " else -1 for letter in line]
    lines = [
        "// Generated by tools/launcher/bake-loading-text.py from Montserrat Medium (SIL Open Font",
        "// License 1.1). Do not edit: change the script and run it again.",
        f"const int kGlyphWidth = {WIDTH};",
        f"const int kGlyphHeight = {HEIGHT};",
        f"const float kGlyphCap = {CAP:.1f};",
        f"const float kGlyphBase = {BASE:.1f};",
        f"const float kGlyphLeft = {LEFT:.1f};",
        f"const float kGlyphSpread = {SPREAD:.1f};",
        "// The overlay's box in a picture 1080 rows high: its cap height, the margin around the",
        "// text in it, its whole height and where its baseline stands from its top. hud.h and",
        "// vulkan_hud_draw.inc size and place the box with these; all four are here so they",
        "// cannot drift.",
        f"const float kHudCapUnits = {CAP_UNITS:.1f};",
        f"const float kHudPadUnits = {PAD_UNITS:.1f};",
        f"const float kHudBoxUnits = {CAP_UNITS + 2 * PAD_UNITS:.1f};",
        f"const float kHudBaselineUnits = {PAD_UNITS + CAP_UNITS:.1f};",
        f"// The chip the line stands in (hud_panel), in that same picture: how far its corners are",
        f"// cut round, and how wide its accent bar is when colours are on.",
        f"const float kHudRadiusUnits = {RADIUS_UNITS:.1f};",
        f"const float kHudAccentUnits = {ACCENT_UNITS:.1f};",
        f"// The overlay's line, in characters (hud.h, kHudTextMax).",
        f"const int kHudTextMax = {TEXT_MAX};",
        f"const int kDigit = {GLYPHS.index('0')};",
        f"const int kPercent = {GLYPHS.index('%')};",
        f"const int kSlash = {GLYPHS.index('/')};",
        "// The widest digit, which every figure in the overlay's line takes (hud_step).",
        f"const float kHudDigitAdvance = {max(advances[GLYPHS.index(d)] for d in '0123456789'):.2f};",
        # The overlay's line reaches the shader as characters, not as kText's indexes.
        "// ASCII 32 to 126 as a glyph index, -1 where the font has no glyph (drawn as a space).",
        f"const int kCharIndex[{127 - 32}] = int[{127 - 32}](",
        "    " + ", ".join(str(GLYPHS.index(chr(code)) if chr(code) in GLYPHS else -1)
                          for code in range(32, 127)) + ");",
        "// How far each glyph moves the pen, in texels.",
        f"const float kAdvance[{len(advances)}] = float[{len(advances)}](",
        "    " + ", ".join(f"{value:.2f}" for value in advances) + ");",
        "// The lines of text: glyph indexes (-1 is a space), and where each line starts and how long it is.",
        f"const int kText[{len(codes)}] = int[{len(codes)}](",
        "    " + ", ".join(str(code) for code in codes) + ");",
        f"const ivec2 kLine[{len(starts)}] = ivec2[{len(starts)}](",
        "    " + ", ".join(f"ivec2({start}, {count})" for start, count in starts) + ");",
    ]
    for index, words in enumerate(tables):
        lines.append(f"const uint kGlyph{index}[{words.size}] = uint[{words.size}](")
        for start in range(0, words.size, 8):
            chunk = ", ".join(f"0x{value:08x}u" for value in words[start:start + 8])
            lines.append("    " + chunk + ("," if start + 8 < words.size else ""))
        lines.append(");")
    lines += ["// One word of a glyph's field.", "uint glyph_word(int glyph, int word)", "{", "    switch (glyph)", "    {"]
    lines += [f"    case {index}: return kGlyph{index}[word];" for index in range(len(tables))]
    lines += ["    }", "    return 0u;", "}"]
    # How a glyph's field is read and a line laid out over it, which both shaders need.
    lines += HELPERS.splitlines()
    words = np.concatenate(tables)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT.name}: {len(GLYPHS)} glyphs of {WIDTH}x{HEIGHT} texels, {words.size} words, {len(LINES)} lines")
    # The same advances for C++, which has to know how wide the line is before anything draws it.
    every = [advances[GLYPHS.index(chr(code))] if chr(code) in GLYPHS else SPACE_ADVANCE
             for code in range(32, 127)]
    digit_advance = max(advances[GLYPHS.index(d)] for d in "0123456789")
    columns = [", ".join(f"{value:.2f}f" for value in every[start:start + 8]) for start in range(0, len(every), 8)]
    header = [
        "// Generated by tools/launcher/bake-loading-text.py from Montserrat Medium (SIL Open Font",
        "// License 1.1). Do not edit: change the script and run it again.",
        "#pragma once",
        "#include <string_view>",
        "namespace Eden {",
        f"// The overlay's font, the one headless/loading_text.glsl draws: a cap of {CAP} texels,",
        "// and how far each ASCII character from 32 to 126 moves the pen in texels at that cap.",
        "// A character the font has no glyph for moves it as a space does.",
        f"inline constexpr float kHudGlyphCap = {CAP:.2f}f;",
        f"inline constexpr float kHudSpaceAdvance = {SPACE_ADVANCE:.2f}f;",
        "// The overlay's box in a picture 1080 rows high, as kHudCapUnits and kHudPadUnits are in",
        "// loading_text.glsl. Its height is the cap plus twice the pad.",
        f"inline constexpr float kHudCapUnits = {CAP_UNITS:.1f}f;",
        f"inline constexpr float kHudPadUnits = {PAD_UNITS:.1f}f;",
        f"inline constexpr float kHudBoxUnits = {CAP_UNITS + 2 * PAD_UNITS:.1f}f;",
        f"inline constexpr float kHudBaselineUnits = {PAD_UNITS + CAP_UNITS:.1f}f;",
        f"inline constexpr float kHudAdvance[{len(every)}] = {{",
        *[f"    {column}{',' if index + 1 < len(columns) else ''}" for index, column in enumerate(columns)],
        "};",
        "// The widest digit, which every figure in the overlay's line takes: a number that is",
        "// rewritten several times a second must not move what stands after it. hud_step in",
        "// loading_text.glsl lays the line out the same way.",
        f"inline constexpr float kHudDigitAdvance = {digit_advance:.2f}f;",
        "// One line's width, in whatever units its cap is given in.",
        "inline constexpr float HudTextWidth(std::string_view text, float cap) {",
        "    float width = 0.0f;",
        "    for (const char c : text) {",
        "        const unsigned char code = static_cast<unsigned char>(c);",
        "        width += (c >= '0' && c <= '9' ? kHudDigitAdvance",
        "                  : code >= 32 && code < 127 ? kHudAdvance[code - 32] : kHudSpaceAdvance) * cap /",
        "                 kHudGlyphCap;",
        "    }",
        "    return width;",
        "}",
        "} // namespace Eden",
    ]
    ADVANCES_OUTPUT.write_text("\n".join(header) + "\n", encoding="utf-8", newline="\n")
    print(f"{ADVANCES_OUTPUT.name}: advances for ASCII 32..126")


if __name__ == "__main__":
    main()
