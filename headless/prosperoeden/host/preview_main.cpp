// ProsperoEden - Launcher preview on a PC: draws the real screens to PNG files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: pe_preview <ui assets dir> <output dir> [width height]
//        pe_preview <ui assets dir> <output dir> --tour [width height]   (raw RGBA frames on stdout)
// Renders with the launcher's own draw list, shaders and font through Mesa's
// surfaceless EGL (llvmpipe), with sample data in place of the console.

#include "fake_services.hpp"
#include "pe/core/file.hpp"
#include "pe/gfx/gl_batch.hpp"
#include "pe/gfx/gl_program.hpp"
#include "pe/gfx/system_fonts.hpp"
#include "pe/ui/launcher.hpp"

#include <array>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using pe::ui::Key;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr ?
            get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) :
            eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

struct Stage
{
    pe::host::FakeServices &services;
    pe::ui::Textures &textures;
    pe::ui::Fonts fonts;
    pe::gfx::GlBatch &batch;
    int width;
    int height;
    std::string output;
    std::unique_ptr<pe::ui::Launcher> launcher;
    pe::gfx::DrawList list;
    std::vector<unsigned char> pixels;
    bool tour = false;
    bool ok = true;
    std::vector<pe::audio::Cue> heard;

    void restart(bool first_start = false)
    {
        launcher = std::make_unique<pe::ui::Launcher>(services, textures, fonts, first_start);
    }
    void frame()
    {
        launcher->update(1.0f / 60.0f);
        for (pe::audio::Cue cue : launcher->take_cues())
            heard.push_back(cue);
        if (tour)
            emit();
    }
    void wait(float seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * 60.0f + 0.5f); ++i)
            frame();
    }
    void press(std::initializer_list<Key> keys, float pause = 0.0f)
    {
        for (Key key : keys)
        {
            launcher->press(key);
            frame();
            if (pause > 0.0f)
                wait(pause);
        }
    }
    void render()
    {
        list.clear();
        launcher->draw(list);
        batch.sync_font_texture(fonts.texture, *fonts.font);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        batch.draw(list, pe::gfx::fit_viewport(width, height), width, height);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    // One frame of the tour, top row first, to stdout.
    void emit()
    {
        render();
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        for (int y = height - 1; y >= 0; --y)
            std::fwrite(pixels.data() + static_cast<std::size_t>(y) * stride, 1, stride, stdout);
    }
    void shoot(const char *name)
    {
        if (tour)
            return;
        render();
        const std::string path = output + "/" + name + ".png";
        const bool written =
            stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0;
        std::fprintf(stderr, "%s: %zu instances, %zu draw calls, GL error 0x%x%s\n", name,
                     list.instances().size(), batch.last_draw_calls(), glGetError(),
                     written ? "" : " (not written)");
        ok = ok && written;
    }
};

