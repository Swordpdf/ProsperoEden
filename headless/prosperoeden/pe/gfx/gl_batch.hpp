// ProsperoEden - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/draw_list.hpp"
#include "pe/gfx/font.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>

namespace pe::gfx
{

// Draws a DrawList with one instanced program: all instances are uploaded
// once per frame into an orphaned stream buffer, then each run is one
// glDrawArraysInstancedBaseInstance call (triangle lists only).
class GlBatch
{
  public:
    GlBatch() = default;
    GlBatch(const GlBatch &) = delete;
    GlBatch &operator=(const GlBatch &) = delete;
    ~GlBatch();

    bool init();
    // Deletes the program, vertex array and buffer; init() recreates them.
    void release();
    // Uploads a font atlas as a single-level R8 texture; returns its name.
    std::uint32_t create_font_texture(const Font &font);
    // Uploads the atlas rows that changed since the last call (glyphs of other scripts are drawn
    // into the atlas as text needs them). Call it after a frame's text is laid out, before draw().
    void sync_font_texture(std::uint32_t texture, const Font &font);
    // Uploads RGBA8 pixels as a single-level texture; returns its name.
    std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba);
    // Uploads BC7 blocks as a single-level texture; returns its name, or 0 when the graphics
    // driver does not take them.
    std::uint32_t create_bc7_texture(int width, int height, const std::uint8_t *blocks, std::size_t size);
    void delete_texture(std::uint32_t texture);

    // Draws into the currently bound framebuffer of the given size.
    void draw(const DrawList &list, const Viewport &viewport, int surface_width,
              int surface_height);

    std::size_t last_draw_calls() const
    {
        return draw_calls_;
    }

  private:
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint buffer_ = 0;
    std::size_t capacity_ = 0; // instances
    std::size_t draw_calls_ = 0;
};

} // namespace pe::gfx
