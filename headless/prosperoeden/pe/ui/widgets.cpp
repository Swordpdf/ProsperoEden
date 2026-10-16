// ProsperoEden - Launcher building blocks: backdrop, panels, plates, buttons, icons, lists, hints.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pe::ui
{

namespace
{

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.28318530718f;
FitReport fit_report = nullptr;

// A text colour as shown: with high contrast, greys go most of the way to white and accents a
// little, and faded text is less faded.
Color text_ink(Color color)
{
    if (!look().high_contrast)
        return color;
    const float high = std::max({color.r, color.g, color.b});
    const float low = std::min({color.r, color.g, color.b});
    const float lift = high - low > 0.24f ? 0.25f : 0.70f;
    return {color.r + (1.0f - color.r) * lift, color.g + (1.0f - color.g) * lift,
            color.b + (1.0f - color.b) * lift, 1.0f - (1.0f - color.a) * 0.5f};
}

// Draws at exactly `drawn` on the line laid out for text of `size`.
float draw_text(Canvas &c, std::string_view value, float x, float baseline, float size, float drawn,
                Color color, Align align, float tracking)
{
    gfx::TextStyle style;
    style.size = drawn;
    style.color = text_ink(color);
    style.align = align;
    style.tracking = tracking;
    // Text of a title's size is drawn heavier without being asked (Heavy does it for smaller text).
    const float weight = c.weight > 0.0f ? c.weight : size >= 44.0f ? th().weight_title : 0.0f;
    style.weight = weight * drawn / 48.0f;
    return c.list.text(*c.fonts.font, c.fonts.texture, value, x, baseline + (drawn - size) * 0.35f,
                       style);
}
const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
const Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
const Color kClear{0.0f, 0.0f, 0.0f, 0.0f};

// A repeatable 0..1 value for element `index` and property `salt`.
float noise(int index, int salt)
{
    std::uint32_t value = static_cast<std::uint32_t>(index) * 0x9e3779b1u +
                          static_cast<std::uint32_t>(salt) * 0x85ebca6bu + 0x27d4eb2fu;
    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    value ^= value >> 12;
    value *= 0x297a2d39u;
    value ^= value >> 15;
    return static_cast<float>(value >> 8) / 16777216.0f;
}

// The focus glow's slow pulse, 0..1.
float breath(const Canvas &c)
{
    return 0.5f + 0.5f * std::sin(c.time * th().breathe * motion());
}

} // namespace

// ---------------------------------------------------------------- backdrop

void Backdrop::update(float dt)
{
    // With reduced motion the art stands still.
    time_ += dt * motion();
}

Rect Backdrop::art() const
{
    // The art is 2048x1152 around the 1920x1080 screen: room to drift.
    const float dx = 24.0f * th().drift * std::sin(time_ * 0.050f);
    const float dy = 11.0f * th().drift * std::sin(time_ * 0.037f + 1.3f);
    return {-64.0f + dx, -36.0f + dy, 2048.0f, 1152.0f};
}

Rect Backdrop::picture(const Textures &textures) const
{
    // A wallpaper is resized to the screen, whole: whatever its shape leaves over shows the
    // theme behind, and it keeps its own place instead of drifting (the drift would push a part
    // of it past the edge), so no part of it is cut.
    const int width = textures.wallpaper_width();
    if (width > 0)
    {
        const int height = textures.wallpaper_height();
        const float scale = std::min(1920.0f / width, 1080.0f / height);
        const float w = width * scale;
        const float h = height * scale;
        return {(1920.0f - w) * 0.5f, (1080.0f - h) * 0.5f, w, h};
    }
    return art();
}

Rect Backdrop::uv(const Rect &r, const Textures &textures) const
{
    const Rect a = picture(textures);
    return {(r.x - a.x) / a.w, (r.y - a.y) / a.h, r.w / a.w, r.h / a.h};
}

void Backdrop::draw(gfx::DrawList &list, const Textures &textures, float dim) const
{
    const Theme &t = th();
    const Rect screen{0.0f, 0.0f, 1920.0f, 1080.0f};
    list.rounded_rect(screen, 0.0f, t.base);
    const Rect a = art();
    if (textures.backdrop() != 0)
        list.image(textures.backdrop(), picture(textures), {0.0f, 0.0f, 1.0f, 1.0f}, kWhite);

    // The artwork's light breathes.
    const float sun_x = a.x + t.light_x * a.w;
    const float sun_y = a.y + t.light_y * a.h;
    const float glow = 0.5f + 0.5f * std::sin(time_ * 0.55f);
    list.shadow({sun_x - 150.0f, sun_y - 150.0f, 300.0f, 300.0f}, 150.0f, 170.0f,
                t.highlight.with_alpha(0.03f + 0.04f * glow));

    // Motes of light rise, nearer than the art.
    constexpr float kSpan = 640.0f;
    const float near_x = (a.x + 64.0f) * 1.7f;
    const float near_y = (a.y + 36.0f) * 1.7f;
    for (int i = 0; i < t.motes && !look().reduce_motion && !look().high_contrast; ++i)
    {
        const float speed = 6.0f + 11.0f * noise(i, 2);
        const float travel = std::fmod(time_ * speed + noise(i, 3) * kSpan, kSpan);
        const float life = std::sin(kPi * travel / kSpan);
        const float x = 800.0f + 1120.0f * noise(i, 1) + near_x +
                        28.0f * std::sin(time_ * (0.21f + 0.2f * noise(i, 4)) + kTau * noise(i, 5));
        const float y = 900.0f - travel + near_y;
        const float size = 3.0f + 5.0f * noise(i, 6);
        const float twinkle =
            0.65f + 0.35f * std::sin(time_ * (0.9f + 1.4f * noise(i, 7)) + kTau * noise(i, 8));
        const float alpha = 0.30f * life * twinkle;
        const Color color = gfx::mix(t.accent_pale, t.highlight, noise(i, 9));
        list.shadow({x - size, y - size, size * 2.0f, size * 2.0f}, size, size * 0.9f,
                    color.with_alpha(alpha));
        list.circle(x, y, size * 0.28f, color.with_alpha(std::min(1.0f, alpha * 1.8f)));
    }
    // High contrast: the art steps back behind everything.
    if (look().high_contrast)
        dim = std::max(dim, 0.62f);
    if (dim > 0.0f)
        list.rounded_rect(screen, 0.0f, t.scrim.with_alpha(dim));
}

// ---------------------------------------------------------------- text

float text(Canvas &c, std::string_view value, float x, float baseline, float size, Color color,
           Align align, float tracking)
{
    return draw_text(c, value, x, baseline, size, text_size(size), color, align, tracking);
}

float text_width(Canvas &c, std::string_view value, float size, float tracking)
{
    return c.fonts.font->measure(value, text_size(size), tracking);
}

float text_fit(Canvas &c, std::string_view value, float x, float baseline, float size, Color color,
               float max_width, Align align)
{
    const float drawn = text_size(size);
    return draw_text(c, c.fonts.font->fit(value, drawn, max_width), x, baseline, size, drawn, color,
                     align, 0.0f);
}

void set_fit_report(FitReport report)
{
    fit_report = report;
}

float text_shrink(Canvas &c, std::string_view value, float x, float baseline, float size,
                  Color color, float max_width, Align align, float tracking, float least)
{
    const gfx::Font &font = *c.fonts.font;
    const float drawn = text_size(size);
    const float width = font.measure(value, drawn, tracking);
    if (width <= max_width)
        return draw_text(c, value, x, baseline, size, drawn, color, align, tracking);
    // Larger text gives its extra size back before anything is cut.
    const float scale = std::max(least * size / drawn, max_width / width);
    if (fit_report != nullptr)
        fit_report(value, scale, width * scale > max_width + 0.5f);
    return draw_text(c, font.fit(value, drawn * scale, max_width + 0.5f, tracking * scale), x,
                     baseline, size, drawn * scale, color, align, tracking * scale);
}

int text_block(Canvas &c, std::string_view value, float x, float first_baseline, float size,
               float line_height, Color color, float max_width, int max_lines, float least)
{
    const gfx::Font &font = *c.fonts.font;
    const float full = text_size(size);
    float drawn = full;
    std::vector<std::string> lines = font.wrap(value, drawn, max_width);
    // A step smaller at a time, until the text fits its lines or the letters are as small as
    // allowed.
    // (Larger text gives its extra size back before anything is cut.)
    const float smallest = size * least;
    while (static_cast<int>(lines.size()) > max_lines && drawn > smallest + 0.01f)
    {
        drawn = std::max(smallest, drawn * 0.94f);
        lines = font.wrap(value, drawn, max_width);
    }
    if (fit_report != nullptr && drawn < full)
        fit_report(value, drawn / full, static_cast<int>(lines.size()) > max_lines);
    const int count = std::min(static_cast<int>(lines.size()), max_lines);
    for (int line = 0; line < count; ++line)
    {
        const bool cut = line == count - 1 && static_cast<int>(lines.size()) > max_lines;
        // The last line that fits swallows the next one, so its ellipsis shows more follows.
        const std::string content =
            !cut ? lines[line] :
            lines[line] + (gfx::joins_without_space(lines[line], lines[line + 1]) ? "" : " ") + lines[line + 1];
        draw_text(c, font.fit(content, drawn, max_width), x,
                  first_baseline + line_height * static_cast<float>(line), size, drawn, color,
                  Align::left, 0.0f);
    }
    return count;
}

float kicker(Canvas &c, std::string_view value, float x, float baseline, Color color, float max_width,
             Align align)
{
    return text_shrink(c, value, x, baseline, th().size_small, color, max_width, align,
                       th().tracking_kicker);
}

namespace
{

// The warning mark with its left edge at x, centred on the line of text of `size` at `baseline`.
// Returns the room it takes, the gap to the text included.
float warning_mark(Canvas &c, float x, float baseline, float size, Color color, bool draw = true)
{
    const float drawn = text_size(size);
    const float radius = drawn * 0.46f;
    if (draw)
    {
        const Color ink = text_ink(color);
        const float cx = x + radius;
        const float cy = baseline - size * 0.35f;
        c.list.ring(cx, cy, radius - 0.5f, drawn * 0.085f, ink);
        c.list.line(cx, cy - radius * 0.50f, cx, cy + radius * 0.10f, drawn * 0.11f, ink);
        c.list.circle(cx, cy + radius * 0.48f, drawn * 0.065f, ink);
    }
    return radius * 2.0f + drawn * 0.42f;
}

} // namespace

float notice(Canvas &c, std::string_view value, float x, float baseline, float size, Color color,
             float max_width, bool warning, Align align)
{
    if (!warning || value.empty())
        return text_shrink(c, value, x, baseline, size, color, max_width, align);
    const float mark = warning_mark(c, x, baseline, size, color, false);
    if (align == Align::right)
    {
        const float width = text_shrink(c, value, x, baseline, size, color, max_width - mark, align);
        warning_mark(c, x - width - mark, baseline, size, color);
        return width + mark;
    }
    warning_mark(c, x, baseline, size, color);
    return mark + text_shrink(c, value, x + mark, baseline, size, color, max_width - mark, align);
}

void notice_block(Canvas &c, std::string_view value, float x, float first_baseline, float size,
                  float line_height, Color color, float max_width, int max_lines, bool warning)
{
    const float mark = warning && !value.empty() ? warning_mark(c, x, first_baseline, size, color) : 0.0f;
    text_block(c, value, x + mark, first_baseline, size, line_height, color, max_width - mark,
               max_lines, kShrink);
}

float value_column(Canvas &c, std::initializer_list<std::string_view> labels, float left,
                   float column, float size, float tracking)
{
    float widest = 0.0f;
    for (const std::string_view label : labels)
        widest = std::max(widest, text_width(c, label, size, tracking));
    return std::max(column, left + widest + 24.0f);
}

// ---------------------------------------------------------------- surfaces

void glass(Canvas &c, const Rect &r, float radius, Color tint, Color edge, float shadow)
{
    const Theme &t = th();
    if (shadow > 0.0f)
        c.list.shadow({r.x + 6.0f, r.y + t.shadow_offset, r.w - 12.0f, r.h - 8.0f}, radius, t.shadow_blur,
                      kBlack.with_alpha(0.42f * shadow));
    if (look().high_contrast)
    {
        // A solid panel with a clear edge: nothing of the art shows through the text.
        c.list.bordered_rect(r, radius, gfx::mix(t.scrim, t.panel, 0.4f), 2.0f, t.copy);
        return;
    }
    if (t.glass_panels && c.textures.backdrop_blur() != 0)
        c.list.rounded_image(c.textures.backdrop_blur(), r, c.backdrop.uv(r, c.textures), radius, kWhite);
    else
        tint = tint.with_alpha(1.0f / std::max(tint.a, 0.01f)); // no glass: the tint is the panel
    c.list.bordered_rect(r, radius, tint, t.border, edge);
    // Light catches the top edge.
    const float sheen = std::min(r.h * 0.45f, 150.0f);
    c.list.gradient_rect({r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, sheen}, radius,
                         kWhite.with_alpha(0.045f), kWhite.with_alpha(0.0f));
}

void panel(Canvas &c, const Rect &r, float strength)
{
    glass(c, r, th().radius_panel, th().panel.with_alpha(strength), th().panel_edge.with_alpha(0.42f));
}

Plate row_plate()
{
    const Theme &t = th();
    return {t.radius_row,
            t.row.with_alpha(0.88f),
            t.row_edge.with_alpha(0.34f),
            t.row_focus.with_alpha(0.97f),
            t.accent.with_alpha(0.20f),
            t.accent_deep.with_alpha(0.16f),
            t.accent_pale.with_alpha(0.95f)};
}

Plate list_plate()
{
    const Theme &t = th();
    return {t.radius_row,
            gfx::mix(t.row, t.panel, 0.25f).with_alpha(0.88f),
            t.row_edge.with_alpha(0.34f),
            t.row_focus.with_alpha(0.97f),
            t.accent.with_alpha(0.20f),
            t.accent_deep.with_alpha(0.16f),
            t.accent_pale.with_alpha(0.95f)};
}

Plate button_plate()
{
    const Theme &t = th();
    return {t.radius_row,
            t.glass.with_alpha(0.80f),
            t.panel_edge.with_alpha(0.46f),
            t.row_focus.with_alpha(0.97f),
            t.accent.with_alpha(0.20f),
            t.accent_deep.with_alpha(0.16f),
            t.accent_pale.with_alpha(0.95f)};
}

Plate tile_plate()
{
    const Theme &t = th();
    return {t.radius_card,
            t.glass.with_alpha(0.74f),
            t.row_edge.with_alpha(0.36f),
            t.row_focus.with_alpha(0.97f),
            t.accent.with_alpha(0.18f),
            t.accent_deep.with_alpha(0.14f),
            t.accent_pale.with_alpha(0.95f)};
}

Plate nav_plate()
{
    const Theme &t = th();
    return {t.radius_row,
            kClear,
            kClear,
            t.row_focus.with_alpha(0.0f),
            t.accent.with_alpha(0.26f),
            t.accent_deep.with_alpha(0.26f),
            t.accent.with_alpha(0.60f)};
}

void plate_rest(Canvas &c, const Plate &style, const Rect &r)
{
    if (style.fill.a <= 0.0f && style.edge.a <= 0.0f)
        return;
    if (look().high_contrast)
        c.list.bordered_rect(r, style.radius, gfx::mix(th().scrim, th().row, 0.6f), 1.5f, th().faint);
    else
        c.list.bordered_rect(r, style.radius, style.fill, th().border, style.edge);
}

void plate_focus(Canvas &c, const Plate &style, const Rect &r, float amount)
{
    if (amount <= 0.001f)
        return;
    const Theme &t = th();
    if (look().high_contrast)
    {
        // The highlight is a dark fill inside a bright outline: it does not rest on colour.
        c.list.bordered_rect(r, style.radius, gfx::mix(t.scrim, t.row_focus, 0.7f).with_alpha(amount), 3.0f,
                             t.title.with_alpha(amount));
        return;
    }
    // The highlight's light, breathing slowly.
    const float glow = 0.20f + 0.12f * breath(c);
    c.list.shadow({r.x - 2.0f, r.y + 2.0f, r.w + 4.0f, r.h + 2.0f}, style.radius + 2.0f, t.focus_glow,
                  t.accent.with_alpha(glow * amount));
    if (style.focus_base.a > 0.0f)
        c.list.rounded_rect(r, style.radius, style.focus_base.with_alpha(amount));
    c.list.hgradient_rect(r, style.radius, style.focus_left.with_alpha(amount),
                          style.focus_right.with_alpha(amount), t.focus_width,
                          style.focus_edge.with_alpha(amount));
}

void plate(Canvas &c, const Plate &style, const Rect &r, float focus)
{
    plate_rest(c, style, r);
    plate_focus(c, style, r, focus);
}

void focus_ring(Canvas &c, const Rect &r, float radius, float amount)
{
    if (amount <= 0.001f)
        return;
    const Theme &t = th();
    if (look().high_contrast)
    {
        c.list.bordered_rect({r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, r.h + 6.0f}, radius + 3.0f, kClear, 3.0f,
                             t.title.with_alpha(amount));
        return;
    }
    const float glow = 0.26f + 0.16f * breath(c);
    c.list.shadow({r.x - 4.0f, r.y, r.w + 8.0f, r.h + 6.0f}, radius + 4.0f, t.focus_glow + 8.0f,
                  t.accent.with_alpha(glow * amount));
    const float w = t.focus_width;
    c.list.bordered_rect({r.x - w, r.y - w, r.w + 2.0f * w, r.h + 2.0f * w}, radius + w, kClear, w,
                         t.accent_pale.with_alpha(0.95f * amount));
}

void cover(Canvas &c, const std::string &path, const Rect &r, float radius, float shadow)
{
    if (shadow > 0.0f)
        c.list.shadow({r.x + 3.0f, r.y + 10.0f * shadow, r.w - 6.0f, r.h - 4.0f}, radius,
                      24.0f * shadow, kBlack.with_alpha(0.5f));
    const Cover image = c.textures.cover(path, r.w);
    const float fade = image.texture != 0 ? tween::cubic_out(image.age / 0.22f) : 0.0f;
    if (fade < 1.0f)
    {
        // A dark tile until the cover is ready; the app icon when the game has none.
        c.list.gradient_rect(r, radius, th().glass, th().panel);
        if (image.missing && c.textures.brand() != 0)
            c.list.rounded_image(c.textures.brand(), r, {0.0f, 0.0f, 1.0f, 1.0f}, radius,
                                 kWhite.with_alpha(0.92f));
    }
    if (image.texture != 0)
        c.list.rounded_image(image.texture, r, {0.0f, 0.0f, 1.0f, 1.0f}, radius,
                             kWhite.with_alpha(fade));
    c.list.bordered_rect(r, radius, kClear, 1.0f, kWhite.with_alpha(0.10f));
}

void controller_icon(Canvas &c, const Rect &r, float lit)
{
    const std::uint32_t picture = c.textures.controller();
    if (picture == 0)
        return;
    const float u = r.w / 72.0f; // the picture is drawn in a 72x50 box
    if (lit > 0.01f)
    {
        // The light under a connected controller, breathing slowly.
        const float glow = 0.85f + 0.15f * std::sin(c.time * 1.7f * motion() + r.x * 0.01f);
        c.list.shadow({r.x + 8.0f * u, r.y + 8.0f * u, r.w - 16.0f * u, r.h - 14.0f * u},
                      14.0f * u, 22.0f * u, th().accent.with_alpha(0.26f * lit * glow));
    }
    c.list.image(picture, r, {0.0f, 0.0f, 1.0f, 1.0f}, gfx::mix(kWhite.with_alpha(0.16f), th().text, lit));
    if (lit > 0.01f)
    {
        // The light bar on either side of the touchpad.
        const Color bar = th().accent.with_alpha(lit);
        for (const float x : {26.0f, 46.0f})
            c.list.line(r.x + x * u, r.y + 10.0f * u, r.x + x * u, r.y + 19.0f * u, 1.4f * u, bar);
    }
}

void toggle(Canvas &c, float right, float cy, float position)
{
    constexpr float kWidth = 64.0f;
    constexpr float kHeight = 34.0f;
    const Theme &t = th();
    const Rect track{right - kWidth, cy - kHeight * 0.5f, kWidth, kHeight};
    c.list.bordered_rect(track, kHeight * 0.5f, gfx::mix(t.row, t.row_edge, 0.18f).with_alpha(0.95f), 1.0f,
                         t.row_edge.with_alpha(0.7f));
    c.list.hgradient_rect(track, kHeight * 0.5f, t.accent.with_alpha(0.95f * position),
                          gfx::mix(t.accent, t.accent_deep, 0.35f).with_alpha(0.95f * position));
    const float knob_x = track.x + kHeight * 0.5f + (kWidth - kHeight) * position;
    c.list.shadow({knob_x - 12.0f, cy - 10.0f, 24.0f, 24.0f}, 12.0f, 6.0f, kBlack.with_alpha(0.35f));
    c.list.circle(knob_x, cy, 12.5f, gfx::mix(t.copy, t.on_accent, position));
}

void level_bar(Canvas &c, float right, float cy, float width, float level, float focus)
{
    constexpr float kHeight = 8.0f;
    const Theme &t = th();
    const Rect track{right - width, cy - kHeight * 0.5f, width, kHeight};
    c.list.rounded_rect(track, kHeight * 0.5f, gfx::mix(t.row, t.row_edge, 0.28f).with_alpha(0.95f));
    const float filled = std::max(kHeight, width * std::clamp(level, 0.0f, 1.0f));
    if (level > 0.0f)
        c.list.hgradient_rect({track.x, track.y, filled, kHeight}, kHeight * 0.5f, t.accent_deep, t.accent);
    const float knob = 7.0f + 3.0f * focus;
    c.list.shadow({track.x + filled - knob, cy - knob + 2.0f, knob * 2.0f, knob * 2.0f}, knob, 5.0f,
                  kBlack.with_alpha(0.35f));
    c.list.circle(track.x + (level > 0.0f ? filled : 0.0f), cy, knob, t.title);
}

void progress_bar(Canvas &c, const Rect &r, float done)
{
    const Theme &t = th();
    const float radius = r.h * 0.5f;
    c.list.rounded_rect(r, radius, t.text.with_alpha(0.16f));
    done = std::clamp(done, 0.0f, 1.0f);
    if (done <= 0.0f)
        return;
    const float width = std::max(r.h, r.w * done);
    c.list.hgradient_rect({r.x, r.y, width, r.h}, radius, gfx::mix(t.accent, t.accent_deep, 0.3f),
                          gfx::mix(t.accent, kWhite, 0.25f));
    // A sheen runs along what is done while it grows.
    if (done < 1.0f && !look().reduce_motion && width > 40.0f)
    {
        const float span = width + 120.0f;
        const float at = r.x - 120.0f + std::fmod(c.time * 260.0f, span);
        c.list.push_clip({r.x, r.y, width, r.h});
        c.list.hgradient_rect({at, r.y, 60.0f, r.h}, 0.0f, kWhite.with_alpha(0.0f), kWhite.with_alpha(0.55f));
        c.list.hgradient_rect({at + 60.0f, r.y, 60.0f, r.h}, 0.0f, kWhite.with_alpha(0.55f),
                              kWhite.with_alpha(0.0f));
        c.list.pop_clip();
    }
}

float chooser(Canvas &c, std::string_view value, float right, float baseline, float focus,
              Color color)
{
    const float size = th().size_text;
    const float arrow = 26.0f * focus; // room the chevrons take when focused
    const float width = text(c, value, right - arrow, baseline, size, color, Align::right);
    if (focus <= 0.01f)
        return width;
    const float cy = baseline - size * 0.35f;
    const Color ink = th().accent_pale.with_alpha(focus);
    const float rx = right - 4.0f;
    c.list.line(rx - 7.0f, cy - 8.0f, rx, cy, 2.2f, ink);
    c.list.line(rx, cy, rx - 7.0f, cy + 8.0f, 2.2f, ink);
    const float lx = right - arrow - width - 16.0f;
    c.list.line(lx + 7.0f, cy - 8.0f, lx, cy, 2.2f, ink);
    c.list.line(lx, cy, lx + 7.0f, cy + 8.0f, 2.2f, ink);
    return width + arrow + 16.0f;
}

// ---------------------------------------------------------------- chips

namespace
{
constexpr float kChipHeight = 44.0f;
constexpr float kChipPad = 18.0f;

// What a chip has before its text: nothing, a dot, or the warning mark.
float chip_lead(Canvas &c, Tone tone)
{
    if (tone == Tone::accent)
        return 18.0f;
    if (tone == Tone::warning)
        return warning_mark(c, 0.0f, 0.0f, th().size_small, kClear, false);
    return 0.0f;
}
} // namespace

float chip_width(Canvas &c, std::string_view value, Tone tone, float max_width)
{
    const float lead = chip_lead(c, tone);
    return std::min(max_width, kChipPad * 2.0f + lead + text_width(c, value, th().size_small));
}

float chip(Canvas &c, std::string_view value, float x, float cy, Tone tone, float max_width)
{
    const Theme &t = th();
    const float width = chip_width(c, value, tone, max_width);
    const float height = look().large_text ? kChipHeight + 6.0f : kChipHeight;
    const Rect r{x, cy - height * 0.5f, width, height};
    const float radius = t.pill_chips ? height * 0.5f : t.radius_row * 0.6f;
    const Color ink = tone == Tone::warning ? t.warning : tone == Tone::accent ? t.accent_pale : t.body;
    if (look().high_contrast)
        c.list.bordered_rect(r, radius, gfx::mix(t.scrim, t.row, 0.6f), 1.5f, ink);
    else if (tone == Tone::warning)
        c.list.bordered_rect(r, radius, t.warning.with_alpha(0.12f), t.border, t.warning.with_alpha(0.55f));
    else if (tone == Tone::accent)
        c.list.bordered_rect(r, radius, t.accent.with_alpha(0.12f), t.border, t.accent.with_alpha(0.50f));
    else
        c.list.bordered_rect(r, radius, t.glass.with_alpha(0.74f), t.border, t.panel_edge.with_alpha(0.34f));
    const float line = baseline(r.y, r.h, t.size_small);
    float left = r.x + kChipPad;
    const float lead = chip_lead(c, tone);
    if (tone == Tone::accent)
        c.list.circle(left + 4.0f, cy, 4.0f, t.accent);
    else if (tone == Tone::warning)
        warning_mark(c, left, line, t.size_small, t.warning);
    left += lead;
    text_shrink(c, value, left, line, t.size_small, ink, r.x + r.w - kChipPad - left);
    return width;
}

// ---------------------------------------------------------------- icons

void icon(Canvas &c, Icon which, float cx, float cy, float size, Color ink)
{
    gfx::DrawList &list = c.list;
    const float u = size / 24.0f;
    const float x0 = cx - size * 0.5f;
    const float y0 = cy - size * 0.5f;
    const float w = std::max(1.6f, 1.8f * u);
    ink = text_ink(ink);
    const auto X = [&](float value) { return x0 + value * u; };
    const auto Y = [&](float value) { return y0 + value * u; };
    const auto stroke = [&](float ax, float ay, float bx, float by) { list.line(X(ax), Y(ay), X(bx), Y(by), w, ink); };
    const auto path = [&](std::initializer_list<float> points)
    {
        const float *p = points.begin();
        for (std::size_t i = 0; i + 3 < points.size(); i += 2)
            stroke(p[i], p[i + 1], p[i + 2], p[i + 3]);
    };
    const auto round = [&](float ax, float ay, float radius) { list.ring(X(ax), Y(ay), radius * u, w, ink); };
    const auto dot = [&](float ax, float ay, float radius) { list.circle(X(ax), Y(ay), radius * u, ink); };
    const auto box = [&](float ax, float ay, float bw, float bh, float radius)
    { list.bordered_rect({X(ax), Y(ay), bw * u, bh * u}, radius * u, ink.with_alpha(0.0f), w, ink); };
    const auto bow = [&](float ax, float ay, float radius, float start, float sweep)
    { arc(list, X(ax), Y(ay), radius * u, w, start, sweep, ink); };
    switch (which)
    {
    case Icon::home:
        path({3.0f, 11.5f, 12.0f, 4.0f, 21.0f, 11.5f});
        path({5.5f, 10.0f, 5.5f, 19.5f, 18.5f, 19.5f, 18.5f, 10.0f});
        return;
    case Icon::library:
        box(3.5f, 3.5f, 7.0f, 7.0f, 1.6f);
        box(13.5f, 3.5f, 7.0f, 7.0f, 1.6f);
        box(3.5f, 13.5f, 7.0f, 7.0f, 1.6f);
        box(13.5f, 13.5f, 7.0f, 7.0f, 1.6f);
        return;
    case Icon::multiplayer:
        round(8.5f, 8.5f, 3.2f);
        round(16.5f, 9.5f, 2.6f);
        bow(8.5f, 19.5f, 5.7f, kPi, kPi);
        bow(16.0f, 19.5f, 5.0f, 1.5f * kPi, 0.5f * kPi);
        return;
    case Icon::settings:
        stroke(4.0f, 7.0f, 13.0f, 7.0f);
        stroke(17.0f, 7.0f, 20.0f, 7.0f);
        round(15.0f, 7.0f, 2.0f);
        stroke(4.0f, 17.0f, 7.0f, 17.0f);
        stroke(11.0f, 17.0f, 20.0f, 17.0f);
        round(9.0f, 17.0f, 2.0f);
        return;
    case Icon::profile:
        round(12.0f, 8.5f, 3.6f);
        bow(12.0f, 20.5f, 7.5f, kPi + 0.12f, kPi - 0.24f);
        return;
    case Icon::video:
    case Icon::docked:
        box(3.0f, 4.5f, 18.0f, 12.0f, 2.0f);
        stroke(8.5f, 20.0f, 15.5f, 20.0f);
        stroke(12.0f, 16.5f, 12.0f, 20.0f);
        return;
    case Icon::performance:
        bow(12.0f, 15.5f, 8.5f, kPi, kPi);
        stroke(12.0f, 15.5f, 16.0f, 10.0f);
        dot(12.0f, 15.5f, 1.5f);
        return;
    case Icon::audio:
        path({4.0f, 10.0f, 4.0f, 14.0f, 7.5f, 14.0f, 12.0f, 18.0f, 12.0f, 6.0f, 7.5f, 10.0f, 4.0f, 10.0f});
        bow(12.0f, 12.0f, 4.2f, -0.25f * kPi, 0.5f * kPi);
        bow(12.0f, 12.0f, 7.6f, -0.25f * kPi, 0.5f * kPi);
        return;
    case Icon::controls:
        box(3.0f, 7.5f, 18.0f, 10.0f, 4.5f);
        stroke(8.0f, 10.8f, 8.0f, 14.2f);
        stroke(6.3f, 12.5f, 9.7f, 12.5f);
        dot(15.2f, 13.6f, 1.0f);
        dot(17.4f, 11.4f, 1.0f);
        return;
    case Icon::accessibility:
        dot(12.0f, 4.8f, 1.9f);
        stroke(5.0f, 9.0f, 19.0f, 9.0f);
        stroke(12.0f, 9.0f, 12.0f, 15.0f);
        stroke(12.0f, 15.0f, 8.5f, 20.5f);
        stroke(12.0f, 15.0f, 15.5f, 20.5f);
        return;
    case Icon::diagnostics:
        path({3.0f, 12.0f, 7.0f, 12.0f, 9.5f, 6.0f, 13.5f, 18.0f, 16.0f, 12.0f, 21.0f, 12.0f});
        return;
    case Icon::files:
        path({3.5f, 7.5f, 9.5f, 7.5f, 11.5f, 10.0f, 20.5f, 10.0f, 20.5f, 18.5f, 3.5f, 18.5f, 3.5f, 7.5f});
        return;
    case Icon::downloads:
        stroke(12.0f, 4.0f, 12.0f, 15.0f);
        path({7.5f, 11.0f, 12.0f, 15.5f, 16.5f, 11.0f});
        stroke(5.0f, 19.5f, 19.0f, 19.5f);
        return;
    case Icon::sync:
        bow(12.0f, 12.0f, 7.5f, 1.08f * kPi, 0.80f * kPi);
        path({18.6f, 4.0f, 18.6f, 7.8f, 14.8f, 7.8f});
        bow(12.0f, 12.0f, 7.5f, 0.08f * kPi, 0.80f * kPi);
        path({5.4f, 20.0f, 5.4f, 16.2f, 9.2f, 16.2f});
        return;
    case Icon::language:
        round(12.0f, 12.0f, 8.5f);
        stroke(3.5f, 12.0f, 20.5f, 12.0f);
        bow(4.2f, 12.0f, 11.5f, -0.265f * kPi, 0.53f * kPi);
        bow(19.8f, 12.0f, 11.5f, 0.735f * kPi, 0.53f * kPi);
        return;
    case Icon::theme:
        round(9.0f, 9.0f, 4.6f);
        round(15.0f, 9.0f, 4.6f);
        round(12.0f, 14.6f, 4.6f);
        return;
    case Icon::about:
        round(12.0f, 12.0f, 8.5f);
        stroke(12.0f, 11.0f, 12.0f, 17.0f);
        dot(12.0f, 7.6f, 1.2f);
        return;
    case Icon::handheld:
        box(2.0f, 7.0f, 20.0f, 10.0f, 3.0f);
        dot(5.5f, 12.0f, 1.1f);
        dot(18.5f, 12.0f, 1.1f);
        return;
    case Icon::check:
        path({5.0f, 12.5f, 9.5f, 17.0f, 19.0f, 7.5f});
        return;
    case Icon::lock:
        box(6.0f, 11.0f, 12.0f, 9.0f, 2.0f);
        bow(12.0f, 11.0f, 4.0f, kPi, kPi);
        return;
    }
}

// ---------------------------------------------------------------- controller hints

float pad_width(Pad button, float size)
{
    switch (button)
    {
    case Pad::none:
        return 0.0f;
    case Pad::l1:
    case Pad::r1:
    case Pad::l2:
    case Pad::r2:
        return size * 1.45f;
    case Pad::options:
    case Pad::create:
        return size * 1.15f;
    case Pad::touchpad:
        return size * 1.75f;
    default:
        return size;
    }
}

namespace
{

// light: the colour of the light bar beside the touchpad.
void pad_shape(Canvas &c, Pad button, float x, float cy, float size, Color ring, Color ink, Color dim, Color light)
{
    const float width = pad_width(button, size);
    const float cx = x + width * 0.5f;
    const float half = size * 0.5f;
    const float stroke = size * 0.085f;
    switch (button)
    {
    case Pad::none:
        return;
    case Pad::cross:
    {
        const float d = size * 0.17f;
        c.list.ring(cx, cy, half - 0.5f, 1.6f, ring);
        c.list.line(cx - d, cy - d, cx + d, cy + d, stroke, ink);
        c.list.line(cx - d, cy + d, cx + d, cy - d, stroke, ink);
        return;
    }
    case Pad::circle:
        c.list.ring(cx, cy, half - 0.5f, 1.6f, ring);
        c.list.ring(cx, cy, size * 0.21f, stroke, ink);
        return;
    case Pad::square:
    {
        const float side = size * 0.36f;
        c.list.ring(cx, cy, half - 0.5f, 1.6f, ring);
        c.list.bordered_rect({cx - side * 0.5f, cy - side * 0.5f, side, side}, 1.5f,
                             ink.with_alpha(0.0f), stroke, ink);
        return;
    }
    case Pad::triangle:
    {
        const float w = size * 0.46f;
        const float h = size * 0.40f;
        c.list.ring(cx, cy, half - 0.5f, 1.6f, ring);
        c.list.triangle({cx - w * 0.5f, cy - h * 0.60f, w, h}, ink, stroke);
        return;
    }
    case Pad::dpad:
    case Pad::updown:
    case Pad::leftright:
    {
        const float arm = size * 0.36f;
        const float thick = size * 0.26f;
        const Color vertical = button == Pad::leftright ? dim : ink;
        const Color horizontal = button == Pad::updown ? dim : ink;
        c.list.rounded_rect({cx - arm, cy - thick * 0.5f, arm * 2.0f, thick}, thick * 0.28f,
                            horizontal);
        c.list.rounded_rect({cx - thick * 0.5f, cy - arm, thick, arm * 2.0f}, thick * 0.28f,
                            vertical);
        if (button != Pad::dpad)
        {
            // Redraw the bright arm over the crossing so it reads as one piece.
            if (button == Pad::updown)
                c.list.rounded_rect({cx - thick * 0.5f, cy - arm, thick, arm * 2.0f},
                                    thick * 0.28f, ink);
            else
                c.list.rounded_rect({cx - arm, cy - thick * 0.5f, arm * 2.0f, thick},
                                    thick * 0.28f, ink);
        }
        return;
    }
    case Pad::l1:
    case Pad::r1:
    case Pad::l2:
    case Pad::r2:
    {
        // A shoulder button as a key cap: the bumpers low and wide, the triggers taller and rounder.
        const bool trigger = button == Pad::l2 || button == Pad::r2;
        const float height = size * (trigger ? 0.94f : 0.78f);
        const Rect cap{x, cy - height * 0.5f, width, height};
        const float radius = height * (trigger ? 0.44f : 0.32f);
        c.list.gradient_rect(cap, radius, ink.with_alpha(0.26f), ink.with_alpha(0.07f));
        c.list.bordered_rect(cap, radius, ink.with_alpha(0.0f), 1.6f, ring);
        // Light along its upper edge.
        c.list.line(cap.x + radius, cap.y + 3.0f, cap.x + cap.w - radius, cap.y + 3.0f, 1.0f, ink.with_alpha(0.38f));
        const char *name = button == Pad::l1 ? "L1" : button == Pad::r1 ? "R1" : button == Pad::l2 ? "L2" : "R2";
        const float before = c.weight;
        c.weight = th().weight_title;
        draw_text(c, name, cx, cy + size * 0.165f, size * 0.46f, size * 0.46f, ink, Align::center, 0.0f);
        c.weight = before;
        return;
    }
    case Pad::l3:
    case Pad::r3:
    {
        // A stick from above, pressed in: its well, its cap, its name.
        c.list.ring(cx, cy, half - 0.5f, 1.6f, ring);
        c.list.circle(cx, cy, size * 0.36f, ink.with_alpha(0.20f));
        c.list.ring(cx, cy, size * 0.36f, 1.2f, ink.with_alpha(0.55f));
        const float before = c.weight;
        c.weight = th().weight_title;
        draw_text(c, button == Pad::l3 ? "L3" : "R3", cx, cy + size * 0.125f, size * 0.36f, size * 0.36f, ink,
                  Align::center, 0.0f);
        c.weight = before;
        return;
    }
    case Pad::options:
    case Pad::create:
    {
        // The two small buttons beside the touchpad: a capsule with its sign.
        const float height = size * 0.62f;
        const Rect cap{x, cy - height * 0.5f, width, height};
        c.list.gradient_rect(cap, height * 0.5f, ink.with_alpha(0.24f), ink.with_alpha(0.07f));
        c.list.bordered_rect(cap, height * 0.5f, ink.with_alpha(0.0f), 1.6f, ring);
        if (button == Pad::options)
        {
            // Three lines.
            for (int row = -1; row <= 1; ++row)
                c.list.line(cx - size * 0.20f, cy + static_cast<float>(row) * size * 0.13f, cx + size * 0.20f,
                            cy + static_cast<float>(row) * size * 0.13f, stroke * 0.8f, ink);
        }
        else
        {
            // Three rays from a point.
            const float foot = cy + size * 0.17f;
            c.list.line(cx, foot, cx, cy - size * 0.19f, stroke * 0.8f, ink);
            c.list.line(cx - size * 0.08f, foot, cx - size * 0.24f, cy - size * 0.10f, stroke * 0.8f, ink);
            c.list.line(cx + size * 0.08f, foot, cx + size * 0.24f, cy - size * 0.10f, stroke * 0.8f, ink);
        }
        return;
    }
    case Pad::touchpad:
    {
        // The touchpad: a wide plate with the light bar at either side of it.
        const float height = size * 0.86f;
        const Rect plate{x + size * 0.14f, cy - height * 0.5f, width - size * 0.28f, height};
        const float radius = height * 0.28f;
        c.list.gradient_rect(plate, radius, ink.with_alpha(0.26f), ink.with_alpha(0.06f));
        c.list.bordered_rect(plate, radius, ink.with_alpha(0.0f), 1.6f, ring);
        c.list.line(plate.x + radius, plate.y + 3.0f, plate.x + plate.w - radius, plate.y + 3.0f, 1.0f,
                    ink.with_alpha(0.38f));
        for (const float side : {x + size * 0.05f, x + width - size * 0.05f})
            c.list.line(side, cy - height * 0.24f, side, cy + height * 0.24f, std::max(2.0f, size * 0.07f), light);
        return;
    }
    }
}

} // namespace

void draw_pad(Canvas &c, Pad button, float x, float cy, float size, float alpha)
{
    const bool bold = look().high_contrast;
    const Color ring = th().text.with_alpha((bold ? 0.85f : 0.40f) * alpha);
    const Color ink = th().text.with_alpha((bold ? 1.0f : 0.92f) * alpha);
    const Color dim = th().text.with_alpha((bold ? 0.45f : 0.30f) * alpha);
    pad_shape(c, button, x, cy, size, ring, ink, dim, th().accent.with_alpha(alpha));
}

void draw_pad_ink(Canvas &c, Pad button, float x, float cy, float size, Color ink)
{
    pad_shape(c, button, x, cy, size, ink.with_alpha(0.75f), ink, ink.with_alpha(0.4f), ink);
}

float draw_hints(Canvas &c, const Hint *hints, int count, float x, float cy, Color color,
                 float max_width)
{
    constexpr float kSize = 28.0f;
    constexpr float kIconGap = 10.0f;
    constexpr float kPairGap = 6.0f;
    constexpr float kItemGap = 34.0f;
    const float size = th().size_small;
    // Laid out once to measure, once to draw.
    const auto run = [&](bool draw)
    {
        float cursor = x;
        for (int i = 0; i < count; ++i)
        {
            const Hint &hint = hints[i];
            if (draw)
                draw_pad(c, hint.button, cursor, cy, kSize);
            cursor += pad_width(hint.button, kSize);
            if (hint.second != Pad::none)
            {
                cursor += kPairGap;
                if (draw)
                    draw_pad(c, hint.second, cursor, cy, kSize);
                cursor += pad_width(hint.second, kSize);
            }
            cursor += kIconGap;
            cursor += draw ? text(c, tr(hint.label), cursor, cy + size * 0.35f, size, color) :
                             text_width(c, tr(hint.label), size);
            if (i + 1 < count)
                cursor += kItemGap;
        }
        return cursor - x;
    };
    const float width = run(false);
    if (max_width <= 0.0f || width <= max_width)
        return run(true);
    const float scale = max_width / width;
    c.list.push_transform(scale, x, cy, 0.0f, 0.0f);
    run(true);
    c.list.pop_transform();
    return max_width;
}

// ---------------------------------------------------------------- buttons

namespace
{

// The button's glyph and its words as one group in the middle of r.
void button_face(Canvas &c, const Rect &r, std::string_view label, Pad button, Color ink, bool own_ink)
{
    const float size = r.h >= 80.0f ? 28.0f : th().size_text;
    const float glyph = button == Pad::none ? 0.0f : std::min(40.0f, r.h * 0.46f);
    const float gap = button == Pad::none ? 0.0f : 16.0f;
    const float room = r.w - 48.0f - glyph - gap;
    const float words = std::min(room, text_width(c, label, size));
    const float left = r.x + (r.w - (glyph + gap + words)) * 0.5f;
    if (button != Pad::none)
    {
        if (own_ink)
            draw_pad_ink(c, button, left, r.y + r.h * 0.5f, glyph, ink);
        else
            draw_pad(c, button, left, r.y + r.h * 0.5f, glyph);
    }
    text_shrink(c, label, left + glyph + gap, baseline(r.y, r.h, size), size, ink, room);
}

} // namespace

void primary_button(Canvas &c, const Rect &r, std::string_view label, Pad button, float focus, float press,
                    bool enabled)
{
    const Theme &t = th();
    gfx::DrawList &list = c.list;
    list.push_opacity(enabled ? 1.0f : 0.4f);
    const float scale = 1.0f + (0.03f * focus - 0.035f * press * focus) * motion();
    list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, 0.0f);
    focus_ring(c, r, t.radius_row, focus);
    if (look().high_contrast)
        list.bordered_rect(r, t.radius_row, t.accent, 2.0f, t.title);
    else
        list.hgradient_rect(r, t.radius_row, gfx::mix(t.accent, kWhite, 0.24f),
                            gfx::mix(t.accent, t.accent_deep, 0.22f));
    // Light on the upper half of its face.
    list.gradient_rect({r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, r.h * 0.5f}, t.radius_row, kWhite.with_alpha(0.16f),
                       kWhite.with_alpha(0.0f));
    {
        const Heavy heavy(c);
        // On the accent the ink is the theme's own, whatever the contrast setting.
        const bool contrast = look().high_contrast;
        look().high_contrast = false;
        button_face(c, r, label, button, t.on_accent, true);
        look().high_contrast = contrast;
    }
    list.pop_transform();
    list.pop_opacity();
}

