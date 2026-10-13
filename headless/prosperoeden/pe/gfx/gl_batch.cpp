// ProsperoEden - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/gfx/gl_batch.hpp"

#include "pe/gfx/gl_program.hpp"

#include <algorithm>
#include <cmath>

namespace pe::gfx
{

namespace
{

constexpr const char *kVertex = R"(
layout(location = 0) in vec4 a_rect;
layout(location = 1) in vec4 a_top;
layout(location = 2) in vec4 a_bottom;
layout(location = 3) in vec4 a_border;
layout(location = 4) in vec4 a_params;
layout(location = 5) in vec4 a_extra;
layout(location = 0) uniform vec4 u_viewport; // scale, offset x, offset y
layout(location = 1) uniform vec2 u_surface;
out vec2 v_local;
out vec2 v_virtual;
out vec2 v_uv;
out vec2 v_st;
flat out vec2 v_half;
flat out vec4 v_top;
flat out vec4 v_bottom;
flat out vec4 v_border;
flat out vec4 v_params;
flat out vec4 v_extra;
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                 vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main()
{
    vec2 corner = kCorners[gl_VertexID];
    int shape = int(a_params.w + 0.5);
    vec2 size = a_rect.zw;
    // Grow shapes by their softness plus one output pixel for anti-aliasing;
    // glyph and image quads map texels exactly and are not grown.
    float pad = (shape == 1 || shape == 3) ? 0.0 : a_params.z + 1.0 / u_viewport.x;
    vec2 local = mix(vec2(-pad), size + vec2(pad), corner);
    vec2 virt = a_rect.xy + local;
    vec2 unit = local / max(size, vec2(1e-3));
    v_virtual = virt;
    v_local = local - 0.5 * size;
    v_half = 0.5 * size;
    v_st = clamp(unit, 0.0, 1.0);
    v_uv = mix(a_extra.xy, a_extra.zw, unit);
    v_top = a_top;
    v_bottom = a_bottom;
    v_border = a_border;
    v_params = a_params;
    v_extra = a_extra;
    vec2 surface = virt * u_viewport.x + u_viewport.yz;
    vec2 ndc = surface / u_surface * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
layout(location = 0) uniform vec4 u_viewport;
layout(location = 2) uniform sampler2D u_texture;
in vec2 v_local;
in vec2 v_virtual;
in vec2 v_uv;
in vec2 v_st;
flat in vec2 v_half;
flat in vec4 v_top;
flat in vec4 v_bottom;
flat in vec4 v_border;
flat in vec4 v_params;
flat in vec4 v_extra;
out vec4 frag_color;

float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float triangle(vec2 p, vec2 p0, vec2 p1, vec2 p2)
{
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 q1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 q2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

void main()
{
    float px = 1.0 / u_viewport.x; // virtual units per output pixel
    int shape = int(v_params.w + 0.5);
    vec4 fill = mix(v_top, v_bottom, shape == 7 ? v_st.x : v_st.y);
    vec4 color;
    if (shape == 0 || shape == 7)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = fill;
        if (v_params.y > 0.0)
        {
            float inner = clamp(0.5 - (d + v_params.y) / px, 0.0, 1.0);
            color = mix(v_border, fill, inner);
        }
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 1)
    {
        float sdf = texture(u_texture, v_uv).r;
        float distance = (sdf - 0.5) * 2.0 * v_params.x + v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(distance / px + 0.5, 0.0, 1.0));
    }
    else if (shape == 2)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = vec4(fill.rgb, fill.a * (1.0 - smoothstep(-v_params.z, v_params.z, d)));
    }
    else if (shape == 3)
    {
        color = texture(u_texture, v_uv) * fill;
    }
    else if (shape == 8)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = texture(u_texture, v_uv) * fill;
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 4)
    {
        vec2 pa = v_virtual - v_extra.xy;
        vec2 ba = v_extra.zw - v_extra.xy;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(pa - ba * h) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else
    {
        float d = triangle(v_local, vec2(0.0, -v_half.y), vec2(-v_half.x, v_half.y),
                           vec2(v_half.x, v_half.y));
        if (v_params.y > 0.0)
            d = abs(d + 0.5 * v_params.y) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    if (color.a <= 0.0)
        discard;
    frag_color = color;
}
)";

} // namespace

GlBatch::~GlBatch()
{
    release();
}

