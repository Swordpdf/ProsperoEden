// ProsperoEden - The game loading screen on a PC: draws loading_scene.glsl to PNG files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: loading_preview <headless dir> <output dir> [width height [cover.tga]]
// The shader is compiled as the OpenGL renderer compiles it on the console (graphics.cpp: a
// "#version 330 core" line, the picture's sampler, loading_text.glsl, loading_scene.glsl, the
// uniforms and a main()), and drawn at a few moments of a game's start through Mesa's surfaceless
// EGL. The picture is built by the console's own code (loading_art.h) from the cover given, a TGA
// as the launcher keeps them, or from one drawn here.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "loading_art.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

bool open_context()
{
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_platform_display != nullptr ?
                             get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) :
                             eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

std::string read(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// A cover to show when none is given: a dusk sky over hills, as the launcher's own samples are.
std::vector<unsigned char> sample_cover(int side)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4);
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(side);
            const float v = static_cast<float>(y) / static_cast<float>(side);
            float red = 0.98f - 0.70f * v;
            float green = 0.52f - 0.10f * v;
            float blue = 0.30f + 0.36f * v;
            const float sun = std::exp(-((u - 0.64f) * (u - 0.64f) + (v - 0.40f) * (v - 0.40f)) * 60.0f);
            red += 0.5f * sun;
            green += 0.4f * sun;
            blue += 0.1f * sun;
            const float far_hill = 0.62f + 0.05f * std::sin(u * 9.0f) + 0.03f * std::sin(u * 23.0f + 1.0f);
            const float near_hill = 0.78f + 0.06f * std::sin(u * 6.0f + 2.0f) + 0.02f * std::sin(u * 31.0f);
            if (v > far_hill)
                red = 0.16f, green = 0.30f, blue = 0.34f;
            if (v > near_hill)
                red = 0.05f, green = 0.13f, blue = 0.12f;
            unsigned char *pixel = rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) +
                                                  static_cast<std::size_t>(x)) * 4;
            pixel[0] = static_cast<unsigned char>(std::fmin(red, 1.0f) * 255.0f);
            pixel[1] = static_cast<unsigned char>(std::fmin(green, 1.0f) * 255.0f);
            pixel[2] = static_cast<unsigned char>(std::fmin(blue, 1.0f) * 255.0f);
            pixel[3] = 255;
        }
    }
    return rgba;
}

// One moment of a start: the time since loading began (1000 more: reduced motion), whether the
// game has a cover, and the state the shader is given (hud.h, Loading::State): the step, the
// progress in thousandths, the shaders built and their total, the seconds and the milliseconds the
// step has lasted.
struct Moment
{
    const char *name;
    float seconds;
    bool cover;
    std::array<unsigned, 6> state;
};

constexpr Moment kMoments[] = {
    {"0-arriving", 0.22f, true, {1, 10, 0, 0, 0, 220}},
    {"1-reading-the-game", 0.9f, true, {1, 60, 0, 0, 0, 900}},
    {"2-starting-graphics", 3.4f, true, {2, 300, 0, 0, 2, 2400}},
    {"3-building-shaders", 7.3f, true, {3, 565, 1284, 3910, 4, 4100}},
    {"3-building-shaders-late", 14.6f, true, {3, 872, 3691, 3910, 11, 11400}},
    {"4-starting-the-game", 18.2f, true, {4, 930, 3910, 3910, 3, 3600}},
    {"4-starting-the-game-slow", 47.0f, true, {4, 985, 3910, 3910, 31, 31000}},
    {"5-step-changing", 18.34f, true, {4, 902, 3910, 3910, 0, 140}},
    {"6-reduced-motion", 1007.3f, true, {3, 565, 1284, 3910, 4, 4100}},
    {"7-no-cover", 7.3f, false, {3, 565, 1284, 3910, 4, 4100}},
    {"7-no-cover-slow", 47.0f, false, {4, 985, 3910, 3910, 31, 31000}},
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <headless dir> <output dir> [width height [cover.tga]]\n", argv[0]);
        return 2;
    }
    const std::string source = argv[1];
    const std::string output = argv[2];
    const int width = argc > 4 ? std::atoi(argv[3]) : 1920;
    const int height = argc > 4 ? std::atoi(argv[4]) : 1080;
    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    const std::string scene = read(source + "/loading_text.glsl") + read(source + "/loading_scene.glsl");
    if (scene.size() < 1000)
    {
        std::fprintf(stderr, "cannot read the shader from %s\n", source.c_str());
        return 1;
    }
    const std::string fragment =
        "#version 330 core\nuniform sampler2D loading_art;\n" + scene +
        "uniform vec2 size; uniform float seconds; uniform uint state[24]; out vec4 color;\n"
        "void main(){ color = vec4(loading_scene(gl_FragCoord.xy, size, seconds, state), 1.0); }\n";
    const char *sources[] = {"#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                             "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}",
                             fragment.c_str()};
    const GLuint program = glCreateProgram();
    for (unsigned i = 0; i < 2; ++i)
    {
        const GLuint shader = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(shader, 1, &sources[i], nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096]{};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "error: the loading shader does not compile:\n%s\n", log);
            return 1;
        }
        glAttachShader(program, shader);
    }
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        std::fprintf(stderr, "error: the loading shader does not link\n");
        return 1;
    }

    GLuint framebuffer = 0;
    GLuint color = 0;
    GLuint vertices = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glGenVertexArrays(1, &vertices);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    glViewport(0, 0, width, height);
    glBindVertexArray(vertices);
    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, "size"), static_cast<float>(width), static_cast<float>(height));

    // The picture with a cover, and the one of a game without (a single clear texel).
    Eden::Loading::Art with_cover;
    if (argc > 5)
    {
        int cover_width = 0;
        int cover_height = 0;
        std::vector<std::uint8_t> cover;
        if (!Eden::Loading::ReadCover(argv[5], cover_width, cover_height, cover))
        {
            std::fprintf(stderr, "cannot read the cover %s\n", argv[5]);
            return 1;
        }
        with_cover = Eden::Loading::MakeArt(cover.data(), cover_width, cover_height);
    }
    else
    {
        with_cover = Eden::Loading::MakeArt(sample_cover(256).data(), 256, 256);
    }
    const Eden::Loading::Art pictures[] = {Eden::Loading::Art{}, with_cover};
    GLuint textures[2]{};
    glGenTextures(2, textures);
    for (int i = 0; i < 2; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pictures[i].width, pictures[i].height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pictures[i].rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glUniform1i(glGetUniformLocation(program, "loading_art"), 0);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_flip_vertically_on_write(1);
    bool ok = true;
    for (const Moment &moment : kMoments)
    {
        std::array<GLuint, 24> state{};
        for (std::size_t i = 0; i < moment.state.size(); ++i)
            state[i] = moment.state[i];
        const Eden::Loading::Art &picture = pictures[moment.cover ? 1 : 0];
        state[6] = static_cast<GLuint>(picture.cover_width) | static_cast<GLuint>(picture.cover_height) << 16;
        state[7] = state[1] * 1000;
        glBindTexture(GL_TEXTURE_2D, textures[moment.cover ? 1 : 0]);
        glUniform1f(glGetUniformLocation(program, "seconds"), moment.seconds);
        glUniform1uiv(glGetUniformLocation(program, "state"), 24, state.data());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const std::string path = output + "/loading-" + moment.name + ".png";
        const bool written = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0;
        std::fprintf(stderr, "%s: GL error 0x%x%s\n", moment.name, glGetError(), written ? "" : " (not written)");
        ok = ok && written;
    }
    return ok ? 0 : 1;
}
