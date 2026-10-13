// SPDX-License-Identifier: GPL-3.0-or-later
// The screen shown while a game loads: the game's cover in the middle with a line of light going
// round it as the start advances, the step under it, and the cover's colours spread softly behind
// everything. Drawn entirely in this shader. Shared by both graphics backends, which add their own
// #version line, a sampler2D named loading_art, loading_text.glsl before this file, and a main().
//
//   vec3 loading_scene(vec2 pixel, vec2 size, float seconds, uint state[24])
//
// pixel has its origin at the bottom left; seconds counts from the start of loading. With 1000
// added to it nothing on the screen moves (Settings > Accessibility, reduced motion).
// state comes from hud.h (Loading::State): the step the start is at (1 to 4, 0 before the first),
// the progress in thousandths, the shaders built and their total, the seconds and then the
// milliseconds the step has lasted, the size of the cover in loading_art (its width, and its
// height 16 bits up; 0 when the game has none), the progress in millionths, and from the ninth
// word the theme's accent, pale accent, deep accent and base colours as 0x01RRGGBB (0: the ones
// below, which are Canopy's).
// loading_art (loading_art.h) holds the cover in its top left corner and, kArtGap texels to its
// right, a blurred copy of it kArtBlur texels a side. Its first row is the picture's top. A game
// without a cover gets the same screen around a pane of glass with the progress as a figure.

const vec3 kAccent = vec3(0.663, 0.859, 0.388);
const vec3 kPale = vec3(0.875, 0.910, 0.651);
const vec3 kDeep = vec3(0.141, 0.365, 0.290);
const vec3 kBase = vec3(0.024, 0.035, 0.039);
const vec3 kTitle = vec3(1.0, 0.988, 0.937);
const float kArtGap = 2.0;
const float kArtBlur = 64.0;
// Where each step begins in the progress, and after the last where that one ends (hud.h,
// Loading::Target).
const float kStepFrom[6] = float[6](0.0, 0.02, 0.25, 0.40, 0.90, 0.99);
const int kLineBrand = 0; // then the four steps: kLine[step]
const int kLineSeconds = 6;
// The inks of the lettering.
const int kInkBrand = 0, kInkNow = 1, kInkWord = 2, kInkFigure = 3, kInkAccent = 4, kInkAccentDim = 5;

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float glyph_texel(int glyph, int x, int y)
{
    x = clamp(x, 0, kGlyphWidth - 1);
    y = clamp(y, 0, kGlyphHeight - 1);
    int at = y * kGlyphWidth + x;
    return float((glyph_word(glyph, at >> 3) >> uint((at & 7) * 4)) & 0xfu) / 15.0;
}

// The lettering is read in one place only, at the end of loading_scene: the console's shader
// compiler copes badly with the glyph table being read from many. So the text is laid out first,
// which only decides which glyph, if any, this pixel falls in (a Pick), and what ink it gets.
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

// Takes the pixel for this glyph when it lies between the glyph's pen and the next one.
// at is where the pen stands (left end of the baseline), cap the height of a capital in pixels.
void pick_glyph(inout Pick pick, int glyph, int ink, vec2 pixel, vec2 at, float cap, float tracking)
{
    float texel = cap / kGlyphCap;
    float step = pen_step(glyph, cap, tracking);
    if (pixel.x < at.x - 0.5 * tracking * cap || pixel.x >= at.x + step - 0.5 * tracking * cap)
        return;
    pick.glyph = glyph;
    pick.ink = ink;
    pick.st = vec2((pixel.x - at.x) / texel + kGlyphLeft,
                   float(kGlyphHeight) - ((pixel.y - at.y) / texel + kGlyphBase)) - 0.5;
    pick.texel = texel;
}

float line_width(int line, float cap, float tracking)
{
    float width = 0.0;
    for (int i = 0; i < kLine[line].y; ++i)
        width += pen_step(kText[kLine[line].x + i], cap, tracking);
    return width - tracking * cap;
}

