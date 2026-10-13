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

constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
// The categories: a tile each, four to a row; under them, what the focused one is about.
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
    kLanguage,
    kTheme,
    kCategoryCount,
};
constexpr const char *kCategories[kCategoryCount] = {
    TR("Profiles"), TR("Video"), TR("Performance"), TR("Audio"), TR("Controls"), TR("Accessibility"),
    TR("Diagnostics"), TR("Game files"), TR("Downloads"), TR("Save sync"), TR("Language"), TR("Theme")};
constexpr Icon kIcons[kCategoryCount] = {
    Icon::profile, Icon::video, Icon::performance, Icon::audio, Icon::controls, Icon::accessibility,
    Icon::diagnostics, Icon::files, Icon::downloads, Icon::sync, Icon::language, Icon::theme};
constexpr int kTileColumns = 4;
constexpr int kTileRows = 3;
constexpr float kTileGap = 24.0f;
constexpr float kTileWidth = (layout::kWidth - kTileGap * (kTileColumns - 1)) / kTileColumns;
constexpr float kTileHeight = 224.0f;
constexpr float kTilesTop = 152.0f;
constexpr Rect kStrip{layout::kLeft, kTilesTop + (kTileHeight + kTileGap) * kTileRows, layout::kWidth, 68.0f};
Rect tile_rect(int index)
{
    return {layout::kLeft + (kTileWidth + kTileGap) * static_cast<float>(index % kTileColumns),
            kTilesTop + (kTileHeight + kTileGap) * static_cast<float>(index / kTileColumns), kTileWidth, kTileHeight};
}

// The Video dialog's rows, and the window that shows five of them (placed as a game's settings
// are).
enum VideoRow : int
{
    video_renderer,
    video_output,
    video_resolution,
    video_filter,
    video_astc,
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
    int &at = settings_.selected;
    const int before = at;
    switch (key)
    {
    case Key::circle:
        go_section(0);
        return;
    case Key::up:
        if (at >= kTileColumns)
            at -= kTileColumns;
        break;
    case Key::down:
        if (at < kTileColumns * (kTileRows - 1))
            at += kTileColumns;
        break;
    case Key::left:
        if (at % kTileColumns == 0)
            return focus_rail();
        at -= 1;
        break;
    case Key::right:
        if (at % kTileColumns != kTileColumns - 1)
            at += 1;
        break;
    case Key::cross:
        press_ = 1.0f;
        switch (at)
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
        case kTheme:
            open_modal(Modal::theme);
            option_ = theme_index();
            option_cursor_.snap(dialog_row_top(Modal::theme, option_));
            break;
        default:
            open_modal(Modal::diagnostics);
            break;
        }
        return;
    default:
        return;
    }
    if (at != before)
    {
        // What the tile is about comes in again under the tiles.
        section_.value = 0.0f;
        section_.velocity = 0.0f;
        message_.clear();
        cue(Cue::focus);
    }
}