void secondary_button(Canvas &c, const Rect &r, std::string_view label, Pad button, float focus, float press,
                      bool enabled)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(enabled ? 1.0f : 0.4f);
    const float scale = 1.0f + (0.03f * focus - 0.035f * press * focus) * motion();
    list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, 0.0f);
    plate(c, button_plate(), r, focus);
    button_face(c, r, label, button, gfx::mix(th().text, th().title, focus), false);
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------- lists

std::string list_position(int selected, int count)
{
    return fill(tr("{0} OF {1}"), {std::to_string(selected), std::to_string(count)});
}

void ListView::follow()
{
    const int max_top = std::max(0, count - visible);
    if (selected < top_)
        top_ = selected;
    if (selected > top_ + visible - 1)
        top_ = selected - (visible - 1);
    top_ = std::clamp(top_, 0, max_top);
    scroll_.target = static_cast<float>(top_) * pitch;
    cursor_.target = static_cast<float>(selected) * pitch;
}

void ListView::reset(int new_count, int new_selected)
{
    count = std::max(0, new_count);
    selected = std::clamp(new_selected, 0, std::max(0, count - 1));
    // Open with the selection a few rows down when the list is long enough.
    top_ = std::max(0, selected - visible / 2);
    follow();
    scroll_.snap(scroll_.target);
    cursor_.snap(cursor_.target);
}