// A line of text with the left end of its baseline at `at`.
void pick_line(inout Pick pick, int line, int ink, vec2 pixel, vec2 at, float cap, float tracking)
{
    if (pixel.y < at.y - 0.4 * cap || pixel.y > at.y + 1.4 * cap || pixel.x < at.x - cap)
        return;
    float pen = at.x;
    for (int i = 0; i < kLine[line].y; ++i)
    {
        int glyph = kText[kLine[line].x + i];
        if (glyph >= 0)
            pick_glyph(pick, glyph, ink, pixel, vec2(pen, at.y), cap, tracking);
        pen += pen_step(glyph, cap, tracking);
    }
}

float number_width(uint value, float cap, float tracking)
{
    float width = 0.0;
    for (int i = 0; i < 6; ++i)
    {
        width += pen_step(kDigit + int(value % 10u), cap, tracking);
        value /= 10u;
        if (value == 0u)
            break;
    }
    return width - tracking * cap;
}

// A number with the right end of its baseline at `at`.
void pick_number(inout Pick pick, uint value, int ink, vec2 pixel, vec2 at, float cap, float tracking)
{
    if (pixel.y < at.y - 0.4 * cap || pixel.y > at.y + 1.4 * cap)
        return;
    float pen = at.x + tracking * cap;
    for (int i = 0; i < 6; ++i)
    {
        int glyph = kDigit + int(value % 10u);
        pen -= pen_step(glyph, cap, tracking);
        pick_glyph(pick, glyph, ink, pixel, vec2(pen, at.y), cap, tracking);
        value /= 10u;
        if (value == 0u)
            break;
    }
}

// A box with round ends: how much of a pixel it covers.
float pill(vec2 pixel, vec2 from, vec2 to, float radius)
{
    vec2 a = vec2(from.x + radius, 0.5 * (from.y + to.y));
    vec2 b = vec2(max(to.x - radius, a.x), a.y);
    float along = clamp(pixel.x, a.x, b.x);
    return clamp(radius - length(pixel - vec2(along, a.y)) + 0.5, 0.0, 1.0);
}

