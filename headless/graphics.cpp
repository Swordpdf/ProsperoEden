// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance.h"
#include "assets_dir.h"
#include <glad/glad.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#if __has_include(<ps5_opengl_display_modes.h>)
#include <ps5_opengl_display_modes.h>  // the output's refresh rate (display_refresh.h)
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <cstring>
#include <time.h>
#include "hud.h"
#include "loading_art.h"
#include "network/network.h"
#include "loading_scene_glsl.h"
#include "preferences.h"
#include "diagnostics.h"
#include "display_refresh.h"
#include "graphics.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "core/perf_stats.h"
#include "video_core/renderer_base.h"

// Match the SDK native application's shim: Mesa's C TLS is initialized by
// compiler-rt; it has no additional C++ per-thread initializer.
extern "C" void eden_glapi_tls_context_init() __asm__("_ZTH23_mesa_glapi_tls_Context");
extern "C" void eden_glapi_tls_context_init() {}
#ifdef PS5_NATIVE
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif

namespace Eden {
#ifdef EDEN_PS5_VULKAN
void PresentVulkanLoading(VideoCore::RendererBase& renderer);
#endif
namespace {
int RoomPingMilliseconds() {
    const auto member = Network::GetRoomMember().lock();
    return member ? member->GetPingMilliseconds() : -1;
}
// What the loading screen's time carries for reduced motion (loading_scene.glsl): 1000 when the
// player turned it on in Settings > Accessibility, read once.
double LoadingCalm() {
    static const double calm = LoadPreferences().reduce_motion ? 1000.0 : 0.0;
    return calm;
}
std::atomic<bool> hud_enabled{true};
HudClock vulkan_hud_clock;
// What the display is actually given: the guest's frames plus the ones frame generation
// adds (CountPresentedFrame(), hud.h).
HudClock vulkan_hud_output_clock;
HudSnapshot vulkan_hud;
double vulkan_hud_stats_time{}, vulkan_hud_speed{};
bool vulkan_loading{};
double vulkan_loading_start{-1};
unsigned vulkan_loading_frames{};
LoadingPace vulkan_loading_pace;
void Check(bool success, const char* operation) {
    if (!success) {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "%s (EGL 0x%x)", operation, eglGetError());
        Report("Graphics failure", detail);
        std::fflush(stdout);
        throw std::runtime_error(operation);
    }
}
void CheckDirectFormats() {
    // Run before creating the renderer: these draws qualify its two formerly staged formats.
    GLuint program = glCreateProgram(), vao{}, texture{}, framebuffer{}, queries[3]{};
    const GLenum query_targets[]{GL_SAMPLES_PASSED, GL_PRIMITIVES_GENERATED,
                                  GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN};
    glGenVertexArrays(1, &vao);
    glGenTextures(1, &texture);
    glGenFramebuffers(1, &framebuffer);
    glGenQueries(3, queries);
    SCOPE_EXIT {
        glUseProgram(0);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDeleteProgram(program);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &texture);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteQueries(3, queries);
    };
    const char* sources[]{
        "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}",
        "#version 330 core\nout vec4 color;void main(){color=vec4(0.25,0.5,0.75,0.5);}"};
    for (unsigned i = 0; i < 2; ++i) {
        GLuint shader = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(shader, 1, &sources[i], nullptr);
        glCompileShader(shader);
        GLint ok{};
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        glAttachShader(program, shader);
        glDeleteShader(shader);
        Check(ok, "direct-format shader compilation");
    }
    glLinkProgram(program);
    GLint linked{};
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    Check(linked, "direct-format program link");
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, 256, 144);
    const GLenum formats[]{GL_R11F_G11F_B10F, GL_SRGB8_ALPHA8, GL_SRGB8_ALPHA8,
                           GL_RGBA8, GL_RGBA16F};
    const int expected[][4]{{64, 128, 191, 255}, {137, 188, 225, 128}, {64, 128, 191, 128},
                           {64, 128, 191, 128}, {64, 128, 191, 128}};
    const int clear_expected[][4]{{191, 64, 128, 255}, {225, 137, 188, 64}, {191, 64, 128, 64},
                                 {191, 64, 128, 64}, {191, 64, 128, 64}};
    for (unsigned i = 0; i < 5; ++i) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, formats[i], 256, 144, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0);
        Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "direct-format framebuffer");
        if (i == 1) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        unsigned char pixel[4]{};
        glReadPixels(128, 72, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        std::printf("EDEN_GL_DIRECT_FORMAT case=%u rgba=%u,%u,%u,%u\n", i,
                    pixel[0], pixel[1], pixel[2], pixel[3]);
        Check(glGetError() == GL_NO_ERROR, "direct-format GL error");
        for (unsigned channel = 0; channel < 4; ++channel)
            Check(std::abs(int(pixel[channel]) - expected[i][channel]) <= 1,
                  "direct-format pixel mismatch");
        for (unsigned q = 0; q < 3; ++q) glBeginQuery(query_targets[q], queries[q]);
        glClearColor(0.75f, 0.25f, 0.5f, 0.25f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(192, 72, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        std::printf("EDEN_GL_CLEAR_FORMAT case=%u rgba=%u,%u,%u,%u\n", i,
                    pixel[0], pixel[1], pixel[2], pixel[3]);
        for (unsigned channel = 0; channel < 4; ++channel)
            Check(std::abs(int(pixel[channel]) - clear_expected[i][channel]) <= 1,
                  "direct-format full-clear mismatch");
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 128, 144);
        glClearColor(0.25f, 0.5f, 0.75f, 0.5f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        for (unsigned side = 0; side < 2; ++side) {
            glReadPixels(side ? 192 : 64, 72, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            for (unsigned channel = 0; channel < 4; ++channel)
                Check(std::abs(int(pixel[channel]) -
                               (side ? clear_expected[i][channel] : expected[i][channel])) <= 1,
                      "direct-format scissor or preserved-pixel mismatch");
        }
        Check(glGetError() == GL_NO_ERROR, "direct-format clear GL error");
        std::printf("EDEN_GL_DIRECT_CLEAR_PASS case=%u full=1 scissor=1 preserved=1\n", i);
        for (GLenum target : query_targets) glEndQuery(target);
        GLuint counts[3]{};
        for (unsigned q = 0; q < 3; ++q) {
            glGetQueryObjectuiv(queries[q], GL_QUERY_RESULT, &counts[q]);
            Check(counts[q] == 0, "clear-only query must be zero");
        }
        // Both guest-style draws must count, but the intervening clear must not.
        for (unsigned q = 0; q < 3; ++q) glBeginQuery(query_targets[q], queries[q]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (GLenum target : query_targets) glEndQuery(target);
        const GLuint expected_counts[]{2u * 256u * 144u, 2, 0};
        for (unsigned q = 0; q < 3; ++q) {
            glGetQueryObjectuiv(queries[q], GL_QUERY_RESULT, &counts[q]);
            Check(counts[q] == expected_counts[q], "draw-clear-draw query count mismatch");
        }
        glReadPixels(128, 72, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        for (unsigned channel = 0; channel < 4; ++channel)
            Check(std::abs(int(pixel[channel]) - expected[i][channel]) <= 1,
                  "draw state not restored after query-isolated clear");
        Check(glGetError() == GL_NO_ERROR, "query-isolated clear GL error");
        std::printf("EDEN_GL_CLEAR_QUERY_PASS case=%u clear_counts=0,0,0 draw_counts=%u,%u,%u\n",
                    i, counts[0], counts[1], counts[2]);
    }
    std::puts("EDEN_GL_DIRECT_FORMAT_PASS cases=5");
}
void Cleanup(bool success, const char* operation) noexcept {
    if (!success) {
        std::fprintf(stderr, "EGL cleanup failed: %s (0x%x)\n", operation, eglGetError());
        std::_Exit(1);
    }
}
class Context;
thread_local Context* current_window_context{};
class Context final : public Core::Frontend::GraphicsContext {
public:
    Context(EGLDisplay d, EGLConfig config, EGLSurface s, EGLContext share,
            std::atomic<bool>* owner, Core::System* system_)
        : display(d), surface(s), window_owner(owner), system(system_) {
        constexpr EGLint attributes[]{EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
            EGL_CONTEXT_MINOR_VERSION_KHR, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE};
        Check(eglBindAPI(EGL_OPENGL_API), "eglBindAPI");
        context = eglCreateContext(display, config, share, attributes);
        Check(context != EGL_NO_CONTEXT, "eglCreateContext 4.6");
        if (window_owner) {
            hud_context = eglCreateContext(display, config, share, attributes);
            if (hud_context == EGL_NO_CONTEXT) {
                Cleanup(eglDestroyContext(display, context), "failed HUD context setup");
                context = EGL_NO_CONTEXT;
                Check(false, "HUD context creation");
            }
        }
    }
    ~Context() override {
        if (hud_context != EGL_NO_CONTEXT) {
            Cleanup(eglMakeCurrent(display, surface, surface, hud_context), "HUD cleanup current");
            glDeleteProgram(hud_program);
            glDeleteVertexArrays(1, &hud_vao);
            glDeleteProgram(loading_program);
            glDeleteVertexArrays(1, &loading_vao);
            glDeleteTextures(1, &loading_texture);
            DoneCurrent();
            Cleanup(eglDestroyContext(display, hud_context), "HUD context");
        }
        if (eglGetCurrentContext() == context) DoneCurrent();
        Cleanup(eglDestroyContext(display, context), "eglDestroyContext");
        if (window_owner) *window_owner = false;
    }
    void MakeCurrent() override {
        Check(eglMakeCurrent(display, surface, surface, context), "eglMakeCurrent");
        if (window_owner && !swap_interval_set) {
            Check(eglSwapInterval(display, 0), "disable presentation throttling");
            swap_interval_set = true;
            std::puts("EDEN_EGL_SWAP_INTERVAL value=0");
        }
        current_window_context = window_owner ? this : nullptr;

    }
    void DoneCurrent() override {
        current_window_context = nullptr;
        Cleanup(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                "release current context");
    }
    void SwapBuffers() override {
        const auto now = Now();
        if (loading && !startup_gate.Ready(now)
        ) {
            ++startup_guest_frames;
            PresentLoading();
            return;
        }
        if (loading) {
            loading = false;
            std::printf("EDEN_LOADING_DONE frames=%u seconds=%.2f scene=%d\n", loading_frames,
                        loading_start < 0 ? 0.0 : now - loading_start, !loading_failed);
            if (system) (void)system->GetAndResetPerfStats();
            last_stats = now;
            std::printf("EDEN_GAME_VISIBLE guest_frames=%u stabilization_ms=100 smooth_frames=8\n",
                        startup_guest_frames + 1);
        }
        ++presented_frames;
#ifdef EDEN_DEV_PROFILE
        static const double capture_start = now;
        static bool captured_passes = false;
        if (!captured_passes && now - capture_start >= 150.0) {
            captured_passes = true;
            Eden::Performance::capture_passes = 8;
            std::printf("EDEN_DEV_PASS_REQUEST time=%.3f\n", now - capture_start);
        }
        static double sample_start = now, prior_frame = now, worst_frame = 0;
        static unsigned sample_frames = 0;
        worst_frame = std::max(worst_frame, now - prior_frame);
        prior_frame = now;
        ++sample_frames;
        if (now - sample_start >= 5.0) {
            Eden::Performance::SampleGpuFrame(presented_frames);
            std::printf("EDEN_DEV_DRAW calls=%llu ns=%llu\n",
                        Eden::Performance::rasterizer_draw.calls.load(),
                        Eden::Performance::rasterizer_draw.nanoseconds.load());
            std::printf("EDEN_DEV_QUEUE idle_ns=%llu dispatch_ns=%llu\n",
                        Eden::Performance::gpu_queue_wait.nanoseconds.load(),
                        Eden::Performance::gpu_dispatch.nanoseconds.load());
            std::printf("EDEN_DEV_FRAME frames=%u seconds=%.6f fps=%.3f worst_ms=%.3f total=%u\n",
                        sample_frames, now - sample_start, sample_frames / (now - sample_start),
                        worst_frame * 1000.0, presented_frames);
            sample_start = now;
            sample_frames = 0;
            worst_frame = 0;
        }
#endif
#ifndef EDEN_DEV_PROFILE
        if (sample_start < 0) {
            sample_start = prior_sample_frame = now;
        } else {
            ++sample_frames;
            sample_worst = std::max(sample_worst, now - prior_sample_frame);
            prior_sample_frame = now;
            if (now - sample_start >= 5.0) {
                std::printf("EDEN_GAME_FRAME frames=%u seconds=%.6f fps=%.3f worst_ms=%.3f total=%u\n",
                            sample_frames, now - sample_start,
                            sample_frames / (now - sample_start), sample_worst * 1000.0,
                            presented_frames);
                sample_start = now;
                sample_frames = 0;
                sample_worst = 0;
            }
        }
#endif
        clock.Present(now);
        if (system && now - last_stats >= 1.0) {
            const auto stats = system->GetAndResetPerfStats();
            speed_percent = stats.emulation_speed * 100.0;
            last_stats = now;
        }
        const auto text = FormatHudText(clock, speed_percent, "OGL", RoomPingMilliseconds());
        if (hud_enabled.load(std::memory_order_relaxed)) DrawHud(text.data(), false);
        Check(eglSwapBuffers(display, surface), "eglSwapBuffers");
    }
    void PresentLoading() {
        const double now = Now();
        if (loading_start < 0) loading_start = now;
        if (!DrawLoading(now - loading_start + LoadingCalm())) {
            // The scene's shader did not build on this driver: plain text instead.
            const unsigned dots = static_cast<unsigned>(now * 4) % 4;
            char text[16] = "LOADING";
            for (unsigned i = 0; i < dots; ++i) text[7 + i] = '.';
            DrawHud(text, true);
        }
        Check(eglSwapBuffers(display, surface), "loading swap");
        if (loading_frames++ < 4) std::printf("EDEN_LOADING frame=%u scene=%d\n", loading_frames, !loading_failed);
    }
    // idle: the GPU thread has no game commands waiting (hud.h, LoadingPace).
    bool LoadingTick(bool idle) {
        if (!loading) return false;
        if (loading_pace.Due(Now(), idle)) PresentLoading();
        return true;
    }
private:
    static double Now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // The loading screen (loading_scene.glsl): false when its shader cannot be used.
    bool DrawLoading(double seconds) {
        if (!window_owner || loading_failed) return false;
        Check(eglMakeCurrent(display, surface, surface, hud_context), "loading current");
        SCOPE_EXIT { Cleanup(eglMakeCurrent(display, surface, surface, context), "loading restore"); };
        if (!loading_program) {
            const std::string fragment = std::string("#version 330 core\nuniform sampler2D loading_art;\n") +
                kLoadingSceneGlsl +
                "uniform vec2 size; uniform float seconds; uniform uint state[24]; out vec4 color;\n"
                "void main(){ color = vec4(loading_scene(gl_FragCoord.xy, size, seconds, state), 1.0); }\n";
            const char* sources[]{
                "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}",
                fragment.c_str()};
            const GLuint program = glCreateProgram();
            bool built = true;
            for (unsigned i = 0; i < 2; ++i) {
                GLuint shader = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
                glShaderSource(shader, 1, &sources[i], nullptr);
                glCompileShader(shader);
                GLint ok{};
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (!ok) {
                    char log[512]{};
                    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                    Report("loading screen", log);
                }
                glAttachShader(program, shader);
                glDeleteShader(shader);
                built = built && ok;
            }
            GLint linked{};
            if (built) {
                glLinkProgram(program);
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
            }
            if (!linked) {
                Report("loading screen", "The loading scene's shader did not build; showing text instead");
                glDeleteProgram(program);
                loading_failed = true;
                return false;
            }
            loading_program = program;
            glGenVertexArrays(1, &loading_vao);
            // Its picture (loading_art.h): the game's cover and its blurred copy, or one clear
            // texel for a game without a cover.
            const Loading::Art art = Loading::GetArt();
            glGenTextures(1, &loading_texture);
            glBindTexture(GL_TEXTURE_2D, loading_texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, art.width, art.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         art.rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        EGLint width{}, height{};
        Check(eglQuerySurface(display, surface, EGL_WIDTH, &width) &&
              eglQuerySurface(display, surface, EGL_HEIGHT, &height), "loading dimensions");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_BLEND);
        glUseProgram(loading_program);
        glBindVertexArray(loading_vao);
        glViewport(0, 0, width, height);
        glUniform2f(glGetUniformLocation(loading_program, "size"), float(width), float(height));
        glUniform1f(glGetUniformLocation(loading_program, "seconds"), float(seconds));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, loading_texture);
        glUniform1i(glGetUniformLocation(loading_program, "loading_art"), 0);
        // What the start is doing (hud.h).
        const auto state = Loading::State();
        glUniform1uiv(glGetUniformLocation(loading_program, "state[0]"), state.size(), state.data());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
        return true;
    }
    void DrawHud(const char* text, bool startup) {
        if (!window_owner) return;
        // A separate context on this same thread isolates guest GL state and queries.
        Check(eglMakeCurrent(display, surface, surface, hud_context), "HUD current");
        SCOPE_EXIT { Cleanup(eglMakeCurrent(display, surface, surface, context), "HUD restore"); };
        if (!hud_program) {
            hud_program = glCreateProgram();
            const char* sources[]{
                "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}",
                "#version 330 core\nuniform uint glyphs[28]; uniform ivec2 origin; uniform int startup;"
                "uniform float cell; out vec4 color;"
                "void main(){ivec2 p=ivec2(gl_FragCoord.xy)-origin; bool ink=false;"
                "if(p.x>=0&&p.y>=0){ivec2 c=ivec2(vec2(p)/cell);int i=c.x/4;int x=c.x%4;"
                "if(i<28&&x<3&&c.y<5) ink=((glyphs[i]>>uint(c.y*3+2-x))&1u)!=0u;}"
                "color=startup!=0?(ink?vec4(0.7216,0.9490,0.0471,1.0):vec4(0.0196,0.0392,0.0039,1.0))"
                ":(ink?vec4(0.9,0.95,1.0,1.0):vec4(0.025,0.04,0.075,0.5));}"};
            for (unsigned i = 0; i < 2; ++i) {
                GLuint shader = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
                glShaderSource(shader, 1, &sources[i], nullptr);
                glCompileShader(shader);
                GLint ok{};
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                glAttachShader(hud_program, shader);
                glDeleteShader(shader);
                Check(ok, "HUD shader compile");
            }
            glLinkProgram(hud_program);
            GLint ok{};
            glGetProgramiv(hud_program, GL_LINK_STATUS, &ok);
            Check(ok, "HUD program link");
            glGenVertexArrays(1, &hud_vao);
        }
        EGLint width{}, height{};
        Check(eglQuerySurface(display, surface, EGL_WIDTH, &width) &&
              eglQuerySurface(display, surface, EGL_HEIGHT, &height), "HUD dimensions");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(hud_program);
        glBindVertexArray(hud_vao);
        // Laid out for a picture 1080 rows high (a glyph's cell is 4 pixels there) and scaled to
        // the surface's.
        const float scale = static_cast<float>(height) / 1080.0f;
        const auto units = [scale](int value) { return static_cast<int>(std::lround(value * scale)); };
        const int text_width = int(std::strlen(text)) * 16;
        glViewport(startup ? 0 : units(16), startup ? 0 : height - units(64),
                   startup ? width : units(text_width + 24), startup ? height : units(48));
        const auto glyphs = HudText(text);
        glUniform1uiv(glGetUniformLocation(hud_program, "glyphs[0]"), glyphs.size(), glyphs.data());
        glUniform2i(glGetUniformLocation(hud_program, "origin"),
                    startup ? (width - units(text_width)) / 2 : units(28),
                    startup ? (height - units(20)) / 2 : height - units(50));
        glUniform1i(glGetUniformLocation(hud_program, "startup"), startup);
        glUniform1f(glGetUniformLocation(hud_program, "cell"), 4.0f * scale);
        if (!startup) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
    }
    unsigned presented_frames{}, loading_frames{};
    bool loading{true}, loading_failed{};
    double loading_start{-1};
    LoadingPace loading_pace;
    GLuint loading_program{}, loading_vao{}, loading_texture{};
    unsigned startup_guest_frames{};
    StartupGate startup_gate;
    HudClock clock;
#ifndef EDEN_DEV_PROFILE
    double sample_start{-1}, prior_sample_frame{}, sample_worst{};
    unsigned sample_frames{};
#endif
    EGLContext hud_context{EGL_NO_CONTEXT};
    GLuint hud_program{}, hud_vao{};
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context{EGL_NO_CONTEXT};
    std::atomic<bool>* window_owner;
    bool swap_interval_set{};
    Core::System* system{};
    double last_stats{}, speed_percent{100.0};
};
}
void ToggleHud() {
    const bool enabled = !hud_enabled.load(std::memory_order_relaxed);
    hud_enabled.store(enabled, std::memory_order_relaxed);
    // A game with its own FPS overlay setting keeps the change for itself.
    const uint64_t title = session_title.load();
    if (auto game = LoadGameSettings(title); title && game.hud >= 0) {
        game.hud = enabled ? 1 : 0;
        if (!SaveGameSettings(title, game)) Report("settings", "Could not save the game's HUD setting");
        return;
    }
    auto preferences = LoadPreferences();
    preferences.hud = enabled;
    if (!SavePreferences(preferences)) Report("settings", "Could not save HUD preference");
}
HudSnapshot GetVulkanHud() {
    return vulkan_loading || hud_enabled.load(std::memory_order_relaxed) ? vulkan_hud : HudSnapshot{};
}
// idle: the GPU thread has no game commands waiting (hud.h, LoadingPace).
bool LoadingTick(VideoCore::RendererBase& renderer, bool idle) {
#ifdef EDEN_PS5_VULKAN
    if (vulkan_loading) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool first = vulkan_loading_start < 0;
        if (first) vulkan_loading_start = now;
        if (vulkan_loading_pace.Due(now, idle)) {
            vulkan_hud = MakeLoadingSnapshot(now - vulkan_loading_start + LoadingCalm());
            PresentVulkanLoading(renderer);
            ++vulkan_loading_frames;
            if (first) sceSystemServiceHideSplashScreen();
        }
        return true;
    }
#endif
    return current_window_context && current_window_context->LoadingTick(idle);
}
GraphicsWindow::GraphicsWindow(bool use_vulkan) : vulkan(use_vulkan) {
    vulkan_loading = vulkan;
    vulkan_loading_start = -1;
    vulkan_loading_frames = 0;
    vulkan_loading_pace = {};
    hud_enabled.store(PreferencesFor(session_title.load()).hud, std::memory_order_relaxed);
#ifdef EDEN_PS5_VULKAN
    if (vulkan) {
        vulkan_hud_clock = {};
        vulkan_hud_output_clock = {};
        vulkan_hud = MakeLoadingSnapshot(0);
        vulkan_hud_stats_time = vulkan_hud_speed = 0;
        window_info.type = Core::Frontend::WindowSystemType::PS5;
        window_info.render_surface = this;
        // The frame the filter scales the game to (Settings > Video > Output resolution); the
        // renderer copies it to the driver's output.
        UpdateCurrentFramebufferLayout(static_cast<u32>(Display::output_width.load()),
                                       static_cast<u32>(Display::output_height.load()));
        std::printf("EDEN_VULKAN_FRAME_SIZE %dx%d\n", Display::output_width.load(), Display::output_height.load());
        return;
    }
#else
    if (vulkan) throw std::runtime_error("Vulkan adapter not built");
#endif
    // PS5 shader compilation uses the renderer context; parallel shared contexts are not stable.
    strict_context_required = true;
    try {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#ifdef PS5_OPENGL_DYNAMIC_DISPLAY
        // Settings > Video: the output's size and refresh rate. They are settings of the process, so
        // every session sets them; a display that cannot show 120 Hz presents at 60 Hz, and a size
        // that is refused leaves the one the launcher opened with.
        if (display != EGL_NO_DISPLAY &&
            !eglSetDisplayModePS5(display, Display::output_width.load(), Display::output_height.load()))
            std::fprintf(stderr, "[ProsperoEden] OpenGL display: the output size was not set (0x%x)\n",
                         static_cast<unsigned>(eglGetError()));
        if (display != EGL_NO_DISPLAY && !eglSetDisplayRefreshPS5(display, Display::requested_hz.load()))
            std::fprintf(stderr, "[ProsperoEden] OpenGL display: the refresh rate was not set (0x%x)\n",
                         static_cast<unsigned>(eglGetError()));
#endif
        Check(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "eglInitialize");
        constexpr EGLint attributes[]{EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count{};
        Check(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1,
              "eglChooseConfig");
        surface = eglCreateWindowSurface(display, config, 0, nullptr);
        Check(surface != EGL_NO_SURFACE, "eglCreateWindowSurface");
        EGLint width{}, height{};
        Check(eglQuerySurface(display, surface, EGL_WIDTH, &width) &&
              eglQuerySurface(display, surface, EGL_HEIGHT, &height) && width > 0 && height > 0,
              "eglQuerySurface dimensions");
        UpdateCurrentFramebufferLayout(width, height);
        constexpr EGLint ctx_attributes[]{EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
            EGL_CONTEXT_MINOR_VERSION_KHR, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE};
        Check(eglBindAPI(EGL_OPENGL_API), "eglBindAPI");
        root = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attributes);
        Check(root != EGL_NO_CONTEXT, "root Compatibility context");
        Check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, root), "root current");
        Check(gladLoadGLLoader([](const char* name) -> void* {
            return reinterpret_cast<void*>(eglGetProcAddress(name));
        }) && GLAD_GL_VERSION_4_6, "GLAD OpenGL 4.6 loading");
        GLint profile{};
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
        Check(profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT, "OpenGL Compatibility profile");
        CheckDirectFormats();
        std::printf("EDEN_GL_VERSION %s\nEDEN_GL_RENDERER %s\nEDEN_GL_PROFILE %d\n",
                    glGetString(GL_VERSION), glGetString(GL_RENDERER), profile);
        Check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "root release");
        window_info.render_surface = surface;
        std::printf("EDEN_EGL_SURFACE %dx%d\n", width, height);
    } catch (...) {
        if (root != EGL_NO_CONTEXT) {
            Cleanup(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "root release");
            Cleanup(eglDestroyContext(display, root), "root");
        }
        if (surface != EGL_NO_SURFACE) Cleanup(eglDestroySurface(display, surface), "surface");
        if (display != EGL_NO_DISPLAY) Cleanup(eglTerminate(display), "display");
        throw;
    }
}
GraphicsWindow::~GraphicsWindow() {
    if (vulkan) return;
    Cleanup(!window_context.load(), "renderer context still alive");
#ifdef PS5_OPENGL_DYNAMIC_DISPLAY
    // What the display took (known once a frame was presented).
    if (EGLint hz = 0; eglGetDisplayModePS5(display, nullptr, nullptr, &hz)) {
        Display::output_millihertz.store(hz * 1000);
        std::printf("EDEN_EGL_REFRESH asked=%d hz=%d\n", Display::requested_hz.load(), hz);
    }
#endif
    Cleanup(eglDestroyContext(display, root), "root");
    Cleanup(eglDestroySurface(display, surface), "surface");
    Cleanup(eglTerminate(display), "display");
    std::puts("EDEN_EGL_CLOSED");
}
void CountPresentedFrame() {
    // The renderer calls this for every frame it hands to the present manager, so F in the HUD is
    // the output rate. Same clock and timestamp as OnFrameDisplayed(); only the count differs.
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    vulkan_hud_output_clock.Present(now);
}