bool ListView::move(int delta)
{
    if (count <= 0)
        return false;
    int next = selected + delta;
    if (wrap)
        next = (next % count + count) % count;
    else
        next = std::clamp(next, 0, count - 1);
    if (next == selected)
        return false;
    selected = next;
    follow();
    return true;
}

bool ListView::page(int delta)
{
    if (count <= 0)
        return false;
    const int next = std::clamp(selected + delta * visible, 0, count - 1);
    if (next == selected)
        return false;
    selected = next;
    follow();
    return true;
}

bool ListView::select(int row)
{
    if (count <= 0)
        return false;
    const int next = std::clamp(row, 0, count - 1);
    if (next == selected)
        return false;
    selected = next;
    follow();
    return true;
}

void ListView::update(float dt)
{
    scroll_.update(dt, th().scroll_spring);
    cursor_.update(dt, th().cursor_spring);
}

int ListView::first_row() const
{
    return std::max(0, static_cast<int>(std::floor(scroll_.value / pitch)));
}

int ListView::last_row() const
{
    return std::min(count - 1, static_cast<int>(std::floor(scroll_.value / pitch)) + visible);
}

float ListView::row_alpha(int row, float row_height) const
{
    const float y = static_cast<float>(row) * pitch - scroll_.value;
    const float window = static_cast<float>(visible - 1) * pitch + row_height;
    const float above = tween::clamp01((y + row_height) / row_height);
    const float below = tween::clamp01((window - y) / row_height);
    return std::min(above, below);
}

