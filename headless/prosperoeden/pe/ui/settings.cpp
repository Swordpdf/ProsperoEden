// ProsperoEden - Launcher settings: the category list and its dialogs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

constexpr Rect kListPanel{108.0f, 188.0f, 820.0f, 720.0f};
constexpr Rect kDetailPanel{980.0f, 188.0f, 820.0f, 720.0f};
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 264.0f;
constexpr float kRowHeight = 50.0f;
enum Category
{
    kProfiles,
    kVideo,
    kPerformance,
    kAudio,
    kControls,
    kAccessibility,
    kDiagnostics,
    kFiles,
    kDownloads,
    kSaveSync,
    kMultiplayer,
    kLanguage,
    kCategoryCount,
};
constexpr const char *kCategories[kCategoryCount] = {
    TR("Profiles"), TR("Video"), TR("Performance"), TR("Audio"), TR("Controls"), TR("Accessibility"),
    TR("Diagnostics"), TR("Game files"), TR("Downloads"), TR("Save sync"), TR("Multiplayer"), TR("Language")};
// The same as headings: capitals differ by language, so each is its own text.
constexpr const char *kHeadings[kCategoryCount] = {
    TR("PROFILES"), TR("VIDEO"), TR("PERFORMANCE"), TR("AUDIO"), TR("CONTROLS"), TR("ACCESSIBILITY"),
    TR("DIAGNOSTICS"), TR("GAME FILES"), TR("DOWNLOADS"), TR("SAVE SYNC"), TR("MULTIPLAYER"), TR("LANGUAGE")};

// The Video dialog's rows, and the window that shows five of them (placed as a game's settings
// are).
enum VideoRow : int
{
    video_renderer,
    video_output,
    video_resolution,
    video_filter,
    video_refresh,
    video_overlay,
    // Frame generation: not shown in a build without it (Services::frame_gen_state).
    video_frame_gen,
    video_frame_gen_target,
    video_frame_gen_multiplier,
    kVideoRows,
};
constexpr int kFrameGenTargets = 6;     // Auto, 60, 90, 120, 144, 240 Hz (settings_store.h)
constexpr int kFrameGenMultipliers = 3; // 2x, 3x, 4x
constexpr float kVideoRowsTop = 334.0f;
constexpr float kVideoRowPitch = 96.0f;
constexpr float kVideoRowHeight = 94.0f;
constexpr int kVideoRowsShown = 5;
constexpr Rect kVideoWindow{592.0f, kVideoRowsTop, 736.0f,
                            kVideoRowPitch * (kVideoRowsShown - 1) + kVideoRowHeight};
// The Performance dialog's switches, and the window that shows four of them over what the
// highlighted one does.
enum PerformanceRow : int
{
    speed_block_list,
    speed_async_shaders,
    speed_fast_gpu,
    speed_unsafe_cpu,
    speed_unsafe_dma,
    speed_reactive_flushing,
    speed_skip_invalidation,
    kPerformanceRows,
};
constexpr int kPerformanceRowsShown = 4;
constexpr Rect kPerformanceWindow{592.0f, kVideoRowsTop, 736.0f,
                                  kVideoRowPitch * (kPerformanceRowsShown - 1) + kVideoRowHeight};
// The sizes of Preferences::output, as every language writes them.
constexpr const char *kOutputs[] = {"1080p", "1440p", "2160p"};

const char *output_name(int output)
{
    return kOutputs[std::clamp(output, 0, 2)];
}

constexpr const char *kFrameGenTargetNames[] = {"", "60", "90", "120", "144", "240"};
constexpr const char *kFrameGenMultiplierNames[] = {"2x", "3x", "4x"};

const char *on_off(bool value)
{
    return value ? tr("On") : tr("Off");
}

std::string percent(int value)
{
    return fill(tr("{0}%"), {std::to_string(value)});
}

std::string pick(const std::vector<std::string> &values, int index)
{
    return index >= 0 && index < static_cast<int>(values.size()) ?
               values[static_cast<std::size_t>(index)] : std::string{"-"};
}

// A path that fits a label: the end is what tells folders apart.
std::string short_path(const std::string &path, std::size_t limit)
{
    return path.size() <= limit ? path : "..." + path.substr(path.size() - (limit - 3));
}

} // namespace