void GraphicsWindow::OnFrameDisplayed() {
#ifdef PS5_NATIVE
    if (vulkan) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (vulkan_loading && vulkan_loading_start >= 0)
            std::printf("EDEN_LOADING_DONE frames=%u seconds=%.2f\n", vulkan_loading_frames,
                        now - vulkan_loading_start);
        vulkan_loading = false;
#ifdef EDEN_PS5_VULKAN
        vulkan_hud_clock.Present(now);
        if (system && now - vulkan_hud_stats_time >= 1.0) {
            vulkan_hud_speed = system->GetAndResetPerfStats().emulation_speed * 100.0;
            vulkan_hud_stats_time = now;
        }
        vulkan_hud =
            MakeHudSnapshot(vulkan_hud_clock, vulkan_hud_output_clock, vulkan_hud_speed,
                            Eden::Display::output_millihertz.load(std::memory_order_relaxed) / 1000.0,
                            RoomPingMilliseconds());
#endif
        ++frame_total;
        // What a recorded controller session counts frames by (gameplay_test.h): the input
        // worker runs on another thread, so it reads a copy rather than the member.
        Display::gameplay_frame.store(frame_total, std::memory_order_relaxed);
        if (frame_sample_start < 0) {
            frame_sample_start = frame_sample_last = now;
        } else {
            ++frame_sample_count;
            frame_sample_worst = std::max(frame_sample_worst, now - frame_sample_last);
#ifdef EDEN_DEV_PROFILE
            // Present intervals by vsync multiple: a 60 FPS title that misses by a little
            // shows up in the 2-vsync bucket, a slow one across all of them. "half" counts the
            // frames that came within one refresh of a 120 Hz output.
            static std::array<unsigned, 5> interval_hist{};
            const double interval_ms = (now - frame_sample_last) * 1000.0;
            ++interval_hist[interval_ms < 12.5 ? 4 : interval_ms < 20.0 ? 0 : interval_ms < 36.0 ? 1 :
                            interval_ms < 52.0 ? 2 : 3];
#endif
            frame_sample_last = now;
            if (now - frame_sample_start >= 5.0) {
                // The game's frames; the ones a slower display did not show are counted apart.
                std::printf("EDEN_VULKAN_FRAME frames=%u seconds=%.6f fps=%.3f worst_ms=%.3f total=%u "
                            "not_shown=%u clock_hz=%.1f\n",
                    frame_sample_count, now - frame_sample_start,
                    frame_sample_count / (now - frame_sample_start),
                    frame_sample_worst * 1000.0, frame_total, Display::skipped_frames.load(),
                    Display::game_millihertz.load() / 1000.0);
#ifdef EDEN_DEV_PROFILE
                std::printf("EDEN_VULKAN_INTERVALS v1=%u v2=%u v3=%u v4plus=%u half=%u\n", interval_hist[0],
                            interval_hist[1], interval_hist[2], interval_hist[3], interval_hist[4]);
                interval_hist = {};
#endif
                Eden::Performance::ReportVulkan();
#ifdef EDEN_DEV_PROFILE
                // Composite runs on the GPU thread, so its owner CPU clock is valid here.
                Eden::Performance::ReportGpuThread(frame_total);
#endif
                frame_sample_start = now;
                frame_sample_count = 0;
                frame_sample_worst = 0;
            }
        }
    }
    if (!first_frame_reported) {
        first_frame_reported = true;
        const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::printf("EDEN_STAGE backend=%s phase=first_frame_callback mono_ns=%lld\n",
                    vulkan ? "Vulkan" : "OpenGL", static_cast<long long>(mono_ns));
        std::fflush(stdout);
    }
    // Development autoboot bypasses the launcher, which normally hides splash.
    // OpenGL already does this inside EGL; Vulkan uses the shared frame callback.
    if (vulkan && !splash_hidden) {
        const int result = sceSystemServiceHideSplashScreen();
        std::printf("EDEN_VULKAN_HIDE_SPLASH rc=%08x\n", static_cast<unsigned>(result));
        std::fflush(stdout);
        splash_hidden = result == 0;
    }