void pictures(Stage &s)
{
    // Home, as it settles after the app opens.
    s.restart(true);
    s.wait(0.42f);
    s.shoot("00-arriving");
    s.wait(3.0f);
    s.shoot("01-home");
    s.press({Key::right});
    s.wait(0.6f);
    s.shoot("02-home-details-focus");
    s.press({Key::down, Key::right});
    s.wait(0.6f);
    s.shoot("03-home-recent-focus");
    s.press({Key::up, Key::up, Key::right});
    s.wait(0.6f);
    s.shoot("04-home-settings-focus");

    // Library.
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(0.14f);
    s.shoot("05-opening-library");
    s.wait(1.2f);
    s.shoot("06-library");
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down,
             Key::down, Key::down});
    s.wait(0.8f);
    s.shoot("07-library-scrolled");
    s.press({Key::right});
    s.wait(0.6f);
    s.shoot("08-library-handheld");
    // The Library's Mods switch: all of this game's mods off, and on again.
    s.press({Key::square});
    s.wait(0.6f);
    s.shoot("44-library-mods-off");
    s.press({Key::square});
    s.wait(0.6f);
    s.shoot("45-library-mods-on");
    s.press({Key::triangle});
    s.wait(0.8f);
    s.shoot("09-game-settings");
    // The game's own Video: a renderer of its own, then the output's refresh rate.
    s.press({Key::down, Key::cross});
    s.wait(0.6f);
    s.shoot("49-game-video");
    s.press({Key::right});
    s.wait(0.6f);
    s.shoot("10-game-settings-changed");
    s.press({Key::down, Key::down, Key::down, Key::right, Key::right});
    s.wait(0.6f);
    s.shoot("40-game-refresh");
    // Its own Performance: Compile ahead switched on for this game only.
    s.press({Key::circle, Key::down, Key::cross, Key::right, Key::right});
    s.wait(0.6f);
    s.shoot("50-game-performance");
    // Its own Controls: a button mapping of its own, and A moved to another button.
    s.press({Key::circle, Key::down, Key::down, Key::cross, Key::down, Key::right});
    s.wait(0.6f);
    s.shoot("51-game-controls");
    s.press({Key::cross, Key::right});
    s.wait(0.6f);
    s.shoot("52-game-mapping");
    {
        const pe::ui::GameSettings game = s.services.game_settings(0);
        if (game.renderer != 0 || game.refresh != 1 || game.performance[0] != 1 || !game.own_mapping ||
            game.mapping[0] != 2 || game.mapping[3] != 1)
        {
            std::fprintf(stderr, "error: the game's own settings did not reach the settings\n");
            s.ok = false;
        }
    }
    // Back in the game's settings: the kinds it changed say how many.
    s.press({Key::circle, Key::circle});
    s.wait(0.6f);
    s.shoot("53-game-settings-changed");
    // The game's mods: the row, the list, one switched on, and a game that has none.
    s.press({Key::down, Key::down});
    s.wait(0.6f);
    s.shoot("35-game-mods-row");
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("36-mods");
    s.press({Key::down, Key::cross});
    s.wait(0.4f);
    s.shoot("37-mods-switched");
    // A mod that lists several cheats: each has its switch, and one frame rate takes the place
    // of the other.
    s.press({Key::down, Key::down, Key::down, Key::cross});
    s.wait(0.6f);
    s.shoot("46-mods-cheats");
    s.services.has_mods = false;
    s.press({Key::circle, Key::cross});
    s.wait(0.6f);
    s.shoot("38-mods-none");
    s.press({Key::square});
    s.wait(0.4f);
    s.shoot("39-mods-folder-made");
    s.services.has_mods = true;
    s.press({Key::circle, Key::down});
    s.wait(0.6f);
    s.shoot("32-game-save-data");
    s.press({Key::cross});
    s.wait(0.4f);
    s.shoot("33-game-save-asked");
    s.press({Key::square});
    s.wait(0.4f);
    s.shoot("34-game-save-exported");
    s.press({Key::circle});
    s.wait(0.3f);
    s.press({Key::cross});
    s.wait(0.30f);
    s.shoot("11-launching");
    s.wait(0.32f);
    s.shoot("12-launching-late");

    // A newer release the app cannot install itself: the notification at the top right, for ten
    // seconds.
    s.restart();
    s.wait(1.0f);
    s.services.update_version = "v1.000.060";
    s.services.update_installable = false;
    s.wait(1.2f);
    s.shoot("58-update-notice");
    s.wait(9.5f);
    s.shoot("59-update-notice-gone");
    s.services.update_installable = true;

    // One it can: the offer when the app opens, then the install (a step per frame in the
    // preview: 40 preparing, 360 downloading, 120 unpacking).
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(2.6f);
    s.shoot("70-update-offer");
    s.press({Key::right, Key::right});
    s.wait(0.4f);
    s.shoot("71-update-offer-skip");
    s.press({Key::left, Key::left, Key::cross});
    s.wait(0.3f);
    s.shoot("72-update-preparing");
    s.wait(3.2f);
    s.shoot("73-update-downloading");
    s.wait(3.5f);
    s.shoot("74-update-unpacking");
    s.wait(2.2f);
    s.shoot("75-update-ready");
    s.wait(0.9f);
    s.shoot("76-update-ready-late");
    // Cancelled while it downloads: the dialog closes, nothing changed.
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(2.6f);
    s.press({Key::cross});
    s.wait(1.5f);
    s.press({Key::circle});
    s.wait(0.1f);
    s.shoot("77-update-cancelling");
    s.wait(1.0f);
    s.shoot("78-update-cancelled");
    // A failure: what went wrong, and Try again or Close.
    s.services.update_version = "v1.000.060";
    s.services.update_fails = true;
    s.restart(true);
    s.wait(2.6f);
    s.press({Key::cross});
    s.wait(7.4f);
    s.shoot("79-update-failed");
    s.services.update_fails = false;

    // The release notes: What's new on the offer opens them; they scroll a few lines at a time
    // (Up/Down) or a page (L1/R1), give a little at the end, and Circle goes back to the offer.
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(2.6f);
    s.press({Key::right});
    s.wait(0.4f);
    s.shoot("80-update-offer-whats-new");
    s.press({Key::cross});
    s.wait(0.12f);
    s.shoot("81-update-notes-opening");
    s.wait(1.0f);
    s.shoot("82-update-notes");
    s.press({Key::down, Key::down}, 0.15f);
    s.wait(0.6f);
    s.shoot("83-update-notes-scrolled");
    s.press({Key::r1, Key::r1, Key::r1}, 0.15f);
    s.wait(0.7f);
    s.shoot("84-update-notes-end");
    s.press({Key::down});
    s.wait(0.05f);
    s.shoot("85-update-notes-end-give");
    s.press({Key::right});
    s.wait(0.4f);
    s.shoot("86-update-notes-back-button");
    s.press({Key::circle});
    s.wait(0.5f);
    s.shoot("87-update-offer-after-notes");
    // A release without notes: two buttons, as before.
    const std::string notes = s.services.update_notes;
    s.services.update_notes.clear();
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(2.6f);
    s.shoot("88-update-offer-no-notes");
    // Notes the catalog cut short end with where the rest is.
    s.services.update_notes = notes;
    s.services.update_notes_truncated = true;
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(2.6f);
    s.press({Key::triangle});
    s.wait(1.2f);
    // (The preview draws only when it takes a picture; the notes are laid out when first drawn.)
    s.shoot("89-update-notes-truncated");
    s.press({Key::r1, Key::r1, Key::r1, Key::r1}, 0.15f);
    s.wait(0.8f);
    s.shoot("89-update-notes-truncated-end");
    s.services.update_notes_truncated = false;

    // A game's file taken away while the menu shows it: within a moment it leaves the home screen
    // (another recent game takes its place) and the Library.
    s.restart();
    s.wait(1.0f);
    for (const pe::ui::Game &game : s.services.games())
        if (game.name == "Echoes of the Valley")
            s.services.removed.push_back(game.file);
    s.wait(2.5f);
    s.shoot("56-home-game-removed");
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.shoot("57-library-game-removed");
    s.services.removed.clear();

    // Settings and its dialogs.
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::right, Key::cross});
    s.wait(1.0f);
    s.shoot("13-settings");
    // Profiles, the first category: a new one, playing as it, and taking it off the list again.
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("60-profiles");
    // The highlight follows the selection (it glides: the picture is taken once it has arrived).
    s.press({Key::down});
    s.wait(0.6f);
    s.shoot("60b-profiles-moved");
    s.press({Key::up});
    s.wait(0.2f);
    s.press({Key::down, Key::cross});
    s.wait(0.6f);
    s.shoot("61-profile-added");
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("62-profile-playing");
    {
        const std::vector<pe::ui::Profile> people = s.services.profiles();
        bool reached = people.size() == 2 && !people[0].playing && people[1].playing;
        // The one that is playing cannot be taken off the list; the other one can, asked twice.
        s.press({Key::square, Key::square});
        reached = reached && s.services.profiles().size() == 2;
        s.press({Key::up, Key::cross, Key::down, Key::square});
        s.wait(0.6f);
        s.shoot("63-profile-remove-asked");
        reached = reached && s.services.profiles().size() == 2;
        s.press({Key::square});
        reached = reached && s.services.profiles().size() == 1 && s.services.profiles()[0].playing;
        if (!reached)
        {
            std::fprintf(stderr, "error: the profiles did not reach the services\n");
            s.ok = false;
        }
    }
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("14-video");
    s.press({Key::down, Key::right});
    s.wait(0.6f);
    s.shoot("15-video-saved");
    s.press({Key::down, Key::down, Key::down, Key::right});
    s.wait(0.6f);
    s.shoot("41-video-refresh");
    // The sixth row: the list scrolls to it.
    s.press({Key::down});
    s.wait(0.6f);
    s.shoot("43-video-overlay");
    // Performance: seven switches, four of them showing. Reactive flushing starts on, the
    // others off, and a press on each one reaches the settings.
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("46-performance");
    {
        const auto states = [&s]
        {
            const pe::ui::Preferences p = s.services.preferences();
            return std::array<bool, 7>{p.block_list, p.async_shaders, p.fast_gpu, p.unsafe_cpu,
                                       p.unsafe_dma, p.reactive_flushing, p.skip_invalidation};
        };
        const std::array<bool, 7> start = states();
        bool reached = start == std::array<bool, 7>{false, false, false, false, false, true, false};
        for (std::size_t row = 0; row < start.size(); ++row)
        {
            if (row != 0)
                s.press({Key::down});
            s.press({Key::cross});
            std::array<bool, 7> expected = start;
            for (std::size_t changed = 0; changed <= row; ++changed)
                expected[changed] = !start[changed];
            reached = reached && states() == expected;
            if (row == 2)
            {
                s.wait(0.6f);
                s.shoot("47-performance-changed");
            }
        }
        // The seventh row: the list scrolled to it.
        s.wait(0.6f);
        s.shoot("48-performance-scrolled");
        if (!reached)
        {
            std::fprintf(stderr, "error: the Performance switches did not reach the settings\n");
            s.ok = false;
        }
    }
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("16-audio");
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("17-controls");
    // The button mapping: B moved to Circle takes A's place (A gets Cross), then all back.
    s.press({Key::down});
    s.wait(0.4f);
    s.shoot("54-controls-mapping-row");
    s.press({Key::cross, Key::down, Key::right});
    s.wait(0.6f);
    s.shoot("55-mapping");
    const pe::ui::ButtonMapping moved = s.services.preferences().mapping;
    s.press({Key::square});
    s.wait(0.4f);
    if (moved[0] != 0 || moved[1] != 1 || s.services.preferences().mapping != pe::ui::kDefaultMapping)
    {
        std::fprintf(stderr, "error: the button mapping did not reach the settings\n");
        s.ok = false;
    }
    s.press({Key::circle});
    s.wait(0.4f);
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("30-accessibility");
    s.press({Key::down, Key::down});
    s.wait(0.6f);
    s.shoot("31-accessibility-motion");
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("18-diagnostics");
    s.press({Key::down});
    s.wait(0.6f);
    s.shoot("18b-diagnostics-logs-at-once");
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(1.0f);
    s.shoot("19-game-files");
    s.press({Key::down, Key::down, Key::triangle});
    s.wait(0.6f);
    s.shoot("20-game-files-saved");
    s.press({Key::circle});
    s.wait(0.5f);
    s.press({Key::down, Key::down, Key::down, Key::cross}); // Multiplayer
    s.wait(0.8f);
    s.shoot("C0-multiplayer-offline");
    s.press({Key::down, Key::down, Key::down, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("C1-multiplayer-connecting");
    if (s.services.room.phase != Eden::Multiplayer::Phase::Connecting) s.ok = false;
    s.services.room = {Eden::Multiplayer::Phase::Connected, "", "Friends' room", {"PlayerPS5", "Desktop player"}};
    s.wait(0.8f);
    s.shoot("C2-multiplayer-connected");
    s.services.room = {Eden::Multiplayer::Phase::Failed, "The connection to the room was lost.", {}, {}};
    s.wait(0.8f);
    s.shoot("C3-multiplayer-lost");
    s.press({Key::square});
    s.wait(0.5f);
    if (s.services.room.phase != Eden::Multiplayer::Phase::Idle) s.ok = false;
    s.press({Key::circle, Key::down, Key::cross});
    s.wait(1.0f);
    s.shoot("21-language");
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down,
             Key::down, Key::down, Key::cross});
    s.wait(0.8f);
    s.shoot("22-language-chosen");

    // Two profiles: the home screen names who is playing.
    s.services.add_profile();
    s.restart();
    s.wait(2.0f);
    s.shoot("64-home-two-profiles");
    s.services.people.resize(1);

    // About.
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::right, Key::right, Key::cross});
    s.wait(1.0f);
    s.shoot("23-about");

    // What the home screen says when something is wrong, and before any game was played.
    s.services.setup_ready = false;
    s.restart();
    s.wait(2.0f);
    s.shoot("24-home-setup-required");
    s.services.setup_ready = true;
    s.services.launch_error = "The game's keys are missing from prod.keys.";
    s.restart();
    s.wait(2.0f);
    s.shoot("25-home-launch-failed");
    s.services.launch_error.clear();
    s.services.crash_report = "/data/prosperoeden/logs/crash-20261001-213000.txt";
    s.restart();
    s.wait(2.0f);
    s.shoot("25b-home-crash-report");
    s.services.crash_report.clear();
    s.services.has_history = false;
    s.restart();
    s.wait(2.0f);
    s.shoot("26-home-first-run");
    s.services.has_history = true;

    // Controllers: one, then a third one joining (caught mid-bounce), then all four.
    s.services.connected_controllers = 0b0001;
    s.restart();
    s.wait(2.0f);
    s.shoot("27-home-one-controller");
    s.services.connected_controllers = 0b0101;
    s.wait(0.2f);
    s.shoot("28-home-controller-joining");
    s.services.connected_controllers = 0b1111;
    s.wait(2.0f);
    s.shoot("29-home-four-controllers");
    s.services.connected_controllers = 0b0011;

    // Games on download sources: listed beside the console's own, on their sources until downloaded.
    s.services.has_server = true;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down}); // Lighthouse Keeper
    s.wait(0.6f);
    s.shoot("80-library-server-game");
    // Square: into the download queue; it starts at once.
    s.press({Key::square});
    s.wait(0.5f);
    s.shoot("81-library-queued");
    s.wait(2.0f);
    s.shoot("82-library-downloading");
    // Cross on another: played once it is downloaded, after the one downloading.
    s.press({Key::down, Key::down, Key::down}); // Orbit Postman
    s.wait(0.4f);
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("83-download-waiting");
    // Its turn: it goes on from where it was, what it had read first for the check of its contents.
    for (int frame = 0; frame < 1200; ++frame)
    {
        bool checking = false;
        for (const pe::ui::Download &download : s.services.downloads_now())
            checking = checking || download.state == pe::ui::DownloadState::verifying;
        if (checking)
            break;
        s.frame();
    }
    s.wait(0.3f);
    s.shoot("83b-download-checking");
    s.wait(4.0f);
    s.shoot("84-download-running");
    // Done: the dialog closes and the game starts.
    for (int frame = 0; frame < 900 && s.launcher->selected_game().empty(); ++frame)
        s.frame();
    s.wait(0.3f);
    s.shoot("85-download-done-starting");
    {
        bool joined = false;
        for (const pe::ui::Game &game : s.services.games())
            joined = joined || (game.name == "Orbit Postman" && !game.remote);
        if (!joined || s.launcher->selected_game().empty())
        {
            std::fprintf(stderr, "error: a downloaded game did not join the library and start\n");
            s.ok = false;
        }
    }
    // A downloaded game from the server is deleted from the console in its settings, asked first.
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down}); // Orbit Postman
    s.wait(0.6f);
    s.shoot("94-library-downloaded");
    s.press({Key::triangle});
    s.wait(0.6f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down});
    s.wait(0.6f);
    s.shoot("95-game-delete-row");
    s.press({Key::cross});
    s.wait(0.4f);
    s.shoot("96-game-delete-asked");
    s.press({Key::cross});
    s.wait(1.0f);
    s.shoot("97-library-deleted");
    {
        bool remote = false;
        for (const pe::ui::Game &game : s.services.games())
            remote = remote || (game.name == "Orbit Postman" && game.remote);
        if (!remote)
        {
            std::fprintf(stderr, "error: a deleted game is not on the server only again\n");
            s.ok = false;
        }
    }
    // A download that stops part way: try again, cancel or close.
    s.services.download_fails = true;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down,
             Key::down, Key::down, Key::down}); // Rune Gardens
    s.wait(0.4f);
    // Two sources have it: which one, first.
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("98-choose-source");
    s.press({Key::cross});
    s.wait(5.0f);
    s.shoot("86-download-failed");
    s.press({Key::circle});
    s.wait(0.6f);
    s.shoot("87-library-download-failed");
    // Settings > Downloads: the sources and the queue.
    s.press({Key::circle});
    s.wait(0.8f);
    s.press({Key::up, Key::right, Key::cross});
    s.wait(1.0f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down});
    s.wait(0.6f);
    s.shoot("88-settings-downloads");
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("89-downloads");
    s.press({Key::down});
    s.wait(0.6f);
    s.shoot("90-downloads-failed-row");
    s.press({Key::square});
    s.wait(0.6f);
    s.shoot("91-downloads-cancelled");
    s.services.download_fails = false;
    // The server cannot be reached, and no server at all.
    s.services.server_fails = true;
    s.press({Key::circle});
    s.wait(0.6f);
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("92-downloads-source-offline");
    s.services.server_fails = false;
    s.services.has_server = false;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::right, Key::cross});
    s.wait(1.0f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down,
             Key::cross});
    s.wait(0.8f);
    s.shoot("93-downloads-not-set-up");

    // Save sync: before a game starts its save data is put in step with the server's.
    s.services.save_sync_on = true;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("A0-save-sync-working");
    // Both changed: the player chooses.
    s.services.save_sync_conflict = true;
    s.wait(2.0f);
    s.shoot("A1-save-sync-conflict");
    s.press({Key::down});
    s.wait(0.4f);
    s.shoot("A2-save-sync-conflict-server");
    s.press({Key::cross});
    for (int frame = 0; frame < 600 && s.launcher->selected_game().empty(); ++frame)
        s.frame();
    if (s.launcher->selected_game().empty())
    {
        std::fprintf(stderr, "error: the game did not start after its save data was synced\n");
        s.ok = false;
    }
    // The server cannot be reached: play anyway, try again, or not.
    s.services.save_sync_fails = true;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::cross});
    s.wait(3.0f);
    s.shoot("A3-save-sync-failed");
    s.press({Key::down, Key::down, Key::cross});
    s.wait(0.8f);
    s.services.save_sync_fails = false;
    // After a game, once the menu is back: its save data goes up, with a notice.
    s.services.played("Starfall Odyssey");
    s.restart(false);
    s.wait(2.4f);
    s.shoot("A4-save-sync-backed-up");
    // A server too old for the save sync: before a game, play anyway or not; after one, confirmed.
    s.services.save_sync_too_old = true;
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::cross});
    s.wait(1.2f);
    s.press({Key::cross});
    s.wait(3.0f);
    s.shoot("A5-save-sync-too-old");
    s.press({Key::circle});
    s.wait(0.8f);
    s.services.played("Starfall Odyssey");
    s.restart(false);
    s.wait(3.0f);
    s.shoot("A6-save-sync-too-old-after");
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("A7-save-sync-too-old-confirmed");
    s.services.save_sync_too_old = false;
    s.services.save_sync_on = false;

    // Settings > Save sync: the profiles and their servers; pairing one by a QR code.
    s.restart();
    s.wait(1.0f);
    s.press({Key::up, Key::right, Key::cross});
    s.wait(1.0f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down});
    s.wait(0.6f);
    s.shoot("B0-settings-save-sync");
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("B1-save-sync-profiles");
    s.press({Key::cross});
    s.wait(0.6f);
    s.shoot("B2-save-sync-choose-server");
    s.press({Key::cross});
    s.wait(1.5f);
    s.shoot("B3-pairing-code");
    s.wait(10.0f);
    s.shoot("B4-pairing-done");
    s.press({Key::cross});
    s.wait(0.8f);
    s.shoot("B5-save-sync-paired");
    s.press({Key::down, Key::square});
    s.wait(0.4f);
    s.shoot("B6-save-sync-unlink-asked");
}