void Launcher::press_settings(Key key)
{
    switch (key)
    {
    case Key::circle:
        open(Screen::home, false);
        return;
    case Key::up:
    case Key::down:
        if (settings_.move(key == Key::down ? 1 : -1))
        {
            section_.value = 0.0f;
            section_.velocity = 0.0f;
            cue(Cue::focus);
        }
        return;
    case Key::cross:
        press_ = 1.0f;
        switch (settings_.selected)
        {
        case kFiles:
            open(Screen::files, true);
            enter_files();
            break;
        case kLanguage:
            open(Screen::language, true);
            enter_language();
            break;
        case kProfiles:
            open_profiles();
            break;
        case kDownloads:
            open_sources();
            break;
        case kSaveSync:
            open_sync_setup();
            break;
        case kMultiplayer:
            open_multiplayer();
            break;
        case kVideo:
            open_modal(Modal::video);
            video_rows_.visible = kVideoRowsShown;
            video_rows_.pitch = kVideoRowPitch;
            video_rows_.reset(dialog_rows(Modal::video), 0);
            break;
        case kPerformance:
            open_modal(Modal::performance);
            performance_rows_.visible = kPerformanceRowsShown;
            performance_rows_.pitch = kVideoRowPitch;
            performance_rows_.reset(kPerformanceRows, 0);
            break;
        case kAudio:
            open_modal(Modal::audio);
            break;
        case kControls:
            open_modal(Modal::controls);
            break;
        case kAccessibility:
            open_modal(Modal::accessibility);
            break;
        default:
            open_modal(Modal::diagnostics);
            break;
        }
        return;
    default:
        return;
    }
}

