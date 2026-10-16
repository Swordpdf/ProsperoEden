// ProsperoEden - 2D draw list: instanced shapes, glyphs and images.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/font.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace pe::gfx
{

struct Color
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static constexpr Color rgb(std::uint32_t hex, float alpha = 1.0f)
    {
        return {static_cast<float>((hex >> 16) & 0xff) / 255.0f,
                static_cast<float>((hex >> 8) & 0xff) / 255.0f,
                static_cast<float>(hex & 0xff) / 255.0f, alpha};
    }
    Color with_alpha(float alpha) const
    {
        return {r, g, b, a * alpha};
    }
};

inline Color mix(Color a, Color b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

struct Rect
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Shape modes understood by the batch shader (keep in sync with gl_batch.cpp).
enum class Shape : std::uint8_t
{
    rounded_rect = 0,   // fill (vertical gradient) plus optional border
    glyph = 1,          // SDF text from the run's texture
    shadow = 2,         // soft rounded-rect falloff
    image = 3,          // texture sampled over the rect, tinted
    capsule = 4,        // line segment with round caps
    triangle = 5,       // isosceles triangle pointing up inside the rect
    rounded_rect_h = 7, // rounded_rect with a left-to-right gradient
    rounded_image = 8,  // image clipped to a rounded rect
};

// One instanced quad; field order matches the vertex attributes.
struct Instance
{
    float rect[4];         // x, y, w, h in virtual pixels
    float color_top[4];    // rgba, straight alpha (top, or left for rounded_rect_h)
    float color_bottom[4]; // rgba (bottom, or right)
    float border_color[4]; // rgba
    float params[4];       // radius|range, border|thickness|weight, softness, shape
    float extra[4];        // uv rect (image/glyph) or segment endpoints (capsule)
};
static_assert(sizeof(Instance) == 96);

// Consecutive instances sharing a texture and clip rectangle.
struct Run
{
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t texture = 0; // opaque GL texture name, 0 for none
    bool clipped = false;
    Rect clip; // virtual pixels
};

// How one line of text is drawn.
struct TextStyle
{
    float size = 24.0f;
    Color color;
    Align align = Align::left;
    float tracking = 0.0f; // extra advance after every glyph, in pixels at `size`
    float weight = 0.0f;   // grows (or shrinks) the glyph outline, in pixels
};

// Records a frame of 2D drawing in a virtual coordinate space (1920x1080 by
// default). Transforms (for transitions) apply on the CPU; clip rectangles and
// texture changes start new runs. The GL backend uploads once per frame.
class DrawList
{
  public:
    void clear();
    void reset_state(); // Retain recorded geometry, reset clip/transform/opacity stacks.

    // ---- shapes ----
    void rounded_rect(const Rect &r, float radius, Color fill);
    void gradient_rect(const Rect &r, float radius, Color top, Color bottom);
    // Left-to-right gradient, with an optional border.
    void hgradient_rect(const Rect &r, float radius, Color left, Color right, float border = 0.0f,
                        Color border_color = {});
    void bordered_rect(const Rect &r, float radius, Color fill, float border, Color border_color);
    void shadow(const Rect &r, float radius, float softness, Color color);
    void circle(float cx, float cy, float radius, Color fill);
    void ring(float cx, float cy, float radius, float thickness, Color color);
    void line(float x1, float y1, float x2, float y2, float thickness, Color color);
    // Upward triangle filling r; outline > 0 draws only a stroke of that width.
    void triangle(const Rect &r, Color fill, float outline = 0.0f);
    void image(std::uint32_t texture, const Rect &r, const Rect &uv, Color tint);
    // The image clipped to a rounded rectangle (covers, frosted panels).
    void rounded_image(std::uint32_t texture, const Rect &r, const Rect &uv, float radius,
                       Color tint);

    // ---- text ----
    // font_texture is the GL texture holding font's atlas. Returns the advance.
    float text(const Font &font, std::uint32_t font_texture, std::string_view text, float x,
               float baseline, const TextStyle &style);
    float text(const Font &font, std::uint32_t font_texture, std::string_view text, float x,
               float baseline, float size, Color color, Align align = Align::left);

    // ---- state ----
    void push_clip(const Rect &r); // intersected with the current clip
    void pop_clip();
    // Scales about (origin_x, origin_y), then translates; composes with the
    // current transform. Used for zooms, slides and fades.
    void push_transform(float scale, float origin_x, float origin_y, float dx, float dy);
    void pop_transform();
    void push_opacity(float opacity); // multiplies every colour's alpha
    void pop_opacity();
    float opacity() const
    {
        return opacity_;
    }

    const std::vector<Instance> &instances() const
    {
        return instances_;
    }
    const std::vector<Run> &runs() const
    {
        return runs_;
    }

  private:
    struct Transform
    {
        float scale = 1.0f;
        float dx = 0.0f;
        float dy = 0.0f;
    };

    Instance &append(std::uint32_t texture);
    Instance &box(Shape shape, const Rect &r, float radius, Color first, Color second);
    Rect apply(const Rect &r) const;
    float apply_x(float x) const
    {
        return x * transform_.scale + transform_.dx;
    }
    float apply_y(float y) const
    {
        return y * transform_.scale + transform_.dy;
    }
    void set_color(float *out, Color c) const;

    std::vector<Instance> instances_;
    std::vector<Run> runs_;
    std::vector<Rect> clips_;
    std::vector<Transform> transforms_;
    std::vector<float> opacities_;
    Transform transform_;
    float opacity_ = 1.0f;
    std::vector<GlyphQuad> glyph_scratch_;
};

// Letterboxes a virtual canvas into a surface: surface = virtual * scale + offset.
struct Viewport
{
    float scale = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};
Viewport fit_viewport(int surface_width, int surface_height, float virtual_width = 1920.0f,
                      float virtual_height = 1080.0f);

} // namespace pe::gfx
