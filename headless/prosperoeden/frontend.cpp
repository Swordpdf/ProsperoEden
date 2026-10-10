// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher on the console: its display, sound and controller, and the frame loop around
// pe::ui::Launcher. Everything here is created when the launcher appears and gone before a game
// starts: the game's own window takes over the screen.
#include "frontend.h"

#include "assets_dir.h"
#include "audio_out_init.h"
#include "boot_trace.h"
#include "diagnostics.h"
#include "eden_services.h"
#include "update_notice.h"
#include "pe/audio/sounds.hpp"
#include "pe/core/file.hpp"
#include "pe/core/strings.hpp"
#include "pe/gfx/gl_batch.hpp"
#include "pe/gfx/system_fonts.hpp"
#include "pe/platform/audio_out.hpp"
#include "pe/platform/display_egl.hpp"
#include "pe/ui/launcher.hpp"
#include "radio_input.h"
#include "ps5_system_language.hpp"
#ifdef EDEN_DEV_ROM_ID
#include "crash_trigger.h"
#include "development_input.h"
#include "stop_limit.h"
#include "system_keyboard.h"
#include <atomic>
#include <thread>
#include <cerrno>
#include <fstream>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceSystemServiceHideSplashScreen(void);

namespace {

using Clock = std::chrono::steady_clock;

// The launcher draws at the size of Settings > Video > Output resolution (settings_store.h,
// kOutputKeys); the console scales it to the TV. *output names the size that opened: 1080p when
// the chosen one did not.
bool OpenDisplay(pe::ps5::Display& display, int* output) {
    if (display.open(Eden::kOutputWidth[*output], Eden::kOutputHeight[*output])) return true;
    if (*output == 0) return false;
    *output = 0;
    return display.open(Eden::kOutputWidth[0], Eden::kOutputHeight[0]);
}

// The controller's keys arrive in the launcher's order.
static_assert(static_cast<int>(pe::ui::Key::cross) == RADIO_INPUT_CROSS &&
              static_cast<int>(pe::ui::Key::options) == RADIO_INPUT_OPTIONS &&
              static_cast<int>(pe::ui::Key::right) == RADIO_INPUT_RIGHT);

long long Milliseconds(Clock::duration value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

// Menu sound level 0-100 as a gain: quiet steps stay audible, the top is full level.
float MenuGain(int volume) {
    const float level = static_cast<float>(std::clamp(volume, 0, 100)) / 100.0f;
    return level * level;
}

// The launcher speaks the console's language when the app has a catalog for it
// (ui/lang/<tag>.po, tags as in third_party/ps5_system_language.hpp), and English otherwise.
// language.txt in the app's folder, holding a tag, chooses another one ("en-US": English).
//
// Montserrat has Latin and Cyrillic letters. The other scripts (Japanese, Korean, Chinese, Greek,
// Thai, Arabic: in the launcher's own text, a game's title or a file name) are drawn with the
// console's own fonts, which the font is told of here every time the launcher opens. A catalog
// those fonts cannot draw in full is not used: English is better than missing letters.
void LoadLanguage(pe::gfx::Font& font) {
    static bool loaded = false;
    static std::string tag;
    const bool first = !std::exchange(loaded, true);
    int system_language = -1;
    int rc = 0;
    if (first) {
        rc = sceSystemServiceParamGetInt(ps5::i18n::kSystemLanguageParameter, &system_language);
        tag = rc == 0 ? ps5::i18n::language_tag(system_language) : ps5::i18n::kFallbackLanguage;
        std::string chosen;
        if (pe::read_file(Eden::AppFile("language.txt"), &chosen, 64)) {
            while (!chosen.empty() && static_cast<unsigned char>(chosen.back()) <= ' ') chosen.pop_back();
            if (!chosen.empty()) tag = chosen;
        }
    }
    std::string folder = "none";
    std::size_t files = 0;
    for (const std::string& candidate : pe::gfx::system_font_folders()) {
        std::vector<std::string> found = pe::gfx::system_font_files(candidate, tag);
        if (found.empty()) continue;
        folder = candidate;
        files = found.size();
        font.use_system_fonts(std::move(found), tag);
        break;
    }
    if (!first) return;
    std::string catalog = "none";
    std::size_t texts = 0;
    for (const std::string& candidate : pe::catalog_candidates(tag)) {
        std::string po;
        if (!pe::read_file(Eden::AppFile("ui/lang/" + candidate + ".po"), &po, 1u << 20)) continue;
        texts = pe::catalog().load(po);
        catalog = candidate;
        break;
    }
    if (texts != 0 && !pe::catalog().every([&font](std::string_view text) { return font.can_draw(text); })) {
        pe::catalog().clear();
        catalog += " (not used: no font for it)";
        texts = 0;
    }
    std::fprintf(stderr, "EDEN_LANGUAGE system=%d rc=0x%x tag=%s catalog=%s texts=%zu\n", system_language,
                 static_cast<unsigned>(rc), tag.c_str(), catalog.c_str(), texts);
    std::fprintf(stderr, "EDEN_FONTS folder=%s files=%zu read=%s\n", folder.c_str(), files,
                 font.system_fonts_read().c_str());
    // For a report from another console: what is shown depends on both.
    Eden::BootTrace::Line("system language %s (%d, rc %#x), menu texts %s; console fonts: %zu in %s", tag.c_str(),
                          system_language, static_cast<unsigned>(rc), catalog.c_str(), files, folder.c_str());
}

#ifdef EDEN_DEV_ROM_ID
// The frame as drawn, bottom row first: a 24-bit BMP for the development runner.
int SaveCapture(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54]{'B', 'M'};
    const auto put = [&](int at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return -1;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y) {
        const unsigned char* in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok ? 0 : -1;
}
#endif

std::string RunApp(const std::string& launch_error, bool first_start, Eden::Multiplayer::RoomClient& rooms) {
    const auto opened = Clock::now();
    // What Settings > Video asks for, and what opened.
    int output = std::clamp(Eden::LoadPreferences().output, 0, static_cast<int>(std::size(Eden::kOutputKeys)) - 1);
    int output_open = output;
    pe::ps5::Display display;
    if (!OpenDisplay(display, &output_open)) {
        Eden::Report("menu video failure", pe::ps5::egl_error_name(display.last_error()));
        return {};
    }
    // Something on screen at once: the launcher's own dark, then the first real frame.
    glClearColor(0.024f, 0.035f, 0.039f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    display.swap();
    sceSystemServiceHideSplashScreen();

    std::string selected_game;
    {
        pe::gfx::GlBatch batch;
        pe::gfx::Font font;
        std::string font_data;
        const std::string font_path = Eden::AppFile("ui/fonts/montserrat-medium.pefont");
        const bool shaders = batch.init();
        const bool font_read = shaders && pe::read_file(font_path, &font_data);
        const bool ready = font_read && font.load(font_data);
        if (!ready) {
            const std::string why = !shaders ? std::string{"The launcher's shaders could not be built"} :
                !font_read ? "The launcher's font could not be read: " + font_path + " (" + std::strerror(errno) + ")" :
                "The launcher's font is damaged: " + font_path + " (" + std::to_string(font_data.size()) + " bytes)";
            Eden::Report("menu failure", why.c_str());
            glClearColor(0.7f, 0.08f, 0.16f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            display.swap();
            batch.release();
            display.close();
            return {};
        }
        LoadLanguage(font);
        std::uint32_t font_texture = batch.create_font_texture(font);
        const pe::ui::Fonts fonts{&font, font_texture};

        Eden::Report("setup", "Checking supplied keys and firmware");
        EdenServices services(launch_error, rooms);
        pe::ui::Textures textures(batch, services);
        if (!textures.load_art(Eden::AppFile("ui")))
            Eden::Report("menu", "Launcher art is incomplete; check the app's ui/art folder");
        textures.set_output_scale(static_cast<float>(display.width()) / 1920.0f);

        // Sound: its own thread feeds the console's audio port from the mixer.
        auto mixer = std::make_unique<pe::audio::Mixer>();
        pe::audio::SoundBank sounds;
        std::vector<std::string> sound_errors;
        const int sound_files = sounds.load(Eden::AppFile("ui/sounds"), &sound_errors);
        for (const auto& error : sound_errors) Eden::Report("menu sound", error.c_str());
        pe::ps5::AudioOut audio;
        const bool audio_ready = Eden::AudioOutReady() && audio.start(*mixer);
        if (!audio_ready) Eden::Report("menu sound", "Audio output is not available; the launcher stays silent");

        const bool input_ready = radio_input_init();
        if (!input_ready) Eden::Report("menu failure", "The controller could not be opened");
        // Once per launch: is a newer release listed? The launcher shows the answer when it comes.
        Eden::UpdateNotice::Start();
        pe::ui::Launcher launcher(services, textures, fonts, first_start);
        int menu_volume = launcher.menu_volume();
        mixer->set_bus_gain(pe::audio::Bus::ui, MenuGain(menu_volume));
        std::fprintf(stderr, "EDEN_LAUNCHER display=%dx%d sounds=%d audio=%d ready_ms=%lld\n", display.width(),
                     display.height(), sound_files, audio_ready, Milliseconds(Clock::now() - opened));

        bool running = input_ready;
#ifdef EDEN_DEV_ROM_ID
        static Eden::DevelopmentInput development_input;
        unsigned development_poll = 0;
        // A capture waits for the screen to settle after the input that asked for it.
        int capture_wait = 60;
        std::fprintf(stderr, "EDEN_DEV_LAUNCHER_READY ready=%d\n", running);
#endif
        pe::gfx::DrawList list;
        pe::gfx::Viewport viewport = pe::gfx::fit_viewport(display.width(), display.height());
        auto previous = Clock::now();
        bool first_frame = true;
        while (running && !launcher.done()) {
            const auto frame_start = Clock::now();
            // Start-to-start frame time; a long frame does not make the animations jump.
            const float dt = first_frame ? 1.0f / 60.0f :
                std::min(0.05f, std::chrono::duration<float>(frame_start - previous).count());
            previous = frame_start;
            first_frame = false;

#ifdef EDEN_DEV_ROM_ID
            const auto now = static_cast<std::uint64_t>(Milliseconds(frame_start.time_since_epoch()));
            if (++development_poll >= 10) {
                development_poll = 0;
                std::ifstream command(Eden::AppFile("compat-input.txt"));
                if (development_input.Read(command, now)) {
                    capture_wait = 60;
                    std::fprintf(stderr, "EDEN_DEV_UI_INPUT sequence=%llu buttons=%x\n",
                        static_cast<unsigned long long>(development_input.sequence), development_input.buttons);
                }
                // The runner's quit request: leave the launcher and end the process normally, so a run
                // ends without killing the app (two console losses followed killed runs).
                if (std::remove(Eden::AppFile("quit-app.txt").c_str()) == 0) {
                    std::fprintf(stderr, "EDEN_DEV_QUIT requested=1\n");
                    running = false;
                }
                // The check that the PS5 keyboard opens (system_keyboard.h): asked for from the
                // launcher, left open for a few seconds, closed again.
                if (std::remove(Eden::AppFile("keyboard-test.txt").c_str()) == 0) {
                    std::thread([] {
                        std::atomic<bool> stop{false};
                        std::thread closer([&stop] {
                            std::this_thread::sleep_for(std::chrono::seconds(6));
                            stop = true;
                        });
                        Eden::TextRequest request;
                        request.title = u"ProsperoEden";
                        request.initial = u"Eden";
                        request.max_length = 16;
                        const Eden::TextAnswer answer = Eden::AskSystemKeyboard(request, stop);
                        closer.join();
                        std::fprintf(stderr, "EDEN_DEV_KEYBOARD outcome=%d length=%zu\n",
                                     static_cast<int>(answer.outcome), answer.text.size());
                    }).detach();
                }
                // The runner's crash request, to test the crash report in the launcher.
                Eden::Crash::DevelopmentRequest(Eden::AppFile("crash-app.txt"));
                // The check of what a stop that runs into its limit does (stop_limit.h): the app
                // starts again, here from an idle launcher.
                if (std::remove(Eden::AppFile("restart-app.txt").c_str()) == 0) {
                    std::fprintf(stderr, "EDEN_DEV_RESTART requested=1\n");
                    Eden::StopLimit::RestartNow();
                    std::fprintf(stderr, "EDEN_DEV_RESTART refused=1\n");
                }
            }
            if (!development_input.active) radio_input_poll();
            if (const auto sample = development_input.Sample(now)) {
                static_assert(sizeof(*sample) == 120);
                radio_input_development_sample(&*sample);
            }
#else
            radio_input_poll();
#endif
            radio_input_event_t input{};
            while (radio_input_next(&input)) {
                if (!input.pressed) continue;
                // A slow handler names itself in the log (opening the Library reads every game).
                const auto pressed_at = Clock::now();
                launcher.press(static_cast<pe::ui::Key>(input.key));
                const auto ms = Milliseconds(Clock::now() - pressed_at);
                if (ms >= 50)
                    Eden::Report("slow input", ("key " + std::to_string(static_cast<int>(input.key)) + ": " +
                                                std::to_string(ms) + " ms").c_str());
            }
            launcher.update(dt);
            if (launcher.output() != output) {
                // Output resolution changed in Settings > Video: the display opens again at the new
                // size, and with it everything that lives in its GL context. The launcher stays as
                // it is, on the row that was changed.
                const auto reopen = Clock::now();
                output = output_open = std::clamp(launcher.output(), 0, static_cast<int>(std::size(Eden::kOutputKeys)) - 1);
                textures.release();
                batch.delete_texture(font_texture);
                font_texture = 0;
                batch.release();
                display.close();
                if (!OpenDisplay(display, &output_open) || !batch.init()) {
                    Eden::Report("menu video failure", pe::ps5::egl_error_name(display.last_error()));
                    break;
                }
                font_texture = batch.create_font_texture(font);
                launcher.set_font_texture(font_texture);
                if (!textures.load_art(Eden::AppFile("ui")))
                    Eden::Report("menu", "Launcher art is incomplete; check the app's ui/art folder");
                textures.set_output_scale(static_cast<float>(display.width()) / 1920.0f);
                viewport = pe::gfx::fit_viewport(display.width(), display.height());
                std::fprintf(stderr, "EDEN_LAUNCHER display=%dx%d reopened_ms=%lld\n", display.width(),
                             display.height(), Milliseconds(Clock::now() - reopen));
            }
            for (const pe::audio::Cue cue : launcher.take_cues()) sounds.play(*mixer, cue);
            if (launcher.menu_volume() != menu_volume) {
                menu_volume = launcher.menu_volume();
                mixer->set_bus_gain(pe::audio::Bus::ui, MenuGain(menu_volume));
            }
            const auto updated = Clock::now();

            list.clear();
            launcher.draw(list);
            batch.sync_font_texture(font_texture, font);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            batch.draw(list, viewport, display.width(), display.height());
            const auto drawn = Clock::now();
            bool captured = false;
#ifdef EDEN_DEV_ROM_ID
            if (capture_wait > 0 && --capture_wait == 0 && std::ifstream(Eden::AppFile("ui-capture.txt")).good()) {
                const int rc = SaveCapture(Eden::LogFile("launcher.bmp"), display.width(), display.height());
                std::fprintf(stderr, "EDEN_DEV_UI_CAPTURE rc=%d\n", rc);
                captured = true; // saving the picture is slow, the frame is not
            }
#endif
            if (!display.swap()) {
                Eden::Report("menu video failure", pe::ps5::egl_error_name(display.last_error()));
                running = false;
            }
            {
                // A slow launcher frame names its stage in the log, at most once a second.
                const auto done = Clock::now();
                static auto last_report = done - std::chrono::seconds(2);
                if (!captured && done - frame_start >= std::chrono::milliseconds(100) &&
                    done - last_report >= std::chrono::seconds(1)) {
                    last_report = done;
                    const std::string detail = "update " + std::to_string(Milliseconds(updated - frame_start)) +
                        " ms, draw " + std::to_string(Milliseconds(drawn - updated)) +
                        " ms, present " + std::to_string(Milliseconds(done - drawn)) + " ms";
                    Eden::Report("slow frame", detail.c_str());
                }
            }
        }

        selected_game = launcher.selected_game();
        if (!launcher.done()) selected_game.clear();
        // Let the last sound end (the screen is already dark), then give everything back.
        for (int wait = 0; audio_ready && wait < 60 && mixer->active_voices() > 0; ++wait) sceKernelUsleep(10000);
        audio.stop();
        if (input_ready) radio_input_shutdown();
        textures.release();
        batch.delete_texture(font_texture);
        batch.release();
    }
    display.close();
    std::fprintf(stderr, "EDEN_LAUNCHER closed game=%d\n", !selected_game.empty());
    return selected_game;
}

} // namespace

std::string SelectProsperoEdenGame(Eden::Multiplayer::RoomClient& rooms, const std::string& launch_error) {
    // The first time is the app opening; afterwards the launcher returns from a game.
    static bool first_start = true;
    return RunApp(launch_error, std::exchange(first_start, false), rooms);
}