void Launcher::draw_settings(Canvas &c)
{
    gfx::DrawList &list = c.list;
    draw_frame(c, tr("Settings"), tr("Fine-tune your experience"));

    // ---- categories ----
    glass(c, kListPanel, 26.0f, theme::kPanel.with_alpha(0.80f), theme::kPanelEdge.with_alpha(0.55f));
    text(c, tr("PREFERENCES"), 138.0f, baseline(208.0f, 28.0f, theme::kSmall), theme::kSmall,
         theme::kLimePale, Align::left, 3.0f);
    const auto row_rect = [&](int row) -> Rect
    { return {150.0f, kRowsTop + settings_.pitch * static_cast<float>(row), 736.0f, kRowHeight}; };
    for (int row = 0; row < kCategoryCount; ++row)
        plate_rest(c, kRowPlate, row_rect(row));
    plate_focus(c, kRowPlate, {150.0f, kRowsTop + settings_.cursor(), 736.0f, kRowHeight}, 1.0f);
    const std::string summaries[kCategoryCount] = {
        playing_,
        prefs_.renderer != 0 ? "Vulkan" : "OpenGL",
        prefs_.async_shaders || prefs_.fast_gpu || prefs_.unsafe_cpu || prefs_.unsafe_dma ||
                !prefs_.reactive_flushing || prefs_.skip_invalidation ? tr("On") : "",
        prefs_.mute ? tr("Muted") : percent(prefs_.volume),
        prefs_.vibration ? tr("Vibration on") : tr("Vibration off"),
        prefs_.large_text || prefs_.high_contrast || prefs_.reduce_motion ? tr("On") : "",
        prefs_.detailed_logging ? tr("Detailed logs on") : prefs_.immediate_logs ? tr("On") : "",
        "",
        !sources_.configured ? std::string{tr("Not set up")} :
        sources_.list.size() == 1 ? sources_.list.front().name :
                                    fill(tr("{0} sources"), {std::to_string(sources_.list.size())}),
        [&] {
            int linked = 0;
            for (const SaveSyncProfile &profile : sync_setup_.profiles)
                linked += profile.linked ? 1 : 0;
            return linked == 0 ? std::string{tr("Off")} : fill(tr("{0} of {1} profiles"), {std::to_string(linked),
                                                                                           std::to_string(sync_setup_.profiles.size())});
        }(),
        services_.room_status().phase == Eden::Multiplayer::Phase::Connected ? tr("Connected") : "",
        pick(services_.language_labels(), prefs_.language),
    };
    for (int row = 0; row < kCategoryCount; ++row)
    {
        const Rect r = row_rect(row);
        // What the category is set to, then a chevron: there is more behind the row.
        const float summary =
            text_shrink(c, summaries[row], r.x + r.w - 62.0f, baseline(r.y, r.h, theme::kSmall),
                        theme::kSmall, theme::kMeta, 330.0f, Align::right);
        text_shrink(c, tr(kCategories[row]), r.x + 36.0f, baseline(r.y, r.h, theme::kText24),
                    theme::kText24, theme::kValue, r.w - 36.0f - 62.0f - summary - 24.0f);
        const float cx = r.x + r.w - 34.0f;
        const float cy = r.y + r.h * 0.5f;
        const Color ink = theme::kLimePale.with_alpha(row == settings_.selected ? 0.95f : 0.4f);
        list.line(cx - 4.0f, cy - 8.0f, cx + 4.0f, cy, 2.2f, ink);
        list.line(cx + 4.0f, cy, cx - 4.0f, cy + 8.0f, 2.2f, ink);
    }

    // ---- what the focused category holds ----
    glass(c, kDetailPanel, 26.0f, theme::kPanel.with_alpha(0.80f),
          theme::kPanelEdge.with_alpha(0.55f));
    text(c, tr("ON THIS CONSOLE"), 1016.0f, baseline(210.0f, 28.0f, theme::kSmall), theme::kSmall,
         theme::kLimePale, Align::left, 3.0f);
    text_shrink(c, tr("Make it yours."), 1016.0f, baseline(258.0f, 54.0f, theme::kLead),
                theme::kLead, theme::kTitle, 748.0f);
    text_block(c, tr("Adjust the essentials without leaving your library behind."), 1016.0f,
               baseline(332.0f, 36.0f, theme::kText24), theme::kText24, 36.0f, theme::kCopy, 748.0f,
               2, kShrink);
    list.rounded_rect({1016.0f, 432.0f, 748.0f, 1.0f}, 0.0f, theme::kRule);

    struct Line
    {
        const char *label;
        std::string value;
    };
    std::vector<Line> lines;
    const char *about = "";
    const std::string folder = services_.files_folder();
    const std::string saved_folder = services_.saved_files_folder();
    switch (settings_.selected)
    {
    case kProfiles:
        about = tr("Who is playing. Each profile keeps its own save data and settings.");
        lines = {{tr("PLAYING"), playing_}, {tr("PROFILES"), std::to_string(profiles_.size())}};
        break;
    case kVideo:
        about = tr("Graphics backend and how games are scaled to your TV.");
        lines = {{tr("RENDERER"), prefs_.renderer != 0 ? tr("Vulkan (recommended)") : "OpenGL"},
                 {tr("OUTPUT RESOLUTION"), output_name(prefs_.output)},
                 {tr("RESOLUTION"), pick(services_.resolution_labels(), prefs_.resolution)},
                 {tr("UPSCALING FILTER"), pick(services_.filter_labels(), prefs_.filter)},
                 {tr("REFRESH RATE"), hertz(prefs_.refresh)},
                 {tr("FPS OVERLAY"), on_off(prefs_.hud)}};
        break;
    case kPerformance:
        about = tr("Faster games, at some cost in accuracy.");
        lines = {{tr("COMPILE AHEAD"), on_off(prefs_.block_list)},
                 {tr("ASYNCHRONOUS SHADERS"), on_off(prefs_.async_shaders)},
                 {tr("FASTER GPU EMULATION"), on_off(prefs_.fast_gpu)},
                 {tr("FASTER CPU EMULATION"), on_off(prefs_.unsafe_cpu)},
                 {tr("FASTER DMA"), on_off(prefs_.unsafe_dma)},
                 {tr("REACTIVE FLUSHING"), on_off(prefs_.reactive_flushing)},
                 {tr("SKIP CPU INVALIDATION"), on_off(prefs_.skip_invalidation)}};
        break;
    case kAudio:
        about = tr("Game volume, and the sounds of this menu.");
        lines = {{tr("GAME VOLUME"), percent(prefs_.volume)},
                 {tr("MUTE"), on_off(prefs_.mute)},
                 {tr("MENU SOUNDS"), prefs_.menu_volume > 0 ? percent(prefs_.menu_volume) : tr("Off")}};
        break;
    case kControls:
        about = tr("Vibration, button mapping and the shortcuts during a game.");
        lines = {{tr("VIBRATION"), on_off(prefs_.vibration)},
                 {tr("BUTTON MAPPING"), prefs_.mapping == kDefaultMapping ? tr("As usual") : tr("Changed")},
                 {tr("END GAME"), std::string(tr("Touchpad")) + " + L1"},
                 {tr("FPS OVERLAY"), std::string(tr("Touchpad")) + " + R1"}};
        break;
    case kAccessibility:
        about = tr("Make the menu easier to see and follow.");
        lines = {{tr("LARGER TEXT"), on_off(prefs_.large_text)},
                 {tr("HIGH CONTRAST"), on_off(prefs_.high_contrast)},
                 {tr("REDUCE MOTION"), on_off(prefs_.reduce_motion)}};
        break;
    case kDiagnostics:
        about = tr("Setup status and detailed logs.");
        lines = {{tr("SETUP"), home_.setup_ready ? tr("Ready") : tr("Needs attention")},
                 {tr("DETAILED LOGS"), on_off(prefs_.detailed_logging)}};
        break;
    case kFiles:
        about = tr("The folder that holds your keys, firmware and games.");
        lines = {{tr("IN USE"), short_path(folder, 34)}};
        if (!saved_folder.empty() && saved_folder != folder)
            lines.push_back({tr("NEXT START"), short_path(saved_folder, 34)});
        break;
    case kDownloads:
    {
        about = tr("Games on your network, downloaded when you play them.");
        int queued = 0;
        for (const Download &download : downloads_)
            queued += download.state != DownloadState::failed ? 1 : 0;
        if (!sources_.configured)
            lines.push_back({tr("SOURCES"), tr("Not set up")});
        // Each source with its state; three at most, the FTP server that writes the downloads and the
        // queue under them.
        for (std::size_t i = 0; i < sources_.list.size() && i < 3; ++i)
        {
            const SourceInfo &source = sources_.list[i];
            lines.push_back({source.name.c_str(), source.refreshing ? std::string{tr("Reading...")} :
                                                  source.online     ? fill(tr("{0} games"), {std::to_string(source.games)}) :
                                                                      std::string{tr("Offline")}});
        }
        lines.push_back({tr("FTP SERVER"), fill(tr("Port {0}"), {std::to_string(sources_.ftp_port)})});
        lines.push_back({tr("DOWNLOADS"), std::to_string(queued)});
        break;
    }
    case kSaveSync:
    {
        about = tr("Each profile's save data on a server of its own, synced before and after a game.");
        if (!sync_setup_.error.empty())
            lines.push_back({tr("SAVE-SYNC.JSON"), tr("Not readable")});
        for (std::size_t i = 0; i < sync_setup_.profiles.size() && i < 4; ++i)
        {
            const SaveSyncProfile &profile = sync_setup_.profiles[i];
            lines.push_back({profile.name.c_str(), profile.linked ? (profile.note.empty() ? tr("Linked") : tr("Needs attention")) :
                                                                    tr("Not linked")});
        }
        if (!sync_setup_.automatic)
            lines.push_back({tr("SYNC"), tr("Off")});
        break;
    }
    case kMultiplayer:
        about = tr("Join an Eden room, then choose Local Wireless in a supported game. Everyone needs a compatible game version.");
        lines = {{tr("CONNECTION"), tr("Room address and UDP port")},
                 {tr("TAILSCALE"), tr("Use the local UDP forward address")}};
        break;
    default:
        about = tr("The language games use when they offer it.");
        lines = {{tr("LANGUAGE"), pick(services_.language_labels(), prefs_.language)},
                 {tr("REGION"), services_.language_region(prefs_.language)}};
        break;
    }
    const float shown = tween::clamp01(section_.value);
    list.push_opacity(shown);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - shown) * 10.0f * motion());
    text(c, tr(kHeadings[settings_.selected]), 1016.0f, baseline(458.0f, 30.0f, theme::kSmall),
         theme::kSmall, theme::kLime, Align::left, 3.0f);
    text_shrink(c, about, 1016.0f, baseline(494.0f, 32.0f, 22.0f), 22.0f, theme::kCopy, 748.0f);
    // Five lines sit 62 apart; Video's six and Performance's seven move closer to stay inside
    // the panel.
    const float pitch = lines.size() > 6 ? 46.0f : lines.size() > 5 ? 52.0f : 62.0f;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const float top = 562.0f + pitch * static_cast<float>(i);
        const float value =
            text_shrink(c, lines[i].value, 1764.0f, baseline(top, 36.0f, theme::kText24),
                        theme::kText24, theme::kValue, 470.0f, Align::right);
        text_shrink(c, lines[i].label, 1016.0f, baseline(top, 36.0f, theme::kSmall), theme::kSmall,
                    theme::kLabel, 748.0f - value - 24.0f, Align::left, 2.0f);
        list.rounded_rect({1016.0f, top + 48.0f, 748.0f, 1.0f}, 0.0f, theme::kRule.with_alpha(0.45f));
    }
    list.pop_transform();
    list.pop_opacity();

    static constexpr Hint kHints[] = {
        {Pad::cross, TR("Select")}, {Pad::circle, TR("Back")}, {Pad::updown, TR("Browse settings")}};
    draw_footer(c, kHints, 3);
}

