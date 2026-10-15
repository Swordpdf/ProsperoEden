// ProsperoEden - 2D draw list: instanced shapes, glyphs and images.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/gfx/draw_list.hpp"

#include <algorithm>
#include <cmath>

namespace pe::gfx
{

namespace
{

Rect intersect(const Rect &a, const Rect &b)
{
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool same_clip(const Run &run, bool clipped, const Rect &clip)
{
    if (run.clipped != clipped)
        return false;
    return !clipped || (run.clip.x == clip.x && run.clip.y == clip.y && run.clip.w == clip.w &&
                        run.clip.h == clip.h);
}

void set4(float *out, float a, float b, float c, float d)
{
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

} // namespace

void DrawList::clear()
{
    instances_.clear();
    runs_.clear();
    reset_state();
}

void DrawList::reset_state()
{
    clips_.clear();
    transforms_.clear();
    opacities_.clear();
    transform_ = Transform{};
    opacity_ = 1.0f;
}

Instance &DrawList::append(std::uint32_t texture)
{
    const bool clipped = !clips_.empty();
    const Rect clip = clipped ? clips_.back() : Rect{};
    if (runs_.empty() ||
        (runs_.back().texture != texture && texture != 0 && runs_.back().texture != 0) ||
        !same_clip(runs_.back(), clipped, clip))
    {
        Run run;
        run.first = static_cast<std::uint32_t>(instances_.size());
        run.texture = texture;
        run.clipped = clipped;
        run.clip = clip;
        runs_.push_back(run);
    }
    Run &run = runs_.back();
    if (texture != 0)
        run.texture = texture; // untextured instances can share a textured run
    ++run.count;
    instances_.push_back(Instance{});
    return instances_.back();
}

Rect DrawList::apply(const Rect &r) const
{
    return {apply_x(r.x), apply_y(r.y), r.w * transform_.scale, r.h * transform_.scale};
}

void DrawList::set_color(float *out, Color c) const
{
    set4(out, c.r, c.g, c.b, c.a * opacity_);
}

Instance &DrawList::box(Shape shape, const Rect &r, float radius, Color first, Color second)
{
    Instance &i = append(0);
    const Rect t = apply(r);
    set4(i.rect, t.x, t.y, t.w, t.h);
    set_color(i.color_top, first);
    set_color(i.color_bottom, second);
    set4(i.params, radius * transform_.scale, 0.0f, 0.0f, static_cast<float>(shape));
    return i;
}

void DrawList::rounded_rect(const Rect &r, float radius, Color fill)
{
    box(Shape::rounded_rect, r, radius, fill, fill);
}

void DrawList::gradient_rect(const Rect &r, float radius, Color top, Color bottom)
{
    box(Shape::rounded_rect, r, radius, top, bottom);
}

void DrawList::hgradient_rect(const Rect &r, float radius, Color left, Color right, float border,
                              Color border_color)
{
    Instance &i = box(Shape::rounded_rect_h, r, radius, left, right);
    set_color(i.border_color, border_color);
    i.params[1] = border * transform_.scale;
}

void DrawList::bordered_rect(const Rect &r, float radius, Color fill, float border,
                             Color border_color)
{
    Instance &i = box(Shape::rounded_rect, r, radius, fill, fill);
    set_color(i.border_color, border_color);
    i.params[1] = border * transform_.scale;
}

void DrawList::shadow(const Rect &r, float radius, float softness, Color color)
{
    Instance &i = box(Shape::shadow, r, radius, color, color);
    i.params[2] = std::max(0.5f, softness * transform_.scale);
}

void DrawList::circle(float cx, float cy, float radius, Color fill)
{
    rounded_rect({cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}, radius, fill);
}

void DrawList::ring(float cx, float cy, float radius, float thickness, Color color)
{
    bordered_rect({cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}, radius,
                  Color{color.r, color.g, color.b, 0.0f}, thickness, color);
}

void DrawList::line(float x1, float y1, float x2, float y2, float thickness, Color color)
{
    Instance &i = append(0);
    const float half = thickness * 0.5f;
    const Rect bounds{std::min(x1, x2) - half, std::min(y1, y2) - half,
                      std::fabs(x2 - x1) + thickness, std::fabs(y2 - y1) + thickness};
    const Rect t = apply(bounds);
    set4(i.rect, t.x, t.y, t.w, t.h);
    set_color(i.color_top, color);
    set_color(i.color_bottom, color);
    set4(i.params, 0.0f, thickness * transform_.scale, 0.0f, static_cast<float>(Shape::capsule));
    set4(i.extra, apply_x(x1), apply_y(y1), apply_x(x2), apply_y(y2));
}

void DrawList::triangle(const Rect &r, Color fill, float outline)
{
    Instance &i = box(Shape::triangle, r, 0.0f, fill, fill);
    i.params[1] = outline * transform_.scale;
}

void DrawList::image(std::uint32_t texture, const Rect &r, const Rect &uv, Color tint)
{
    Instance &i = append(texture);
    const Rect t = apply(r);
    set4(i.rect, t.x, t.y, t.w, t.h);
    set_color(i.color_top, tint);
    set_color(i.color_bottom, tint);
    set4(i.params, 0.0f, 0.0f, 0.0f, static_cast<float>(Shape::image));
    set4(i.extra, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
}

void DrawList::rounded_image(std::uint32_t texture, const Rect &r, const Rect &uv, float radius,
                             Color tint)
{
    Instance &i = append(texture);
    const Rect t = apply(r);
    set4(i.rect, t.x, t.y, t.w, t.h);
    set_color(i.color_top, tint);
    set_color(i.color_bottom, tint);
    set4(i.params, radius * transform_.scale, 0.0f, 0.0f,
         static_cast<float>(Shape::rounded_image));
    set4(i.extra, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
}

float DrawList::text(const Font &font, std::uint32_t font_texture, std::string_view text, float x,
                     float baseline, const TextStyle &style)
{
    glyph_scratch_.clear();
    const float width =
        font.layout(text, x, baseline, style.size, style.align, glyph_scratch_, style.tracking);
    const float range = font.sdf_range(style.size) * transform_.scale;
    for (const GlyphQuad &q : glyph_scratch_)
    {
        Instance &i = append(font_texture);
        const Rect t = apply({q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0});
        set4(i.rect, t.x, t.y, t.w, t.h);
        set_color(i.color_top, style.color);
        set_color(i.color_bottom, style.color);
        set4(i.params, range, style.weight * transform_.scale, 0.0f,
             static_cast<float>(Shape::glyph));
        set4(i.extra, q.u0, q.v0, q.u1, q.v1);
    }
    return width;
}

float DrawList::text(const Font &font, std::uint32_t font_texture, std::string_view text, float x,
                     float baseline, float size, Color color, Align align)
{
    TextStyle style;
    style.size = size;
    style.color = color;
    style.align = align;
    return this->text(font, font_texture, text, x, baseline, style);
}

void DrawList::push_clip(const Rect &r)
{
    const Rect transformed = apply(r);
    clips_.push_back(clips_.empty() ? transformed : intersect(clips_.back(), transformed));
}

void DrawList::pop_clip()
{
    if (!clips_.empty())
        clips_.pop_back();
}

void DrawList::push_transform(float scale, float origin_x, float origin_y, float dx, float dy)
{
    transforms_.push_back(transform_);
    // New local mapping: p -> origin + (p - origin) * scale + d, then the parent.
    const float local_dx = origin_x - origin_x * scale + dx;
    const float local_dy = origin_y - origin_y * scale + dy;
    Transform next;
    next.scale = transform_.scale * scale;
    next.dx = transform_.dx + transform_.scale * local_dx;
    next.dy = transform_.dy + transform_.scale * local_dy;
    transform_ = next;
}

void DrawList::pop_transform()
{
    if (!transforms_.empty())
    {
        transform_ = transforms_.back();
        transforms_.pop_back();
    }
}

void DrawList::push_opacity(float opacity)
{
    opacities_.push_back(opacity_);
    opacity_ *= std::clamp(opacity, 0.0f, 1.0f);
}

void DrawList::pop_opacity()
{
    if (!opacities_.empty())
    {
        opacity_ = opacities_.back();
        opacities_.pop_back();
    }
}

Viewport fit_viewport(int surface_width, int surface_height, float virtual_width,
                      float virtual_height)
{
    Viewport viewport;
    const float sx = static_cast<float>(surface_width) / virtual_width;
    const float sy = static_cast<float>(surface_height) / virtual_height;
    viewport.scale = std::min(sx, sy);
    viewport.offset_x = (static_cast<float>(surface_width) - virtual_width * viewport.scale) * 0.5f;
    viewport.offset_y =
        (static_cast<float>(surface_height) - virtual_height * viewport.scale) * 0.5f;
    return viewport;
}

} // namespace pe::gfx