void Launcher::draw_settings(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    draw_frame(c, tr("Settings"), tr("Fine-tune your experience"));

    // What each category is set to, in a few words.
    const int switches = prefs_.block_list + prefs_.async_shaders + prefs_.fast_gpu + prefs_.unsafe_cpu +
                         prefs_.unsafe_dma + prefs_.reactive_flushing + prefs_.skip_invalidation;
    const std::string summaries[kCategoryCount] = {
        playing_,
        pick(services_.resolution_keys(), prefs_.resolution) + "  /  " + pick(services_.filter_labels(), prefs_.filter) +
            "  /  " + output_name(prefs_.output) + "  /  " + hertz(prefs_.refresh),
        fill(tr("{0} of {1} on"), {std::to_string(switches), std::to_string(static_cast<int>(kPerformanceRows))}),
        prefs_.mute ? tr("Muted") : percent(prefs_.volume),
        prefs_.vibration ? tr("Vibration on") : tr("Vibration off"),
        prefs_.large_text || prefs_.high_contrast || prefs_.reduce_motion ? tr("On") : tr("Off"),
        prefs_.detailed_logging ? tr("Detailed logs on") : home_.setup_ready ? tr("Ready") : tr("Needs attention"),
        short_path(services_.files_folder(), 28),
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
        pick(services_.language_labels(), prefs_.language),
        t.name,
    };
    for (int i = 0; i < kCategoryCount; ++i)
    {
        const Rect r = tile_rect(i);
        const float f = tween::clamp01(settings_springs_[static_cast<std::size_t>(i)].value);
        const float scale = 1.0f + (0.03f * f - 0.035f * press_ * f) * motion();
        const float here = arrive(i);
        list.push_opacity(here);
        list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f,
                            -3.0f * f * motion() + (1.0f - here) * 18.0f);
        plate(c, tile_plate(), r, f);
        icon(c, kIcons[i], r.x + 50.0f, r.y + 52.0f, 44.0f, gfx::mix(t.accent, t.accent_pale, f));
        {
            // A name of two long words takes two lines: the icon over it leaves the room.
            const Heavy heavy(c);
            const char *name = tr(kCategories[i]);
            const float width = r.w - 56.0f;
            const float line = baseline(r.y + r.h - 98.0f, 38.0f, 30.0f);
            // (Two words: one long word is drawn smaller rather than broken.)
            if (text_width(c, name, 30.0f) > width && std::string_view{name}.find(' ') != std::string_view::npos &&
                fonts_.font->wrap(name, text_size(30.0f), width).size() == 2)
                text_block(c, name, r.x + 28.0f, line - 36.0f, 30.0f, 36.0f, t.title, width, 2, kShrink);
            else
                text_shrink(c, name, r.x + 28.0f, line, 30.0f, t.title, width);
        }
        text_shrink(c, summaries[i], r.x + 28.0f, baseline(r.y + r.h - 56.0f, 28.0f, t.size_small), t.size_small,
                    gfx::mix(t.meta, t.body, f), r.w - 56.0f);
        list.pop_transform();
        list.pop_opacity();
    }

    // ---- under the tiles: what the focused one is about ----
    glass(c, kStrip, t.radius_row + 2.0f, t.panel.with_alpha(0.80f), t.panel_edge.with_alpha(0.34f), 0.5f);
    const char *about = "";
    switch (settings_.selected)
    {
    case kProfiles:
        about = tr("Who is playing. Each profile keeps its own save data and settings.");
        break;
    case kVideo:
        about = tr("Graphics backend and how games are scaled to your TV.");
        break;
    case kPerformance:
        about = tr("Faster games, at some cost in accuracy.");
        break;
    case kAudio:
        about = tr("Game volume, and the sounds of this menu.");
        break;
    case kControls:
        about = tr("Vibration, button mapping and the shortcuts during a game.");
        break;
    case kAccessibility:
        about = tr("Make the menu easier to see and follow.");
        break;
    case kDiagnostics:
        about = tr("Setup status and detailed logs.");
        break;
    case kFiles:
        about = tr("The folder that holds your keys, firmware and games.");
        break;
    case kDownloads:
        about = tr("Games on your network, downloaded when you play them.");
        break;
    case kSaveSync:
        about = tr("Each profile's save data on a server of its own, synced before and after a game.");
        break;
    case kLanguage:
        about = tr("The language games use when they offer it.");
        break;
    case kTheme:
        about = tr(t.summary);
        break;
    default:
        break;
    }
    const float version = text(c, version_, kStrip.x + kStrip.w - 28.0f, baseline(kStrip.y, kStrip.h, t.size_small),
                               t.size_small, t.faint, Align::right);
    const float shown = tween::clamp01(section_.value);
    list.push_opacity(shown);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - shown) * 8.0f * motion());
    list.circle(kStrip.x + 34.0f, kStrip.y + kStrip.h * 0.5f, 5.0f, t.accent);
    text_shrink(c, about, kStrip.x + 56.0f, baseline(kStrip.y, kStrip.h, 22.0f), 22.0f, t.copy,
                kStrip.w - 28.0f - version - 40.0f - 56.0f);
    list.pop_transform();
    list.pop_opacity();

    static constexpr Hint kHints[] = {
        {Pad::cross, TR("Select")}, {Pad::circle, TR("Back")}, {Pad::l1, TR("Sections"), Pad::r1}};
    draw_footer(c, kHints, 3, home_.system_status);
}