// ---------------------------------------------------------------- dialogs

int Launcher::dialog_rows(Modal modal) const
{
    switch (modal)
    {
    case Modal::video:
        return services_.frame_gen_state() == 0 ? static_cast<int>(video_frame_gen) : static_cast<int>(kVideoRows);
    case Modal::performance:
        return kPerformanceRows;
    case Modal::audio:
    case Modal::accessibility:
        return 3;
    case Modal::game:
        // Console mode, video, performance, audio, controls, language, mods; save data in builds
        // that move saves.
        // A game a download source has can be deleted from the console (and downloaded again).
        return (services_.save_transfer_available() ? 8 : 7) +
               (library_.selected < static_cast<int>(games_.size()) &&
                        !games_[static_cast<std::size_t>(library_.selected)].sources.empty() &&
                        !games_[static_cast<std::size_t>(library_.selected)].remote ? 1 : 0);
    case Modal::controls:     // vibration, the button mapping
    case Modal::diagnostics:  // detailed logging, logs written at once
        return 2;
    default:
        return 1;
    }
}

float Launcher::dialog_row_top(Modal modal, int row) const
{
    switch (modal)
    {
    case Modal::audio:
    case Modal::accessibility:
        return 370.0f + 102.0f * static_cast<float>(row);
    case Modal::performance: // four of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - performance_rows_.scroll();
    case Modal::video: // five of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - video_rows_.scroll();
    case Modal::game:
        return 334.0f + 96.0f * static_cast<float>(row);
    case Modal::controls: // under the shortcuts
        return 560.0f + 96.0f * static_cast<float>(row);
    case Modal::diagnostics: // under the setup's state
        return 500.0f + 102.0f * static_cast<float>(row);
    default:
        return 670.0f;
    }
}