// How far a point is from a box with round corners; negative inside it.
float round_box(vec2 p, vec2 half_size, float radius)
{
    vec2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// How far along the line round the cover a point is, in pixels: from the middle of the top,
// clockwise. corner is where the centres of the round corners are, radius is theirs.
float ring_along(vec2 p, vec2 corner, float radius)
{
    float quarter = 1.5707963 * radius;
    vec2 a = abs(p);
    bool right = p.x > 0.0;
    bool top = p.y > 0.0;
    if (a.x > corner.x && a.y > corner.y)
    {
        // In a corner: the angle turned since the line entered it.
        vec2 d = a - corner;
        float turned = right == top ? atan(d.x, d.y) : atan(d.y, d.x);
        float before = right ? (top ? corner.x : corner.x + quarter + 2.0 * corner.y) :
                               (top ? 3.0 * corner.x + 3.0 * quarter + 4.0 * corner.y :
                                      3.0 * corner.x + 2.0 * quarter + 2.0 * corner.y);
        return before + turned * radius;
    }
    if (a.x - corner.x > a.y - corner.y)
        return right ? corner.x + quarter + corner.y - p.y : 3.0 * corner.x + 3.0 * quarter + 3.0 * corner.y + p.y;
    if (top)
        return right ? p.x : 4.0 * (corner.x + corner.y + quarter) + p.x;
    return 2.0 * corner.x + 2.0 * quarter + 2.0 * corner.y - p.x;
}

// The blurred copy of the cover at st (0 to 1 across it, from its top left), smoothed further:
// four taps weighted as a cubic B-spline, so its few texels show no edges however far they are
// stretched. left is the column it starts at, sheet the size of the texture.
vec3 art_wash(vec2 st, float left, vec2 sheet)
{
    vec2 at = clamp(st, 0.0, 1.0) * (kArtBlur - 1.0) + 0.5;
    vec2 whole = floor(at - 0.5) + 0.5;
    vec2 f = at - whole;
    vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;
    vec2 w3 = f * f * f / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = 1.0 - g0;
    vec2 low = clamp(whole - 1.0 + w1 / g0, 0.5, kArtBlur - 0.5) + vec2(left, 0.0);
    vec2 high = clamp(whole + 1.0 + w3 / g1, 0.5, kArtBlur - 0.5) + vec2(left, 0.0);
    vec3 upper = g0.x * textureLod(loading_art, low / sheet, 0.0).rgb +
                 g1.x * textureLod(loading_art, vec2(high.x, low.y) / sheet, 0.0).rgb;
    vec3 lower = g0.x * textureLod(loading_art, vec2(low.x, high.y) / sheet, 0.0).rgb +
                 g1.x * textureLod(loading_art, high / sheet, 0.0).rgb;
    return g0.y * upper + g1.y * lower;
}

vec3 theme_colour(uint word, vec3 otherwise)
{
    if ((word >> 24) == 0u)
        return otherwise;
    return vec3(float((word >> 16) & 0xffu), float((word >> 8) & 0xffu), float(word & 0xffu)) / 255.0;
}

vec3 loading_scene(vec2 pixel, vec2 size, float seconds, uint state[24])
{
    bool calm = seconds >= 1000.0;
    if (calm)
        seconds -= 1000.0;
    int stage = int(min(state[0], 4u));
    float progress = clamp(float(state[7]) / 1000000.0, 0.0, 1.0);
    uint percent = uint(progress * 100.0 + 0.001);
    // The step's name comes up into place when the step begins.
    float settled = calm ? 1.0 : smoothstep(0.0, 0.35, float(state[5]) / 1000.0);
    vec2 art_size = vec2(float(state[6] & 0xffffu), float(state[6] >> 16));
    bool has_art = art_size.x > 0.0 && art_size.y > 0.0;
    vec3 accent = theme_colour(state[8], kAccent);
    vec3 pale = theme_colour(state[9], kPale);
    vec3 deep = theme_colour(state[10], kDeep);
    vec3 base = theme_colour(state[11], kBase);
    vec3 glass = base + 0.27 * deep;
    float unit = size.y / 1080.0; // one design pixel
    float middle = 0.5 * size.x;
    vec2 uv = pixel / size;
    vec2 sheet = vec2(textureSize(loading_art, 0));
    Pick pick = Pick(-1, 0, vec2(0.0), 1.0);

    // ---- the backdrop: the cover's colours, blurred, dimmed and slowly drifting ----
    vec3 color;
    if (has_art)
    {
        vec2 st = 0.5 + (vec2(uv.x, 1.0 - uv.y) - 0.5) * vec2(0.86, 0.60);
        if (!calm)
            st += 0.025 * vec2(sin(seconds * 0.11), cos(seconds * 0.09));
        vec3 wash = art_wash(st, art_size.x + kArtGap, sheet);
        wash = max(mix(vec3(dot(wash, vec3(0.299, 0.587, 0.114))), wash, 1.45), 0.0);
        color = mix(base, wash, 0.90) * 0.42;
    }
    else
    {
        // No cover: the theme's dusk, lit from behind the pane.
        vec2 from_light = (uv - vec2(0.5, 0.61)) * vec2(size.x / size.y, 1.0);
        float near = dot(from_light, from_light);
        color = base + deep * 0.42 * exp(-near * 2.4) + accent * 0.05 * exp(-near * 9.0);
    }
    color *= mix(0.55, 1.0, smoothstep(0.0, 0.62, uv.y));
    color *= 1.0 - 0.45 * smoothstep(0.42, 1.05, length((uv - 0.5) * vec2(1.0, 1.12)));
    // A darker pool where the lettering stands, whatever the cover's colours are.
    vec2 pool = (pixel - vec2(middle, 340.0 * unit)) / (vec2(560.0, 150.0) * unit);
    color *= 1.0 - 0.30 * exp(-dot(pool, pool));

    // ---- motes rising through it ----
    float moved = calm ? 3.0 : seconds;
    for (int i = 0; i < 10; ++i)
    {
        float n = float(i);
        vec2 seed = vec2(hash21(vec2(n, 1.7)), hash21(vec2(n, 9.2)));
        vec2 at = vec2(seed.x + 0.03 * sin(moved * (0.2 + 0.3 * seed.y) + n),
                       fract(seed.y + moved * (0.010 + 0.018 * hash21(vec2(n, 4.1)))));
        float reach = (1.6 + 2.4 * hash21(vec2(n, 6.3))) * unit;
        vec2 away = pixel - at * size;
        float near = dot(away, away) / (reach * reach);
        float shown = (0.55 + 0.45 * sin(moved * (0.8 + seed.x) + n * 2.4)) * smoothstep(0.0, 0.12, at.y) *
                      (1.0 - smoothstep(0.75, 1.0, at.y));
        color += pale * shown * (0.26 * exp(-near) + 0.045 * exp(-near / 30.0));
    }

    // ---- the cover: where the launcher left it (launcher.cpp, draw_launch), breathing a little ----
    float arrive = calm ? 1.0 : 1.0 - pow(1.0 - clamp(seconds / 0.7, 0.0, 1.0), 3.0);
    vec2 centre = vec2(middle, (660.0 + (calm ? 0.0 : 4.0 * sin(seconds * 0.9))) * unit);
    vec2 half_card = 170.0 * unit * mix(0.94, 1.0, arrive) *
                     (has_art ? art_size / max(art_size.x, art_size.y) : vec2(1.0));
    float radius = 30.0 * unit;
    float gap = 22.0 * unit;
    vec2 rel = pixel - centre;
    float card_sd = round_box(rel, half_card, radius);
    float ring_sd = round_box(rel, half_card + gap, radius + gap);
    // Its shadow, cast downwards.
    color *= 1.0 - 0.50 * (1.0 - smoothstep(-20.0 * unit, 90.0 * unit,
                                            round_box(rel + vec2(0.0, 26.0 * unit), half_card + gap, radius + gap)));

    // ---- the line of light round it: as far round as the start has come ----
    if (abs(ring_sd) < 70.0 * unit)
    {
        float reach = 2.5 * unit;
        vec2 corner = half_card - radius;
        float turn = radius + gap;
        float whole = 4.0 * (corner.x + corner.y) + 6.2831853 * turn;
        float along = ring_along(rel, corner, turn);
        float ahead = along - progress * whole; // above 0: not reached yet
        float track = clamp(reach - abs(ring_sd) + 0.5, 0.0, 1.0);
        float lit = clamp(reach - length(vec2(max(ahead, 0.0), ring_sd)) + 0.5, 0.0, 1.0);
        float begun = progress > 0.0005 ? 1.0 : 0.0;
        lit = max(lit, begun * clamp(reach - length(vec2(whole - along, ring_sd)) + 0.5, 0.0, 1.0));
        // Brighter towards its head, with a light passing along it.
        vec3 light = mix(accent * 0.80, accent, clamp(along / max(progress * whole, 1.0), 0.0, 1.0));
        light = mix(light, pale, exp(-max(-ahead, 0.0) / (90.0 * unit)));
        if (!calm)
        {
            float sweep = fract(seconds * 0.38) * (progress * whole + 300.0 * unit) - 150.0 * unit;
            light += vec3(0.30) * exp(-pow((along - sweep) / (80.0 * unit), 2.0));
        }
        // Its glow on what is around it, strongest at the head.
        float reached = clamp(0.5 - ahead / (12.0 * unit), 0.0, 1.0) * smoothstep(0.0, 36.0 * unit, along);
        color += accent * 0.20 * reached * exp(-abs(ring_sd) / (10.0 * unit));
        color += pale * begun * (calm ? 0.36 : 0.36 + 0.08 * sin(seconds * 3.0)) *
                 exp(-(ahead * ahead + ring_sd * ring_sd) / (484.0 * unit * unit));
        color = mix(color, vec3(1.0), 0.13 * track * arrive);
        color = mix(color, light, lit * arrive);
    }

    float on_card = clamp(0.5 - card_sd, 0.0, 1.0);
    if (on_card > 0.0)
    {
        vec2 st = vec2(rel.x / half_card.x, -rel.y / half_card.y) * 0.5 + 0.5; // from its top left
        vec3 face = mix(color, glass, 0.80) + accent * 0.05 * (1.0 - st.y);
        if (has_art)
        {
            vec4 cover = textureLod(loading_art, clamp(st * art_size, vec2(0.5), art_size - 0.5) / sheet, 0.0);
            face = mix(glass, cover.rgb, cover.a);
        }
        // A line of light along its edge, strongest at the top.
        float rim = clamp(1.0 - abs(card_sd + 1.5 * unit) / (1.5 * unit), 0.0, 1.0);
        face = mix(face, vec3(1.0), rim * mix(0.26, 0.07, st.y));
        color = mix(color, face, on_card * arrive);
    }
    if (!has_art)
    {
        // On the pane: how far the start is, as a figure.
        float cap = 58.0 * unit;
        float sign_cap = 28.0 * unit;
        float figure = number_width(percent, cap, 0.02);
        float left = middle - 0.5 * (figure + 8.0 * unit + pen_step(kPercent, sign_cap, 0.0));
        float baseline = centre.y - 0.5 * cap;
        pick_number(pick, percent, kInkFigure, pixel, vec2(left + figure, baseline), cap, 0.02);
        if (pixel.y > baseline - 0.4 * sign_cap && pixel.y < baseline + 1.4 * sign_cap)
            pick_glyph(pick, kPercent, kInkFigure, pixel, vec2(left + figure + 8.0 * unit, baseline), sign_cap, 0.0);
    }

    // ---- under it: the step the start is at ----
    float headline = 22.0 * unit;
    float line_y = (388.0 - (1.0 - settled) * 10.0) * unit;
    if (stage > 0 && pixel.y > line_y - 0.4 * headline && pixel.y < line_y + 1.4 * headline)
        pick_line(pick, stage, kInkNow, pixel, vec2(middle - 0.5 * line_width(stage, headline, 0.20), line_y),
                  headline, 0.20);

    // ---- the four steps as four short lines, each filling with its own share ----
    float pip = 44.0 * unit;
    float pip_gap = 10.0 * unit;
    if (abs(pixel.y - 346.0 * unit) < 4.0 * unit)
    {
        for (int i = 1; i <= 4; ++i)
        {
            vec2 from = vec2(middle - 2.0 * pip - 1.5 * pip_gap + float(i - 1) * (pip + pip_gap), 344.0 * unit);
            vec2 to = from + vec2(pip, 4.0 * unit);
            float share = i < stage ? 1.0 : i > stage ? 0.0 :
                          clamp((progress - kStepFrom[i]) / (kStepFrom[i + 1] - kStepFrom[i]), 0.0, 1.0);
            color = mix(color, vec3(1.0), 0.16 * pill(pixel, from, to, 2.0 * unit));
            if (share > 0.0)
                color = mix(color, i == stage ? pale : accent,
                            (i == stage ? 1.0 : 0.78) *
                                pill(pixel, from, vec2(from.x + max(pip * share, 4.0 * unit), to.y), 2.0 * unit));
        }
    }

    // ---- a line of figures: how far it is, then the shaders built of their total, or the seconds
    // a slow step has lasted ----
    float small = 14.0 * unit;
    float detail_y = 296.0 * unit;
    if (pixel.y > detail_y - 0.4 * small && pixel.y < detail_y + 1.4 * small)
    {
        bool counting = stage == 3 && state[3] > 0u;
        bool slow = !counting && state[4] >= 20u;
        float sign_at = number_width(percent, small, 0.08) + 0.08 * small;
        float percent_width = has_art ? sign_at + pen_step(kPercent, small, 0.0) : 0.0;
        float between = has_art && (counting || slow) ? 24.0 * unit : 0.0;
        float built_width = number_width(state[2], small, 0.08);
        float slash_width = pen_step(kSlash, small, 0.0);
        float seconds_width = number_width(state[4], small, 0.08);
        float figures = counting ? built_width + slash_width + 14.0 * unit + number_width(state[3], small, 0.08) :
                        slow ? seconds_width + 5.0 * unit + line_width(kLineSeconds, small, 0.0) : 0.0;
        float pen = middle - 0.5 * (percent_width + between + figures);
        if (has_art)
        {
            pick_number(pick, percent, kInkWord, pixel, vec2(pen + sign_at - 0.08 * small, detail_y), small, 0.08);
            pick_glyph(pick, kPercent, kInkWord, pixel, vec2(pen + sign_at, detail_y), small, 0.0);
            pen += percent_width + between;
        }
        if (counting)
        {
            pick_number(pick, state[2], kInkAccent, pixel, vec2(pen + built_width, detail_y), small, 0.08);
            pick_glyph(pick, kSlash, kInkAccentDim, pixel, vec2(pen + built_width + 7.0 * unit, detail_y), small, 0.0);
            pick_number(pick, state[3], kInkAccentDim, pixel, vec2(pen + figures, detail_y), small, 0.08);
        }
        else if (slow)
        {
            pick_number(pick, state[4], kInkAccent, pixel, vec2(pen + seconds_width, detail_y), small, 0.08);
            pick_line(pick, kLineSeconds, kInkAccent, pixel, vec2(pen + seconds_width + 5.0 * unit, detail_y), small, 0.0);
        }
    }

    // ---- the name, small at the bottom ----
    float brand = 12.0 * unit;
    float brand_y = 96.0 * unit;
    if (pixel.y > brand_y - 0.4 * brand && pixel.y < brand_y + 1.4 * brand)
        pick_line(pick, kLineBrand, kInkBrand, pixel, vec2(middle - 0.5 * line_width(kLineBrand, brand, 0.42), brand_y),
                  brand, 0.42);

    // ---- the lettering: the one place the glyph table is read ----
    if (pick.glyph >= 0 && pick.st.x > -1.0 && pick.st.y > -1.0 && pick.st.x < float(kGlyphWidth) &&
        pick.st.y < float(kGlyphHeight))
    {
        ivec2 i = ivec2(floor(pick.st));
        vec2 f = pick.st - vec2(i);
        float value = mix(mix(glyph_texel(pick.glyph, i.x, i.y), glyph_texel(pick.glyph, i.x + 1, i.y), f.x),
                          mix(glyph_texel(pick.glyph, i.x, i.y + 1), glyph_texel(pick.glyph, i.x + 1, i.y + 1), f.x),
                          f.y);
        float cover = clamp((value - 0.5) * 2.0 * kGlyphSpread * pick.texel + 0.5, 0.0, 1.0);
        vec3 ink = pick.ink >= kInkAccent ? accent : pick.ink == kInkBrand ? vec3(1.0) : kTitle;
        float strength = pick.ink == kInkBrand ? 0.30 : pick.ink == kInkNow ? 0.96 * settled :
                         pick.ink == kInkWord ? 0.66 : pick.ink == kInkFigure ? 0.94 : pick.ink == kInkAccent ? 0.92 :
                         0.56;
        color = mix(color, ink, strength * cover);
    }

    // Arrive out of the dark; a little noise keeps the gradients from banding.
    color *= smoothstep(0.0, 0.5, seconds);
    color += (hash21(pixel + fract(seconds) * 61.0) - 0.5) / 255.0 * 1.4;
    return clamp(color, 0.0, 1.0);
}