void GlBatch::release()
{
    if (buffer_ != 0)
        glDeleteBuffers(1, &buffer_);
    if (vao_ != 0)
        glDeleteVertexArrays(1, &vao_);
    if (program_ != 0)
        glDeleteProgram(program_);
    buffer_ = vao_ = program_ = 0;
    capacity_ = 0;
}

bool GlBatch::init()
{
    program_ = build_program("batch2d", kVertex, kFragment);
    if (program_ == 0)
        return false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &buffer_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    constexpr GLsizei stride = sizeof(Instance);
    for (GLuint attribute = 0; attribute < 6; ++attribute)
    {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(
            attribute, 4, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<const void *>(static_cast<std::uintptr_t>(attribute * 16)));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);
    return true;
}

std::uint32_t GlBatch::create_font_texture(const Font &font)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, font.atlas_width(), font.atlas_height(), 0, GL_RED,
                 GL_UNSIGNED_BYTE, font.atlas().data());
    // One level only: mip chains leave ps5-opengl's batched fast path.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void GlBatch::sync_font_texture(std::uint32_t texture, const Font &font)
{
    int first = 0;
    int last = 0;
    if (texture == 0 || !font.take_changed_rows(&first, &last))
        return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, font.atlas_width(), last - first, GL_RED, GL_UNSIGNED_BYTE,
                    font.atlas().data() + static_cast<std::size_t>(first) * font.atlas_width());
}

std::uint32_t GlBatch::create_texture(int width, int height, const std::uint8_t *rgba)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

std::uint32_t GlBatch::create_bc7_texture(int width, int height, const std::uint8_t *blocks, std::size_t size)
{
    constexpr GLenum kBc7 = 0x8E8C; // GL_COMPRESSED_RGBA_BPTC_UNORM
    while (glGetError() != GL_NO_ERROR)
    {
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, kBc7, width, height, 0, static_cast<GLsizei>(size), blocks);
    if (glGetError() != GL_NO_ERROR)
    {
        glDeleteTextures(1, &texture);
        return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void GlBatch::delete_texture(std::uint32_t texture)
{
    if (texture != 0)
        glDeleteTextures(1, &texture);
}

void GlBatch::draw(const DrawList &list, const Viewport &viewport, int surface_width,
                   int surface_height)
{
    draw_calls_ = 0;
    const auto &instances = list.instances();
    if (instances.empty())
        return;
    // Orphan, then fill: re-specifying storage avoids waiting on the GPU's
    // reads of the previous frame (ps5-opengl drains on same-size reuse).
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    capacity_ = std::max(capacity_, instances.size());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_ * sizeof(Instance)), nullptr,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(instances.size() * sizeof(Instance)),
                    instances.data());

    glViewport(0, 0, surface_width, surface_height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(program_);
    glUniform4f(0, viewport.scale, viewport.offset_x, viewport.offset_y, 0.0f);
    glUniform2f(1, static_cast<float>(surface_width), static_cast<float>(surface_height));
    glUniform1i(2, 0);
    glActiveTexture(GL_TEXTURE0);

    GLuint bound_texture = 0;
    bool scissor = false;
    glBindVertexArray(vao_);
    for (const Run &run : list.runs())
    {
        if (run.count == 0)
            continue;
        if (run.texture != 0 && run.texture != bound_texture)
        {
            glBindTexture(GL_TEXTURE_2D, run.texture);
            bound_texture = run.texture;
        }
        if (run.clipped)
        {
            const float x0 = std::floor(run.clip.x * viewport.scale + viewport.offset_x);
            const float y0 = std::floor(run.clip.y * viewport.scale + viewport.offset_y);
            const float x1 =
                std::ceil((run.clip.x + run.clip.w) * viewport.scale + viewport.offset_x);
            const float y1 =
                std::ceil((run.clip.y + run.clip.h) * viewport.scale + viewport.offset_y);
            if (!scissor)
                glEnable(GL_SCISSOR_TEST);
            scissor = true;
            glScissor(static_cast<GLint>(x0), surface_height - static_cast<GLint>(y1),
                      static_cast<GLsizei>(std::max(0.0f, x1 - x0)),
                      static_cast<GLsizei>(std::max(0.0f, y1 - y0)));
        }
        else if (scissor)
        {
            glDisable(GL_SCISSOR_TEST);
            scissor = false;
        }
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(run.count),
                                          run.first);
        ++draw_calls_;
    }
    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

} // namespace pe::gfx