// A walk through the launcher, one frame per call of frame().
void tour(Stage &s)
{
    s.services.connected_controllers = 0b0001;
    s.restart(true);
    s.wait(2.0f);
    // A second controller is switched on, then a third.
    s.services.connected_controllers = 0b0011;
    s.wait(1.2f);
    s.services.connected_controllers = 0b0111;
    s.wait(1.4f);
    s.press({Key::right, Key::left, Key::down, Key::right, Key::right, Key::left, Key::up}, 0.32f);
    s.wait(0.5f);
    s.press({Key::up}, 0.4f);
    s.press({Key::cross});
    s.wait(1.0f);
    s.press({Key::down, Key::down, Key::down, Key::down, Key::down, Key::down, Key::down,
             Key::down, Key::up, Key::up},
            0.22f);
    s.press({Key::right}, 0.6f);
    s.press({Key::triangle}, 0.8f);
    s.press({Key::down, Key::right, Key::down, Key::right}, 0.4f);
    s.press({Key::circle}, 0.6f);
    s.press({Key::circle}, 0.8f);
    s.press({Key::right}, 0.3f);
    s.press({Key::cross});
    s.wait(0.9f);
    s.press({Key::down, Key::down, Key::up}, 0.3f);
    s.press({Key::cross}, 0.8f);
    s.press({Key::right, Key::down, Key::left}, 0.4f);
    s.press({Key::circle}, 0.6f);
    s.press({Key::circle}, 0.9f);
    s.press({Key::down}, 0.5f);
    s.press({Key::cross});
    s.wait(1.2f);
}