const char *Launcher::performance_about(int row)
{
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
    return kAbout[std::clamp(row, 0, kPerformanceRows - 1)];
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
    case Modal::theme:
        return static_cast<int>(themes().size());
    case Modal::game:
        // Console mode, video, performance, audio, controls, system, language, mods; save data in
        // builds that move saves.
        // A game a download source has can be deleted from the console (and downloaded again).
        return (services_.save_transfer_available() ? 9 : 8) +
               (library_.selected < static_cast<int>(games_.size()) &&
                        !games_[static_cast<std::size_t>(library_.selected)].sources.empty() &&
                        !games_[static_cast<std::size_t>(library_.selected)].remote ? 1 : 0);
    case Modal::controls:     // vibration, its strength, the button mapping
        return 3;
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
    case Modal::theme:
        return 370.0f + 102.0f * static_cast<float>(row);
    case Modal::performance: // four of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - performance_rows_.scroll();
    case Modal::video: // five of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - video_rows_.scroll();
    case Modal::game:
        return 334.0f + 96.0f * static_cast<float>(row);
    case Modal::controls: // under the shortcuts
        return 528.0f + 96.0f * static_cast<float>(row);
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
    if (modal_ == Modal::theme)
    {
        // The look changes on the spot, and is kept for the next start.
        if (!activate)
            return;
        const Theme &chosen = themes()[static_cast<std::size_t>(std::clamp(option_, 0, rows - 1))];
        const std::string had = prefs_.theme;
        prefs_.theme = chosen.id;
        const bool saved = save_preferences(true);
        if (saved)
            use_theme(chosen.id);
        else
            prefs_.theme = had;
        cue(saved ? Cue::saved : Cue::error);
        return;
    }

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
        else if (option_ == video_astc)
            prefs_.astc = prefs_.astc != 0 ? 0 : 1;
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
        if (option_ == 2)
        {
            // The button mapping has its own list (game_options.cpp).
            if (activate)
                open_mapping(false);
            return;
        }
        if (option_ == 1)
        {
            // The strength: a quarter at a time, never off (the switch above is for that).
            if (!adjust)
                return;
            prefs_.vibration_strength = std::clamp(prefs_.vibration_strength + 25 * step, 25, 100);
            sound = Cue::slider;
            break;
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
    else if (modal_ == Modal::video && option_ == video_astc && prefs_.astc == 1)
    {
        // Held decoded, these textures take four times the memory they take compressed.
        say(tr("Saved. Full quality uses four times the graphics memory for these textures and can run out "
               "above 1x."));
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
    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f),
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
    case Modal::theme:
        title = tr("Theme");
        copy = tr("The look of this menu.");
        break;
    default:
        title = tr("Diagnostics");
        copy = tr("Detailed logs apply to the next game launch.");
        break;
    }
    text_shrink(c, title, 592.0f, baseline(218.0f, 62.0f, th().size_display), th().size_display,
                th().title, 736.0f);
    text_shrink(c, copy, 592.0f, baseline(291.0f, 32.0f, th().size_small), th().size_small,
                th().copy, 736.0f);

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
        plate_rest(c, row_plate(), {592.0f, dialog_row_top(modal, row), 736.0f, 94.0f});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(),
                {592.0f,
                 scrolls ? kVideoRowsTop + rows_view.cursor() - rows_view.scroll() :
                           option_cursor_.value,
                 736.0f, 94.0f},
                1.0f);

    // A row's name takes what its control (`taken` wide, at the right) leaves of the row.
    const auto label = [&](int row, const char *value, float taken)
    {
        text_shrink(c, value, 628.0f, baseline(dialog_row_top(modal, row), 94.0f, th().size_text),
                    th().size_text, th().value, 664.0f - taken - 28.0f);
    };
    const auto choice = [&](int row, const std::string &value)
    {
        return chooser(c, value, 1296.0f,
                       baseline(dialog_row_top(modal, row), 94.0f, th().size_text),
                       row == option_ ? 1.0f : 0.0f, th().accent_pale);
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
            pick(services_.astc_labels(), prefs_.astc),
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
            TR("Upscaling filter"), TR("ASTC textures"),     TR("Refresh rate"),
            TR("FPS overlay"),      TR("Frame generation"),  TR("Frame gen target"),
            TR("Frame gen multiplier")};
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
                text(c, value, 1292.0f, baseline(dialog_row_top(modal, row), 94.0f, th().size_text),
                     th().size_text, th().accent_pale, Align::right);
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
        // Each shortcut: the two buttons held together, drawn as the controller has them, then
        // what they do.
        struct Shortcut
        {
            Pad with;
            const char *action;
        };
        static constexpr Shortcut kShortcuts[] = {
            {Pad::l1, TR("End the game and return to this menu")},
            {Pad::r1, TR("Show or hide the FPS overlay")}};
        for (int i = 0; i < 2; ++i)
        {
            const float top = 366.0f + 78.0f * static_cast<float>(i);
            const Rect cap{592.0f, top, 186.0f, 54.0f};
            list.bordered_rect(cap, 14.0f, th().row.with_alpha(0.9f), 1.0f, th().row_edge.with_alpha(0.6f));
            constexpr float kButton = 38.0f;
            constexpr float kPlus = 28.0f;
            const float cy = cap.y + cap.h * 0.5f;
            float x = cap.x + (cap.w - pad_width(Pad::touchpad, kButton) - kPlus - pad_width(kShortcuts[i].with, kButton)) * 0.5f;
            draw_pad(c, Pad::touchpad, x, cy, kButton);
            x += pad_width(Pad::touchpad, kButton);
            list.line(x + kPlus * 0.5f - 5.0f, cy, x + kPlus * 0.5f + 5.0f, cy, 2.0f, th().label);
            list.line(x + kPlus * 0.5f, cy - 5.0f, x + kPlus * 0.5f, cy + 5.0f, 2.0f, th().label);
            draw_pad(c, kShortcuts[i].with, x + kPlus, cy, kButton);
            text_shrink(c, tr(kShortcuts[i].action), 802.0f, baseline(top, 54.0f, 22.0f), 22.0f,
                        th().body, 526.0f);
        }
        label(0, tr("Vibration"), kToggle);
        toggle(c, 1292.0f, row_centre(0), knob);
        // The button mapping: as usual or changed, opened with Cross.
        const float shown = text_shrink(
            c, prefs_.mapping == kDefaultMapping ? tr("As usual") : tr("Changed"), 1292.0f,
            baseline(dialog_row_top(modal, 2), 94.0f, th().size_small), th().size_small,
            prefs_.mapping == kDefaultMapping ? th().meta : th().accent_pale, 320.0f, Align::right);
        label(2, tr("Button mapping"), shown);
        // The vibration's strength: a level row, as the volumes are.
        {
            const float value =
                text(c, percent(prefs_.vibration_strength), 1292.0f,
                     baseline(dialog_row_top(modal, 1), 94.0f, th().size_text), th().size_text, th().accent_pale,
                     Align::right);
            const float gap = std::max(96.0f, value + 20.0f);
            level_bar(c, 1292.0f - gap, row_centre(1), 260.0f,
                      static_cast<float>(prefs_.vibration_strength) / 100.0f, option_ == 1 ? 1.0f : 0.0f);
            label(1, tr("Vibration strength"), 260.0f + gap);
        }
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
                   baseline(700.0f, 30.0f, th().size_small), th().size_small, 30.0f, th().meta, 736.0f,
                   2, kShrink);
        break;
    }
    case Modal::theme:
    {
        // Every theme this build has; the one in use carries the mark.
        const std::span<const Theme> all = themes();
        for (int row = 0; row < rows; ++row)
        {
            label(row, all[static_cast<std::size_t>(row)].name, 64.0f);
            if (row != theme_index())
                continue;
            list.circle(1268.0f, row_centre(row), 17.0f, th().accent);
            const bool contrast = look().high_contrast;
            look().high_contrast = false;
            icon(c, Icon::check, 1268.0f, row_centre(row), 22.0f, th().on_accent);
            look().high_contrast = contrast;
        }
        text_block(c, tr(all[static_cast<std::size_t>(std::clamp(option_, 0, rows - 1))].summary), 592.0f,
                   baseline(700.0f, 30.0f, th().size_small), th().size_small, 30.0f, th().meta, 736.0f, 2,
                   kShrink);
        break;
    }
    default:
    {
        text_block(c, services_.setup_details(), 592.0f, baseline(364.0f, 40.0f, th().size_text),
                   th().size_text, 40.0f, th().body, 736.0f, 3, kShrink);
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
                   baseline(716.0f, 30.0f, th().size_small), th().size_small, 30.0f, th().meta, 736.0f,
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
        text_block(c, tr(performance_about(option_)), 592.0f,
                   baseline(window.y + window.h + 22.0f, 30.0f, th().size_small), th().size_small, 30.0f,
                   th().meta, 736.0f, 2, kShrink);
    }

    // Under the rows: Video's five, Performance's four with their line of text, and Controls'
    // three under its shortcuts end lower than the other dialogs' three.
    const float foot = scrolls || modal == Modal::controls ? 848.0f : 811.0f;
    if (!message_.empty() && scrolls)
    {
        // Two lines, placed as a game's own settings place theirs: what a Video choice costs
        // takes more than one.
        notice_block(c, message_, 592.0f, foot - 6.0f, th().size_small, 26.0f,
                     message_warning_ ? th().warning : th().accent_pale, 736.0f, 2, message_warning_);
    }
    else if (!message_.empty())
    {
        notice(c, message_, 592.0f, foot + 7.0f, th().size_small,
               message_warning_ ? th().warning : th().accent_pale, 736.0f, message_warning_);
    }
    else if (modal == Modal::controls && option_ == 2)
    {
        static constexpr Hint kOpen[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kOpen, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else if (rows > 1)
    {
        static constexpr Hint kHints[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kHints, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else
    {
        static constexpr Hint kHints[] = {{Pad::cross, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kHints, 2, 592.0f, foot, th().copy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