#endif
}
void GraphicsWindow::CheckPresentation(Core::Frontend::GraphicsContext& context) {
    if (vulkan) return;
    auto current = context.Acquire();
    // Preserve the renderer's bindings and clear state around this startup oracle.
    GLint framebuffer{}, read_framebuffer{};
    GLfloat clear[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    SCOPE_EXIT {
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    };
    const GLenum initial_error = glGetError();
    if (initial_error != GL_NO_ERROR) std::fprintf(stderr, "Renderer initialization GL error: 0x%x\n", initial_error);
    Check(initial_error == GL_NO_ERROR, "renderer initialization GL state");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (unsigned frame = 0; frame < 2; ++frame) {
        glClearColor(frame ? 1.0f : 0.0f, 0.0f, frame ? 0.0f : 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        unsigned char pixel[4]{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        std::printf("EDEN_GL_PIXEL frame=%u rgba=%u,%u,%u,%u\n", frame,
                    pixel[0], pixel[1], pixel[2], pixel[3]);
        Check(pixel[0] == (frame ? 255 : 0) && pixel[1] == 0 &&
              pixel[2] == (frame ? 0 : 255) && pixel[3] == 255,
              "EGL presentation pixel mismatch");
        Check(glGetError() == GL_NO_ERROR, "presentation GL error");
        static_cast<Context&>(context).PresentLoading();
    }
    std::puts("EDEN_GL_PRESENTATION_PASS frames=2");
}
std::unique_ptr<Core::Frontend::GraphicsContext> GraphicsWindow::CreateSharedContext() const {
    if (vulkan) return std::make_unique<Core::Frontend::GraphicsContext>();
    const bool owns_window = !window_context.exchange(true);
    try {
        return std::make_unique<Context>(display, config, owns_window ? surface : EGL_NO_SURFACE,
                                         root, owns_window ? &window_context : nullptr, system);
    } catch (...) {
        if (owns_window) window_context = false;
        throw;
    }
}
std::shared_future<bool> GraphicsWindow::CaptureNextFrame(VideoCore::RendererBase& renderer,
                                                       std::function<void()> on_complete) {
    const auto layout = GetFramebufferLayout();
    auto pixels = std::make_shared<std::vector<unsigned char>>(layout.width * layout.height * 4);
    auto done = std::make_shared<std::promise<bool>>();
    auto result = done->get_future().share();
    renderer.RequestScreenshot(pixels->data(), [pixels, done, layout, on_complete](bool flip) {
        // Renderer owns GPU readback; its completion callback only writes the captured bytes.
        const auto path_text = Eden::LogFile("game-frame.ppm");
        const char* path = path_text.c_str();
        const auto partial_text = Eden::LogFile("game-frame.ppm.partial");
        const char* partial = partial_text.c_str();
        std::FILE* output = std::fopen(partial, "wb");
        bool ok = output != nullptr;
        if (output) {
            ok = std::fprintf(output, "P6\n%u %u\n255\n", layout.width, layout.height) > 0;
            std::vector<unsigned char> row(layout.width * 3);
            for (unsigned y = 0; ok && y < layout.height; ++y) {
                const unsigned source_y = flip ? layout.height - 1 - y : y;
                for (unsigned x = 0; x < layout.width; ++x) {
                    const auto at = (source_y * layout.width + x) * 4;
                    row[x * 3] = (*pixels)[at + 2];
                    row[x * 3 + 1] = (*pixels)[at + 1];
                    row[x * 3 + 2] = (*pixels)[at];
                }
                ok = std::fwrite(row.data(), 1, row.size(), output) == row.size();
            }
            ok = std::fclose(output) == 0 && ok;
        }
        if (ok) ok = std::rename(partial, path) == 0;
        if (!ok) std::remove(partial);
        std::printf("EDEN_GAME_CAPTURE completed=%u width=%u height=%u\n", ok, layout.width, layout.height);
        done->set_value(ok);
        on_complete();
    }, layout);
    return result;
}
}

#ifdef EDEN_GPU_PROBE
#include "gpu_probe.inc"
#endif