void Launcher::press_dialog(Key key)
{
    const int rows = dialog_rows(modal_);
    const bool adjust = key == Key::left || key == Key::right;
    const bool activate = key == Key::cross;
    const int step = key == Key::left ? -1 : 1;
    if (key == Key::circle)
    {
        close_modal();
        return;
    }
    if ((key == Key::up || key == Key::down) && (modal_ == Modal::video || modal_ == Modal::performance))
    {
        ListView &rows_view = modal_ == Modal::video ? video_rows_ : performance_rows_;
        if (rows_view.move(key == Key::down ? 1 : -1))
        {
            option_ = rows_view.selected;
            message_.clear();
            cue(Cue::focus);
        }
        return;
    }
    if ((key == Key::up || key == Key::down) && rows > 1)
    {
        option_ = (option_ + (key == Key::down ? 1 : rows - 1)) % rows;
        message_.clear();
        cue(Cue::focus);
        return;
    }
    if (!adjust && !activate)
        return;

    const Preferences before = prefs_;
    Cue sound = Cue::toggle;
    switch (modal_)
    {
    case Modal::video:
        if (option_ == video_renderer)
            prefs_.renderer = prefs_.renderer != 0 ? 0 : 1;
        else if (option_ == video_output)
        {
            // The menu follows at once (the frontend opens its display again at this size).
            const int count = static_cast<int>(std::size(kOutputs));
            prefs_.output = (std::clamp(prefs_.output, 0, count - 1) + step + count) % count;
        }
        else if (option_ == video_resolution)
        {
            const int count = static_cast<int>(services_.resolution_labels().size());
            prefs_.resolution = (prefs_.resolution + step + count) % count;
        }
        else if (option_ == video_filter)
        {
            const int count = static_cast<int>(services_.filter_labels().size());
            prefs_.filter = (prefs_.filter + step + count) % count;
        }
        else if (option_ == video_refresh)
            prefs_.refresh = prefs_.refresh != 0 ? 0 : 1;
        else if (option_ >= video_frame_gen && services_.frame_gen_state() != 2)
        {
            say(tr("Frame generation needs Lossless.dll, from Lossless Scaling, in the lossless folder."), true);
            cue(Cue::error);
            return;
        }
        else if (option_ == video_frame_gen)
            prefs_.frame_gen = !prefs_.frame_gen;
        else if (option_ == video_frame_gen_target)
            prefs_.frame_gen_target =
                (std::clamp(prefs_.frame_gen_target, 0, kFrameGenTargets - 1) + step + kFrameGenTargets) % kFrameGenTargets;
        else if (option_ == video_frame_gen_multiplier)
            prefs_.frame_gen_multiplier = (std::clamp(prefs_.frame_gen_multiplier, 0, kFrameGenMultipliers - 1) + step +
                                           kFrameGenMultipliers) % kFrameGenMultipliers;
        else
            prefs_.hud = !prefs_.hud;
        break;
    case Modal::audio:
        if (option_ == 0)
        {
            if (!adjust)
                return;
            prefs_.volume = std::clamp(prefs_.volume + 10 * step, 0, 100);
            sound = Cue::slider;
        }
        else if (option_ == 1)
            prefs_.mute = !prefs_.mute;
        else
        {
            if (!adjust)
                return;
            prefs_.menu_volume = std::clamp(prefs_.menu_volume + 10 * step, 0, 100);
            sound = Cue::slider;
        }
        break;
    case Modal::performance:
    {
        bool *const switches[kPerformanceRows] = {
            &prefs_.block_list, &prefs_.async_shaders,     &prefs_.fast_gpu, &prefs_.unsafe_cpu,
            &prefs_.unsafe_dma, &prefs_.reactive_flushing, &prefs_.skip_invalidation};
        bool &value = *switches[std::clamp(option_, 0, kPerformanceRows - 1)];
        value = !value;
        break;
    }
    case Modal::controls:
        if (option_ == 1)
        {
            // The button mapping has its own list (game_options.cpp).
            if (activate)
                open_mapping(false);
            return;
        }
        prefs_.vibration = !prefs_.vibration;
        break;
    case Modal::accessibility:
        if (option_ == 0)
            prefs_.large_text = !prefs_.large_text;
        else if (option_ == 1)
            prefs_.high_contrast = !prefs_.high_contrast;
        else
            prefs_.reduce_motion = !prefs_.reduce_motion;
        break;
    case Modal::diagnostics:
        if (option_ == 0)
            prefs_.detailed_logging = !prefs_.detailed_logging;
        else
            prefs_.immediate_logs = !prefs_.immediate_logs;
        break;
    default:
        return;
    }
    // The launcher's look changes on the spot: that is its own confirmation.
    if (!save_preferences(modal_ == Modal::accessibility))
    {
        prefs_ = before;
        sound = Cue::error;
    }
    else if (modal_ == Modal::video && option_ == video_refresh && prefs_.refresh == 1)
    {
        // 120 Hz is a request: the display has the last word.
        say(tr("Saved. A display that cannot show 120 Hz stays at 60 Hz."));
    }
    apply_look();
    cue(sound);
}