float ListView::thumb() const
{
    const float range = static_cast<float>(std::max(1, count - visible)) * pitch;
    return tween::clamp01(scroll_.value / range);
}

void scrollbar(Canvas &c, const ListView &view, float x, float y, float height)
{
    if (view.count <= view.visible)
        return;
    c.list.rounded_rect({x, y, 8.0f, height}, 4.0f, th().rule.with_alpha(0.55f));
    const float thumb = std::max(
        64.0f, height * static_cast<float>(view.visible) / static_cast<float>(view.count));
    c.list.rounded_rect({x, y + (height - thumb) * view.thumb(), 8.0f, thumb}, 4.0f, th().accent_pale);
}

void arc(gfx::DrawList &list, float cx, float cy, float radius, float width, float start, float sweep, Color color)
{
    if (sweep <= 0.001f)
        return;
    const int steps = std::max(2, static_cast<int>(sweep / (kPi / 60.0f)));
    float x = cx + radius * std::cos(start);
    float y = cy + radius * std::sin(start);
    for (int i = 1; i <= steps; ++i)
    {
        const float a = start + sweep * static_cast<float>(i) / static_cast<float>(steps);
        const float nx = cx + radius * std::cos(a);
        const float ny = cy + radius * std::sin(a);
        list.line(x, y, nx, ny, width, color);
        x = nx;
        y = ny;
    }
}

} // namespace pe::ui