// PE_TOUR=update: the app opens with a newer release listed; the offer, Update now, the download
// and unpacking, and ProsperoEden closing for the update.
void update_tour(Stage &s)
{
    s.services.connected_controllers = 0b0001;
    s.services.update_version = "v1.000.060";
    s.restart(true);
    s.wait(3.4f);
    // What's new: the notes open, scroll down a little at a time, a page, then back.
    s.press({Key::right}, 0.6f);
    s.press({Key::cross}, 1.6f);
    s.press({Key::down}, 0.7f);
    s.press({Key::down}, 0.7f);
    s.press({Key::down}, 0.9f);
    s.press({Key::r1}, 1.2f);
    s.press({Key::r1}, 1.4f);
    s.press({Key::circle}, 0.9f);
    s.press({Key::left}, 0.6f);
    s.press({Key::cross});
    s.wait(10.5f);
}

} // namespace

namespace
{

// Text that did not fit its place at full size, and how it was fitted (the smallest scale seen).
std::map<std::string, std::pair<float, bool>> fits;

void note_fit(std::string_view text, float scale, bool cut)
{
    auto [entry, added] = fits.try_emplace(std::string{text}, scale, cut);
    if (!added)
    {
        entry->second.first = std::min(entry->second.first, scale);
        entry->second.second = entry->second.second || cut;
    }
}

// The catalog reader, the pattern filler and the choice of catalog, checked before anything is drawn.
bool strings_check()
{
    pe::Catalog catalog;
    const std::size_t count = catalog.load("\xef\xbb\xbf# comment\n"
                                           "msgid \"\"\nmsgstr \"Language: xx\\n\"\n\n"
                                           "#. note\n#: file.cpp\n"
                                           "msgid \"Back\"\r\nmsgstr \"Voltar\"\r\n\n"
                                           "msgid \"Say \\\"{0}\\\" twice\"\n"
                                           "msgstr \"\"\n\"Diga \\\"{0}\\\" \"\n\"duas vezes\"\n\n"
                                           "msgid \"Untranslated\"\nmsgstr \"\"\n");
    const auto same = [](const std::vector<std::string> &left, std::initializer_list<const char *> right)
    { return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                        [](const std::string &a, const char *b) { return a == b; }); };
    const bool ok =
        count == 2 && catalog.find("Back") == "Voltar" &&
        catalog.find("Say \"{0}\" twice") == "Diga \"{0}\" duas vezes" &&
        catalog.find("Untranslated") == "Untranslated" && catalog.find("Missing") == "Missing" &&
        pe::fill("{0} OF {1}", {"3", "12"}) == "3 OF 12" && pe::fill("{1}{0}{2}", {"a", "b"}) == "ba" &&
        pe::fill("{x} {0", {"a"}) == "{x} {0" &&
        same(pe::catalog_candidates("fr-CA"), {"fr-CA", "fr-FR"}) &&
        same(pe::catalog_candidates("pt-PT"), {"pt-PT", "pt-BR"}) &&
        same(pe::catalog_candidates("es-419"), {"es-419", "es-ES"}) &&
        same(pe::catalog_candidates("de-DE"), {"de-DE"}) && pe::catalog_candidates("en-GB").empty() &&
        pe::catalog_candidates("en-US").empty() && pe::catalog_candidates("").empty();
    if (!ok)
        std::fprintf(stderr, "error: the launcher's text functions fail their check\n");
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <ui assets dir> <output dir> [--tour] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    int argument = 3;
    const bool make_tour = argc > argument && std::strcmp(argv[argument], "--tour") == 0;
    if (make_tour)
        ++argument;
    const int width = argc > argument + 1 ? std::atoi(argv[argument]) : 1920;
    const int height = argc > argument + 1 ? std::atoi(argv[argument + 1]) : 1080;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    std::fprintf(stderr, "GL %s / %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    pe::gfx::set_glsl_prefix("#version 450 core\n");

    if (!strings_check())
        return 1;
    // PE_LANG=<tag>: the launcher in that language, as on a console set to it.
    if (const char *language = std::getenv("PE_LANG"); language != nullptr && language[0] != 0)
    {
        std::string used;
        for (const std::string &candidate : pe::catalog_candidates(language))
        {
            std::string catalog;
            if (pe::read_file(assets + "/lang/" + candidate + ".po", &catalog) &&
                pe::catalog().load(catalog) > 0)
            {
                used = candidate;
                break;
            }
        }
        std::fprintf(stderr, "language %s: catalog %s, %zu texts\n", language,
                     used.empty() ? "none (English)" : used.c_str(), pe::catalog().size());
        pe::ui::set_fit_report(note_fit);
    }

    pe::gfx::Font font;
    std::string font_data;
    if (!pe::read_file(assets + "/fonts/montserrat-medium.pefont", &font_data) ||
        !font.load(font_data))
    {
        std::fprintf(stderr, "cannot load the font: %s\n", font.error().c_str());
        return 1;
    }
    // The console's fonts for the scripts the baked one lacks (PE_SYSTEM_FONTS: copies of them).
    {
        const char *language = std::getenv("PE_LANG");
        const std::string tag = language != nullptr && language[0] != 0 ? language : "en-US";
        for (const std::string &folder : pe::gfx::system_font_folders())
        {
            std::vector<std::string> files = pe::gfx::system_font_files(folder, tag);
            if (files.empty())
                continue;
            std::fprintf(stderr, "system fonts: %zu files in %s\n", files.size(), folder.c_str());
            font.use_system_fonts(std::move(files), tag);
            break;
        }
        if (!pe::catalog().every([&font](std::string_view text) { return font.can_draw(text); }))
        {
            std::fprintf(stderr, "error: the catalog has characters no font here has; on the console the "
                                 "launcher would stay English (PE_SYSTEM_FONTS names a folder with the "
                                 "console's fonts)\n");
            return 1;
        }
    }
    pe::gfx::GlBatch batch;
    if (!batch.init())
        return 1;

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    pe::host::FakeServices services(output);
    // PE_LOOK=large,contrast,calm: the accessibility settings, in any combination.
    if (const char *wanted = std::getenv("PE_LOOK"); wanted != nullptr)
    {
        const std::string_view names{wanted};
        pe::ui::Preferences preferences = services.preferences();
        preferences.large_text = names.find("large") != std::string_view::npos;
        preferences.high_contrast = names.find("contrast") != std::string_view::npos;
        preferences.reduce_motion = names.find("calm") != std::string_view::npos;
        services.set_preferences(preferences);
    }
    pe::ui::Textures textures(batch, services);
    if (!textures.load_art(assets))
        std::fprintf(stderr, "warning: launcher art is incomplete\n");
    textures.set_output_scale(static_cast<float>(width) / 1920.0f);

    // PE_SPECIMEN=<file>: instead of the screens, one picture of the file's lines of text (a line
    // starting with "W " is wrapped to a column), to look at a script or a translation closely.
    if (const char *specimen = std::getenv("PE_SPECIMEN"); specimen != nullptr && specimen[0] != 0)
    {
        std::string lines;
        if (!pe::read_file(specimen, &lines))
        {
            std::fprintf(stderr, "cannot read %s\n", specimen);
            return 1;
        }
        const std::uint32_t texture = batch.create_font_texture(font);
        pe::gfx::DrawList list;
        list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, pe::ui::theme::kPanel);
        float x = 40.0f;
        float y = 60.0f;
        const float size = 30.0f;
        const float pitch = 44.0f;
        const float column = 900.0f;
        const auto advance = [&]
        {
            y += pitch;
            if (y > 1060.0f)
            {
                y = 60.0f;
                x += column + 40.0f;
            }
        };
        for (std::size_t start = 0; start < lines.size();)
        {
            std::size_t end = lines.find('\n', start);
            if (end == std::string::npos)
                end = lines.size();
            std::string_view line{lines.data() + start, end - start};
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.substr(0, 2) == "W ")
            {
                for (const std::string &part : font.wrap(line.substr(2), size, column))
                {
                    list.rounded_rect({x, y - size, column, pitch - 6.0f}, 4.0f, pe::ui::theme::kRow);
                    list.text(font, texture, part, x, y, size, pe::ui::theme::kText);
                    advance();
                }
                continue;
            }
            if (line.substr(0, 2) == "F ")
            {
                // Cut to a narrow place, as a long title is.
                list.rounded_rect({x, y - size, 420.0f, pitch - 6.0f}, 4.0f, pe::ui::theme::kRow);
                list.text(font, texture, font.fit(line.substr(2), size, 420.0f), x, y, size, pe::ui::theme::kText);
                advance();
                continue;
            }
            list.text(font, texture, line, x, y, size, pe::ui::theme::kText);
            list.text(font, texture, line, x + column, y, 20.0f, pe::ui::theme::kLime, pe::gfx::Align::right);
            advance();
        }
        batch.sync_font_texture(texture, font);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        batch.draw(list, pe::gfx::fit_viewport(width, height), width, height);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        stbi_flip_vertically_on_write(1);
        const std::string path = output + "/specimen.png";
        const bool written = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0;
        std::fprintf(stderr, "specimen: %zu instances, GL error 0x%x, system fonts read: %s\n", list.instances().size(),
                     glGetError(), font.system_fonts_read().c_str());
        return written ? 0 : 1;
    }

    Stage stage{services, textures, {&font, batch.create_font_texture(font)}, batch, width, height,
                output, nullptr, {}, {}, make_tour, true, {}};
    stage.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_flip_vertically_on_write(1);
    const char *which_tour = std::getenv("PE_TOUR");
    if (make_tour && which_tour && std::strcmp(which_tour, "update") == 0)
        update_tour(stage);
    else if (make_tour)
        tour(stage);
    else
        pictures(stage);
    std::fprintf(stderr, "cues heard: %zu\n", stage.heard.size());
    std::fprintf(stderr, "system fonts read: %s\n", font.system_fonts_read().c_str());
    for (const auto &[text, fit] : fits)
        std::fprintf(stderr, "%s %.2f: %s\n", fit.second ? "cut" : "shrunk", fit.first, text.c_str());
    return stage.ok ? 0 : 1;
}