void Launcher::draw_dialog(Canvas &c, Modal modal, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, 26.0f, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge.with_alpha(0.66f),
          1.6f);

    const char *title = "";
    const char *copy = "";
    switch (modal)
    {
    case Modal::video:
        title = tr("Video");
        copy = tr("How games are drawn and scaled to your TV.");
        break;
    case Modal::performance:
        title = tr("Performance");
        copy = tr("Faster games, at some cost in accuracy.");
        break;
    case Modal::audio:
        title = tr("Audio");
        copy = tr("Game audio; PS5 system-menu music is unchanged.");
        break;
    case Modal::controls:
        title = tr("Controls");
        copy = tr("Controller shortcuts and supported features.");
        break;
    case Modal::accessibility:
        title = tr("Accessibility");
        copy = tr("Make the menu easier to see and follow.");
        break;
    default:
        title = tr("Diagnostics");
        copy = tr("Detailed logs apply to the next game launch.");
        break;
    }
    text_shrink(c, title, 592.0f, baseline(218.0f, 62.0f, theme::kDisplay), theme::kDisplay,
                theme::kTitle, 736.0f);
    text_shrink(c, copy, 592.0f, baseline(291.0f, 32.0f, theme::kSmall), theme::kSmall,
                Color::rgb(0xbecbb9), 736.0f);

    const int rows = dialog_rows(modal);
    // Video's rows scroll in a window of five and Performance's in one of four; the other dialogs
    // show all of theirs.
    const bool scrolls = modal == Modal::video || modal == Modal::performance;
    const ListView &rows_view = modal == Modal::performance ? performance_rows_ : video_rows_;
    const Rect &window = modal == Modal::performance ? kPerformanceWindow : kVideoWindow;
    const int first = scrolls ? rows_view.first_row() : 0;
    const int last = scrolls ? rows_view.last_row() : rows - 1;
    if (scrolls)
        list.push_clip({window.x - 24.0f, window.y - 6.0f, window.w + 48.0f, window.h + 12.0f});
    for (int row = first; row <= last; ++row)
    {
        list.push_opacity(scrolls ? rows_view.row_alpha(row, kVideoRowHeight) : 1.0f);
        plate_rest(c, kRowPlate, {592.0f, dialog_row_top(modal, row), 736.0f, 94.0f});
        list.pop_opacity();
    }
    plate_focus(c, kRowPlate,
                {592.0f,
                 scrolls ? kVideoRowsTop + rows_view.cursor() - rows_view.scroll() :
                           option_cursor_.value,
                 736.0f, 94.0f},
                1.0f);

    // A row's name takes what its control (`taken` wide, at the right) leaves of the row.
    const auto label = [&](int row, const char *value, float taken)
    {
        text_shrink(c, value, 628.0f, baseline(dialog_row_top(modal, row), 94.0f, theme::kText24),
                    theme::kText24, theme::kValue, 664.0f - taken - 28.0f);
    };
    const auto choice = [&](int row, const std::string &value)
    {
        return chooser(c, value, 1296.0f,
                       baseline(dialog_row_top(modal, row), 94.0f, theme::kText24),
                       row == option_ ? 1.0f : 0.0f, theme::kLimePale);
    };
    constexpr float kToggle = 64.0f;
    const auto row_centre = [&](int row) { return dialog_row_top(modal, row) + 47.0f; };
    const float knob = tween::clamp01(switches_[0].value);

    switch (modal)
    {
    case Modal::video:
    {
        const std::string values[] = {
            prefs_.renderer != 0 ? tr("Vulkan (recommended)") : "OpenGL",
            output_name(prefs_.output),
            pick(services_.resolution_labels(), prefs_.resolution),
            pick(services_.filter_labels(), prefs_.filter),
            hertz(prefs_.refresh),
            std::string{}, // FPS overlay: a switch
            on_off(prefs_.frame_gen),
            prefs_.frame_gen_target <= 0 ? std::string{tr("Auto")} :
                fill(tr("{0} Hz"), {kFrameGenTargetNames[std::clamp(prefs_.frame_gen_target, 1, kFrameGenTargets - 1)]}),
            kFrameGenMultiplierNames[std::clamp(prefs_.frame_gen_multiplier, 0, kFrameGenMultipliers - 1)],
        };
        const bool frame_gen_locked = services_.frame_gen_state() != 2;
        static constexpr const char *kNames[kVideoRows] = {
            TR("Renderer"),         TR("Output resolution"), TR("Resolution"),
            TR("Upscaling filter"), TR("Refresh rate"),      TR("FPS overlay"),
            TR("Frame generation"), TR("Frame gen target"),  TR("Frame gen multiplier")};
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(video_rows_.row_alpha(row, kVideoRowHeight));
            if (row == video_overlay)
            {
                label(row, tr(kNames[row]), kToggle);
                toggle(c, 1292.0f, row_centre(row), knob);
            }
            else if (row >= video_frame_gen && frame_gen_locked)
            {
                // Greyed out: the user's Lossless.dll is not there.
                list.push_opacity(0.38f);
                label(row, tr(kNames[row]), choice(row, tr("Unavailable")));
                list.pop_opacity();
            }
            else
            {
                label(row, tr(kNames[row]), choice(row, values[row]));
            }
            list.pop_opacity();
        }
        break;
    }
    case Modal::audio:
    {
        // A level row: its value at the right, the bar ending 20 before it (or where "100%"
        // would leave it), and the name in what remains.
        const auto level_row = [&](int row, const char *name, const std::string &value, int level)
        {
            const float shown =
                text(c, value, 1292.0f, baseline(dialog_row_top(modal, row), 94.0f, theme::kText24),
                     theme::kText24, theme::kLimePale, Align::right);
            const float gap = std::max(96.0f, shown + 20.0f);
            level_bar(c, 1292.0f - gap, row_centre(row), 260.0f, static_cast<float>(level) / 100.0f,
                      option_ == row ? 1.0f : 0.0f);
            label(row, name, 260.0f + gap);
        };
        level_row(0, tr("Game volume"), percent(prefs_.volume), prefs_.volume);
        label(1, tr("Mute"), kToggle);
        toggle(c, 1292.0f, row_centre(1), knob);
        level_row(2, tr("Menu sounds"), prefs_.menu_volume > 0 ? percent(prefs_.menu_volume) : tr("Off"),
                  prefs_.menu_volume);
        break;
    }
    case Modal::controls:
    {
        // Each shortcut: its buttons as a key cap, then what it does.
        struct Shortcut
        {
            const char *key;
            const char *action;
        };
        static constexpr Shortcut kShortcuts[] = {
            {" + L1", TR("End the game and return to this menu")},
            {" + R1", TR("Show or hide the FPS overlay")}};
        for (int i = 0; i < 2; ++i)
        {
            const float top = 366.0f + 78.0f * static_cast<float>(i);
            list.bordered_rect({592.0f, top, 186.0f, 54.0f}, 12.0f, Color::rgb(0x15231d, 0.9f),
                               1.0f, theme::kRowEdge.with_alpha(0.6f));
            text_shrink(c, std::string(tr("Touchpad")) + kShortcuts[i].key, 685.0f,
                        baseline(top, 54.0f, theme::kSmall), theme::kSmall, theme::kLimePale, 170.0f,
                        Align::center);
            text_shrink(c, tr(kShortcuts[i].action), 802.0f, baseline(top, 54.0f, 22.0f), 22.0f,
                        theme::kBody, 526.0f);
        }
        label(0, tr("Vibration"), kToggle);
        toggle(c, 1292.0f, row_centre(0), knob);
        // The button mapping: as usual or changed, opened with Cross.
        const float shown = text_shrink(
            c, prefs_.mapping == kDefaultMapping ? tr("As usual") : tr("Changed"), 1292.0f,
            baseline(dialog_row_top(modal, 1), 94.0f, theme::kSmall), theme::kSmall,
            prefs_.mapping == kDefaultMapping ? theme::kMeta : theme::kLimePale, 320.0f, Align::right);
        label(1, tr("Button mapping"), shown);
        break;
    }
    case Modal::performance:
    {
        static constexpr const char *kNames[kPerformanceRows] = {
            TR("Compile ahead"),        TR("Asynchronous shaders"), TR("Faster GPU emulation"),
            TR("Faster CPU emulation"), TR("Faster DMA"),           TR("Reactive flushing"),
            TR("Skip CPU invalidation")};
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(rows_view.row_alpha(row, kVideoRowHeight));
            // Asynchronous shaders act with Vulkan only: the OpenGL renderer compiles in its own
            // context here (graphics.cpp), and Eden then leaves them off.
            const std::string name =
                std::string(tr(kNames[row])) + (row == speed_async_shaders ? " (Vulkan)" : "");
            label(row, name.c_str(), kToggle);
            toggle(c, 1292.0f, row_centre(row),
                   tween::clamp01(switches_[static_cast<std::size_t>(row)].value));
            list.pop_opacity();
        }
        break;
    }
    case Modal::accessibility:
    {
        static constexpr const char *kNames[] = {TR("Larger text"), TR("High contrast"),
                                                 TR("Reduce motion")};
        static constexpr const char *kAbout[] = {
            TR("Draws the menu's small text larger."),
            TR("Solid panels, brighter text and an outlined highlight."),
            TR("Stops the background drifting and the screens sliding, here and on the loading "
               "screen.")};
        for (int row = 0; row < 3; ++row)
        {
            label(row, tr(kNames[row]), kToggle);
            toggle(c, 1292.0f, row_centre(row),
                   tween::clamp01(switches_[static_cast<std::size_t>(row)].value));
        }
        // What the highlighted switch does.
        text_block(c, tr(kAbout[std::clamp(option_, 0, 2)]), 592.0f,
                   baseline(700.0f, 30.0f, theme::kSmall), theme::kSmall, 30.0f, theme::kMeta, 736.0f,
                   2, kShrink);
        break;
    }
    default:
    {
        text_block(c, services_.setup_details(), 592.0f, baseline(364.0f, 40.0f, theme::kText24),
                   theme::kText24, 40.0f, theme::kBody, 736.0f, 3, kShrink);
        static constexpr const char *kNames[] = {TR("Detailed logging"), TR("Write logs at once")};
        static constexpr const char *kAbout[] = {
            TR("Eden's debug messages in the logs of a game."),
            TR("Keeps the last line before a crash, but games stutter. From the next start.")};
        for (int row = 0; row < 2; ++row)
        {
            label(row, tr(kNames[row]), kToggle);
            toggle(c, 1292.0f, row_centre(row),
                   tween::clamp01(switches_[static_cast<std::size_t>(row)].value));
        }
        // What the highlighted switch does.
        text_block(c, tr(kAbout[std::clamp(option_, 0, 1)]), 592.0f,
                   baseline(716.0f, 30.0f, theme::kSmall), theme::kSmall, 30.0f, theme::kMeta, 736.0f,
                   2, kShrink);
        break;
    }
    }

    if (scrolls)
    {
        list.pop_clip();
        scrollbar(c, rows_view, 1340.0f, window.y, window.h);
    }
    if (modal == Modal::performance)
    {
        // What the highlighted switch does, under the window.
        static constexpr const char *kAbout[kPerformanceRows] = {
            TR("Compiles the code a game used before as it starts, so new areas stutter less."),
            TR("Draws an effect once its shader is ready instead of pausing. Things can be missing "
               "for a moment."),
            TR("Lowest GPU accuracy: faster in demanding games, and graphics can be wrong."),
            TR("Less exact floating-point math: faster, and a few games misbehave."),
            TR("Less exact memory transfers to the GPU: faster, and a few games show wrong graphics."),
            TR("Keeps what a game reads back from the GPU exact. Off is faster, and some effects "
               "break."),
            TR("Skips some checks when a game changes memory the GPU uses: faster, and textures "
               "can be stale.")};
        text_block(c, tr(kAbout[std::clamp(option_, 0, kPerformanceRows - 1)]), 592.0f,
                   baseline(window.y + window.h + 22.0f, 30.0f, theme::kSmall), theme::kSmall, 30.0f,
                   theme::kMeta, 736.0f, 2, kShrink);
    }

    // Under the rows: Video's five, and Performance's four with their line of text, end lower
    // than the other dialogs' three.
    const float foot = scrolls ? 848.0f : 811.0f;
    if (!message_.empty())
    {
        notice(c, message_, 592.0f, foot + 7.0f, theme::kSmall,
               message_warning_ ? theme::kWarning : theme::kLimePale, 736.0f, message_warning_);
    }
    else if (modal == Modal::controls && option_ == 1)
    {
        static constexpr Hint kOpen[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kOpen, 3, 592.0f, foot, theme::kCopy, 736.0f);
    }
    else if (rows > 1)
    {
        static constexpr Hint kHints[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kHints, 3, 592.0f, foot, theme::kCopy, 736.0f);
    }
    else
    {
        static constexpr Hint kHints[] = {{Pad::cross, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kHints, 2, 592.0f, foot, theme::kCopy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
