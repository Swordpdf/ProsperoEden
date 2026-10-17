// ProsperoEden - Launcher settings: the category list and its dialogs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cctype>
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
    kAppearance,
    kColour,
    kCategoryCount,
};
constexpr const char *kCategories[kCategoryCount] = {
    TR("Profiles"), TR("Video"), TR("Performance"), TR("Audio"), TR("Controls"), TR("Accessibility"),
    TR("Diagnostics"), TR("Game files"), TR("Downloads"), TR("Save sync"), TR("Language"), TR("Appearance"),
    TR("Colour")};
constexpr Icon kIcons[kCategoryCount] = {
    Icon::profile, Icon::video, Icon::performance, Icon::audio, Icon::controls, Icon::accessibility,
    Icon::diagnostics, Icon::files, Icon::downloads, Icon::sync, Icon::language, Icon::theme,
    Icon::theme};
constexpr int kTileColumns = 4;
constexpr int kTileRows = 4;
constexpr float kTileGap = 20.0f;
constexpr float kTileWidth = (layout::kWidth - kTileGap * (kTileColumns - 1)) / kTileColumns;
constexpr float kTileHeight = 182.0f;
constexpr float kTilesTop = 140.0f;
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
    video_overlay,         // the FPS overlay's own switch
    video_overlay_options, // opens the overlay's options (Modal::hud)
    video_post_processing, // one of Eden's ReShade FX presets, or off (Vulkan only)
    // Frame generation: not shown in a build without it (Services::frame_gen_state).
    video_frame_gen,
    video_frame_gen_method,
    video_frame_gen_target,
    video_frame_gen_multiplier,
    kVideoRows,
};
// The overlay's options (Modal::hud): what its line says, and how the chip is drawn. Opened from
// Settings > Video > Overlay options; the colour and size switches live here, and the field
// switches are the pieces of the line the user may turn off (hud.h's kHudField*).
enum HudRow : int
{
    hud_colors_row,  // Enable colors
    hud_size_row,    // Size
    hud_corner_row,  // Corner
    hud_backend_row, // from here, one switch per piece of the line
    hud_fps_row,
    hud_ms_row,
    hud_speed_row,
    hud_cpu_row,
    hud_gpu_row,
    hud_mem_row,
    hud_ping_row,
    hud_cores_row, // the CPU piece as each of the four cores (its bit clear), or only the busiest
    hud_resolution_row, // the game's own picture, as it renders it (bit 9; last, so no earlier bit moves)
    kHudRows,
};
constexpr int kFrameGenMethods = 2;     // Lossless Scaling, MihawkGen (settings_store.h)
constexpr int kFrameGenTargets = 6;     // Auto, 60, 90, 120, 144, 240 Hz (settings_store.h)
constexpr int kFrameGenMultipliers = 3; // 2x, 3x, 4x
constexpr int kHudSizes = 3;            // Regular, Medium, Large (settings_store.h, kHudSizeKeys)
constexpr int kHudCorners = 4;          // kHudCornerNames, services.hpp
constexpr int kHudRowsShown = 5;
constexpr float kVideoRowsTop = 334.0f;
constexpr float kVideoRowPitch = 96.0f;
constexpr float kVideoRowHeight = 94.0f;
constexpr int kVideoRowsShown = 5;
constexpr Rect kVideoWindow{592.0f, kVideoRowsTop, 736.0f,
                            kVideoRowPitch * (kVideoRowsShown - 1) + kVideoRowHeight};
// The overlay's options (Modal::hud) show five of their rows and scroll, as Video's do.
constexpr Rect kHudWindow{592.0f, kVideoRowsTop, 736.0f,
                          kVideoRowPitch * (kHudRowsShown - 1) + kVideoRowHeight};
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
    speed_compact_memory,
    speed_memory_layout, // the memory a game's console is told it has; last, so no row moves
    kPerformanceRows,
};
constexpr int kPerformanceSwitches = speed_memory_layout;
constexpr int kPerformanceRowsShown = 4;
constexpr Rect kPerformanceWindow{592.0f, kVideoRowsTop, 736.0f,
                                  kVideoRowPitch * (kPerformanceRowsShown - 1) + kVideoRowHeight};
// The Controls dialog's rows, under its shortcuts: three of the four show and the list scrolls.
constexpr float kControlsRowsTop = 510.0f;
constexpr int kControlsRowsShown = 3;
constexpr Rect kControlsWindow{592.0f, kControlsRowsTop, 736.0f,
                               kVideoRowPitch * (kControlsRowsShown - 1) + kVideoRowHeight};
// The Colour dialog's rows (Default and the theme's presets): shorter than the other dialogs'
// rows, six of the nine show and the list scrolls.
constexpr float kAccentRowPitch = 80.0f;
constexpr float kAccentRowHeight = 72.0f;
constexpr int kAccentRowsShown = 6;
constexpr Rect kAccentWindow{592.0f, kVideoRowsTop, 736.0f,
                             kAccentRowPitch * (kAccentRowsShown - 1) + kAccentRowHeight};
// The Controls rows, in order.
enum ControlRow : int
{
    kControlVibration,
    kControlStrength,
    kControlMapping,
    kControlMenuKeys,
    kControlRows,
};
// The sizes of Preferences::output, as every language writes them.
constexpr const char *kOutputs[] = {"1080p", "1440p", "2160p"};
// Settings > Performance > Memory: what the guest's kernel is told the console has
// (settings_store.h's kMemoryLayoutKeys). A size and its unit, so nothing here to translate.
constexpr const char *kMemoryLayouts[] = {"4 GiB", "6 GiB", "8 GiB"};

constexpr const char *kFrameGenMethodNames[] = {"Lossless Scaling", "MihawkGen"};
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

// The Wallpaper dialog's rows: "None" and the folder's images, which scroll with the folder's
// name under them (docs/WALLPAPERS.md).
constexpr int kWallpaperRowsShown = 4;
constexpr Rect kWallpaperWindow{592.0f, kVideoRowsTop, 736.0f,
                                kVideoRowPitch * (kWallpaperRowsShown - 1) + kVideoRowHeight};

// A file the Wallpaper dialog lists: an image pe/gfx/image.cpp reads, with a name
// settings_store.h's ValidWallpaperName would save.
bool WallpaperImage(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size() || name.size() > 128)
        return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\')
            return false;
    std::string extension = name.substr(dot);
    for (char &c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
           extension == ".gif" || extension == ".tga" || extension == ".dds";
}

// The row what is in use sits on: "None" (0) or the folder's image (1+).
int WallpaperRow(const std::string &chosen, const std::vector<std::string> &files)
{
    if (chosen.empty())
        return 0;
    for (std::size_t index = 0; index < files.size(); ++index)
        if (files[index] == chosen)
            return 1 + static_cast<int>(index);
    return 0;
}

} // namespace

void Launcher::press_settings(Key key)
{
    int &at = settings_.selected;
    const int before = at;
    switch (key)
    {
    case Key::options:
        launcher_picker_ = true;
        return;
    case Key::circle:
        go_section(0);
        return;
    case Key::up:
        if (th().list_settings)
            settings_.move(-1);
        else if (at >= kTileColumns)
            at -= kTileColumns;
        break;
    case Key::down:
        if (th().list_settings)
            settings_.move(1);
        else if (at < kTileColumns * (kTileRows - 1))
            at += kTileColumns;
        break;
    case Key::left:
        if (th().list_settings || at % kTileColumns == 0)
            return focus_rail();
        at -= 1;
        break;
    case Key::right:
        if (!th().list_settings && at % kTileColumns != kTileColumns - 1)
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
            controls_rows_.visible = kControlsRowsShown;
            controls_rows_.pitch = kVideoRowPitch;
            controls_rows_.reset(dialog_rows(Modal::controls), 0);
            break;
        case kAccessibility:
            open_modal(Modal::accessibility);
            break;
        case kAppearance:
            open_modal(Modal::appearance);
            break;
        case kColour:
            open_modal(Modal::accent);
            option_ = accent_index() + 1;
            accent_rows_.visible = kAccentRowsShown;
            accent_rows_.pitch = kAccentRowPitch;
            accent_rows_.reset(dialog_rows(Modal::accent), option_);
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
                         prefs_.unsafe_dma + prefs_.reactive_flushing + prefs_.skip_invalidation +
                         prefs_.compact_memory;
    const std::string summaries[kCategoryCount] = {
        playing_,
        pick(services_.resolution_keys(), prefs_.resolution) + "  /  " + pick(services_.filter_labels(), prefs_.filter) +
            "  /  " + output_name(prefs_.output) + "  /  " + hertz(prefs_.refresh),
        fill(tr("{0} of {1} on"), {std::to_string(switches), std::to_string(kPerformanceSwitches)}),
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
        accent_index() < 0 ? std::string{tr("Default")} :
            std::string{accent_presets()[static_cast<std::size_t>(accent_index())].name},
    };
    if (t.list_settings)
    {
        constexpr float kListRowHeight = 78.0f;
        const float kListLeft = t.top_nav ? 960.0f : layout::kLeft;
        const float kListWidth = t.top_nav ? 864.0f : layout::kWidth;
        const float kListTop = t.top_nav ? 110.0f : 152.0f;
        const float kListWindowHeight = t.top_nav ? 870.0f : 840.0f;
        if (t.top_nav)
            kicker(c, tr("SETTINGS"), kListLeft, baseline(80.0f, 30.0f, t.size_small), t.accent_pale, kListWidth);
        const auto list_row_rect = [&](int row) -> Rect
        {
            return {kListLeft, kListTop + static_cast<float>(row) * settings_.pitch - settings_.scroll(),
                    kListWidth, kListRowHeight};
        };
        list.push_clip({kListLeft - 24.0f, kListTop - 6.0f, kListWidth + 48.0f, kListWindowHeight + 12.0f});
        for (int row = settings_.first_row(); row <= settings_.last_row(); ++row)
        {
            list.push_opacity(settings_.row_alpha(row, kListRowHeight));
            plate_rest(c, row_plate(), list_row_rect(row));
            list.pop_opacity();
        }
        plate_focus(c, row_plate(),
                    {kListLeft, kListTop + settings_.cursor() - settings_.scroll(), kListWidth, kListRowHeight},
                    1.0f);
        for (int row = settings_.first_row(); row <= settings_.last_row(); ++row)
        {
            const Rect r = list_row_rect(row);
            list.push_opacity(settings_.row_alpha(row, kListRowHeight));
            icon(c, kIcons[row], r.x + 48.0f, r.y + r.h * 0.5f, 36.0f,
                 gfx::mix(t.accent, t.accent_pale,
                           tween::clamp01(settings_springs_[static_cast<std::size_t>(row)].value)));
            {
                const Heavy heavy(c);
                text_shrink(c, tr(kCategories[row]), r.x + 88.0f,
                            baseline(r.y, r.h, t.size_text), t.size_text, t.title, 420.0f);
            }
            text_shrink(c, summaries[row], r.x + r.w - 28.0f,
                        baseline(r.y, r.h, t.size_small), t.size_small,
                        gfx::mix(t.meta, t.body,
                                  tween::clamp01(settings_springs_[static_cast<std::size_t>(row)].value)),
                        r.w - 560.0f, Align::right);
            list.pop_opacity();
        }
        list.pop_clip();
        if (settings_.count > settings_.visible)
            scrollbar(c, settings_, kListLeft + kListWidth + 16.0f, kListTop, kListWindowHeight);
    }
    else
    {
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
                const Heavy heavy(c);
                const char *name = tr(kCategories[i]);
                const float width = r.w - 56.0f;
                const float line = baseline(r.y + r.h - 98.0f, 38.0f, 30.0f);
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
        if (!t.top_nav)
        {
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
        case kAppearance:
            about = tr("Choose a theme or change the launcher layout.");
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
        }
    }

    if (t.top_nav)
    {
        static constexpr Hint kHints[] = {
            {Pad::cross, TR("Select")}, {Pad::options, TR("Launcher")}, {Pad::circle, TR("Home")}, {Pad::l1, TR("Sections"), Pad::r1}};
        draw_footer(c, kHints, 4);
        return;
    }
    static constexpr Hint kHints[] = {
        {Pad::cross, TR("Select")}, {Pad::options, TR("Launcher")}, {Pad::circle, TR("Back")}, {Pad::l1, TR("Sections"), Pad::r1}};
    draw_footer(c, kHints, 4, home_.system_status);
}

const char *Launcher::performance_about(int row)
{
    static constexpr const char *kAbout[kPerformanceSwitches] = {
        TR("Compiles the code a game used before as it starts, so new areas stutter less."),
        TR("Draws an effect once its shader is ready instead of pausing. Things can be missing "
           "for a moment."),
        TR("Lowest GPU accuracy: faster in demanding games, and graphics can be wrong."),
        TR("Less exact floating-point math: faster, and a few games misbehave."),
        TR("Less exact memory transfers to the GPU: faster, and a few games show wrong graphics."),
        TR("Keeps what a game reads back from the GPU exact. Off is faster, and some effects "
           "break."),
        TR("Skips some checks when a game changes memory the GPU uses: faster, and textures "
           "can be stale."),
        TR("Gives back memory a game left empty, 16 KiB at a time. More free memory; a game that "
           "writes there again can stutter for a moment.")};
    // The memory row has no line of its own: every language would need one, and its value says
    // what it is (a size of memory) while the message after it changes says when it arrives.
    if (row >= kPerformanceSwitches)
        return "";
    return kAbout[row];
}

// ---------------------------------------------------------------- dialogs

int Launcher::dialog_rows(Modal modal) const
{
    switch (modal)
    {
    case Modal::video:
        return (services_.frame_gen_state() == 0 && services_.mihawkgen_state() == 0)
            ? static_cast<int>(video_frame_gen) : static_cast<int>(kVideoRows);
    case Modal::hud:
        return kHudRows;
    case Modal::performance:
        return kPerformanceRows;
    case Modal::audio:
    case Modal::accessibility:
        return 3;
    case Modal::appearance:
        return 3; // theme, launcher and wallpaper
    case Modal::wallpaper:
        return 1 + static_cast<int>(wallpaper_files_.size()); // "None" and the images
    case Modal::theme:
        return static_cast<int>(themes().size());
    case Modal::accent:
        return static_cast<int>(accent_presets().size()) + 1;
    case Modal::game:
        // Console mode, video, performance, audio, controls, system, language, mods; save data in
        // builds that move saves.
        // A game a download source has can be deleted from the console (and downloaded again).
        return (services_.save_transfer_available() ? 9 : 8) +
               (library_.selected < static_cast<int>(games_.size()) &&
                        !games_[static_cast<std::size_t>(library_.selected)].sources.empty() &&
                        !games_[static_cast<std::size_t>(library_.selected)].remote ? 1 : 0);
    case Modal::controls:     // vibration, its strength, the button mapping, the menu's own buttons
        return kControlRows;
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
    case Modal::appearance:
    case Modal::theme:
        return 370.0f + 102.0f * static_cast<float>(row);
    case Modal::accent: // six of its rows show; the list scrolls to the others
        return kVideoRowsTop + kAccentRowPitch * static_cast<float>(row) - accent_rows_.scroll();
    case Modal::performance: // four of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - performance_rows_.scroll();
    case Modal::video: // five of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - video_rows_.scroll();
    case Modal::wallpaper: // four of its rows show; the list scrolls to the others
        return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - wallpaper_rows_.scroll();
    case Modal::game:
        return 334.0f + 96.0f * static_cast<float>(row);
    case Modal::controls: // under the shortcuts; the list scrolls
        return kControlsRowsTop + kVideoRowPitch * static_cast<float>(row) - controls_rows_.scroll();
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
        if (modal_ == Modal::theme || modal_ == Modal::wallpaper)
        {
            // Back to Appearance, on the row that opened these options.
            const bool from_wallpaper = modal_ == Modal::wallpaper;
            modal_ = modal_shown_ = Modal::appearance;
            option_ = from_wallpaper ? 2 : 0;
            option_cursor_.snap(dialog_row_top(Modal::appearance, option_));
            message_.clear();
            cue(Cue::back);
        }
        else
            close_modal();
        return;
    }
    if ((key == Key::up || key == Key::down) &&
        (modal_ == Modal::video || modal_ == Modal::performance || modal_ == Modal::controls ||
         modal_ == Modal::accent || modal_ == Modal::wallpaper))
    {
        ListView &rows_view = modal_ == Modal::video       ? video_rows_
                              : modal_ == Modal::controls  ? controls_rows_
                              : modal_ == Modal::accent    ? accent_rows_
                              : modal_ == Modal::wallpaper ? wallpaper_rows_
                                                           : performance_rows_;
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
    if (modal_ == Modal::appearance)
    {
        if (!activate)
            return;
        if (option_ == 0)
        {
            open_modal(Modal::theme);
            option_ = theme_index();
            option_cursor_.snap(dialog_row_top(Modal::theme, option_));
        }
        else if (option_ == 1)
        {
            launcher_picker_ = true;
            cue(Cue::open);
        }
        else
        {
            // The wallpaper's own dialog (docs/WALLPAPERS.md): the folder's images, each on its
            // own row, with "None" above them.
            std::vector<std::string> files = services_.wallpapers();
            std::vector<std::string> images;
            for (std::string &name : files)
                if (WallpaperImage(name))
                    images.push_back(std::move(name));
            wallpaper_files_ = std::move(images);
            open_modal(Modal::wallpaper);
            wallpaper_rows_.visible = kWallpaperRowsShown;
            wallpaper_rows_.pitch = kVideoRowPitch;
            const int row = WallpaperRow(prefs_.wallpaper, wallpaper_files_);
            wallpaper_rows_.reset(dialog_rows(Modal::wallpaper), row);
            option_ = row;
            option_cursor_.snap(dialog_row_top(Modal::wallpaper, option_));
        }
        return;
    }
    if (modal_ == Modal::theme)
    {
        // The look changes on the spot, and is kept for the next start.
        if (!activate)
            return;
        const Theme &chosen = themes()[static_cast<std::size_t>(std::clamp(option_, 0, rows - 1))];
        const std::string had = prefs_.theme;
        const std::string had_accent = prefs_.accent;
        prefs_.theme = chosen.id;
        if (had != prefs_.theme) prefs_.accent.clear(); // keep the colour when reselecting this theme
        const bool saved = save_preferences(true);
        if (saved)
        {
            use_theme(chosen.id);
            settings_.visible = th().list_settings ? 10 : kCategoryCount;
            settings_.pitch = th().list_settings ? 84.0f : 88.0f;
            settings_.reset(kCategoryCount, settings_.selected);
            use_accent(prefs_.accent);
        }
        else
        {
            prefs_.theme = had;
            prefs_.accent = had_accent;
        }
        cue(saved ? Cue::saved : Cue::error);
        return;
    }
    if (modal_ == Modal::accent)
    {
        if (!activate)
            return;
        const auto presets = accent_presets();
        const int chosen = std::clamp(option_, 0, rows - 1);
        const std::string had = prefs_.accent;
        prefs_.accent = chosen == 0 ? "" : presets[static_cast<std::size_t>(chosen - 1)].id;
        const bool saved = save_preferences(true);
        if (saved)
        {
            use_accent(prefs_.accent);
        }
        else
            prefs_.accent = had;
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
        else if (option_ == video_post_processing)
        {
            const int count = static_cast<int>(services_.post_processing_labels().size());
            prefs_.post_processing = (std::clamp(prefs_.post_processing, 0, count - 1) + step + count) % count;
        }
        else if (option_ == video_overlay_options)
        {
            // The overlay's options have their own list (settings.cpp, Modal::hud).
            if (activate)
                open_hud();
            return;
        }
        else if (option_ == video_frame_gen_method) {
            auto method_available = [&](int m) {
                return m == 0 ? services_.frame_gen_state() == 2
                              : services_.mihawkgen_state() == 2;
            };
            int m = std::clamp(prefs_.frame_gen_method, 0, kFrameGenMethods - 1);
            for (int i = 0; i < kFrameGenMethods; ++i) {
                m = (m + step + kFrameGenMethods) % kFrameGenMethods;
                if (method_available(m)) break;
            }
            prefs_.frame_gen_method = m;
        }
        else if (option_ >= video_frame_gen && option_ != video_frame_gen_method &&
                 (prefs_.frame_gen_method == 0 ? services_.frame_gen_state() != 2
                                               : services_.mihawkgen_state() != 2))
        {
            if (prefs_.frame_gen_method == 0)
                say(tr("Frame generation needs Lossless.dll, from Lossless Scaling, in the lossless folder."), true);
            else
                say(tr("Frame generation needs MihawkGen weights (network.fgw)."), true);
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
            prefs_.hud = !prefs_.hud; // the FPS overlay's own switch
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
        if (option_ == speed_memory_layout)
        {
            const int count = static_cast<int>(std::size(kMemoryLayouts));
            prefs_.memory_layout =
                (std::clamp(prefs_.memory_layout, 0, count - 1) + step + count) % count;
            break;
        }
        bool *const switches[kPerformanceSwitches] = {
            &prefs_.block_list, &prefs_.async_shaders,     &prefs_.fast_gpu, &prefs_.unsafe_cpu,
            &prefs_.unsafe_dma, &prefs_.reactive_flushing, &prefs_.skip_invalidation,
            &prefs_.compact_memory};
        bool &value = *switches[std::clamp(option_, 0, kPerformanceSwitches - 1)];
        value = !value;
        break;
    }
    case Modal::controls:
        if (option_ == kControlMapping)
        {
            // The button mapping has its own list (game_options.cpp).
            if (activate)
                open_mapping(false);
            return;
        }
        if (option_ == kControlMenuKeys)
        {
            // The menu's own buttons have their own list (game_options.cpp).
            if (activate)
                open_menu_mapping();
            return;
        }
        if (option_ == kControlStrength)
        {
            // The strength: a quarter at a time, never off (the switch above is for that).
            if (!adjust)
                return;
            prefs_.vibration_strength = std::clamp(prefs_.vibration_strength + 25 * step, 25, 100);
            sound = Cue::slider;
            // The controller answers at that strength, so the level is chosen by feel; with the
            // vibration switched off it stays still.
            if (prefs_.vibration)
                services_.vibration_pulse(prefs_.vibration_strength);
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
    case Modal::wallpaper:
        if (!activate)
            return;
        {
            // "None" (row 0) shows the theme's own art again; a row below is that image. The
            // picture changing is the confirmation, as the theme's look changing is.
            const int chosen =
                option_ >= 1 && option_ - 1 < static_cast<int>(wallpaper_files_.size()) ? option_ - 1 : -1;
            prefs_.wallpaper =
                chosen < 0 ? std::string{} : wallpaper_files_[static_cast<std::size_t>(chosen)];
            sound = Cue::saved;
        }
        break;
    default:
        return;
    }
    // The launcher's look changes on the spot: that is its own confirmation.
    if (!save_preferences(modal_ == Modal::accessibility || modal_ == Modal::wallpaper))
    {
        prefs_ = before;
        sound = Cue::error;
    }
    else if (modal_ == Modal::video && option_ == video_refresh && prefs_.refresh == 1)
    {
        // 120 Hz is a request: the display has the last word.
        say(tr("Saved. A display that cannot show 120 Hz stays at 60 Hz."));
    }
    else if (modal_ == Modal::video && option_ == video_post_processing && prefs_.post_processing > 0)
    {
        // Eden's ReShade FX pass is drawn by the Vulkan renderer; OpenGL shows the game as it is.
        say(prefs_.renderer != 0 ? tr("Saved. Applies when a game starts.")
                                 : tr("Saved. Post-processing needs the Vulkan renderer."));
    }
    else if (modal_ == Modal::video && option_ == video_astc && prefs_.astc == 1)
    {
        // Held decoded, these textures take four times the memory they take compressed.
        say(tr("Saved. Full quality uses four times the graphics memory for these textures and can run out "
               "above 1x."));
    }
    else if (modal_ == Modal::performance && option_ == speed_memory_layout)
    {
        // The guest's memory is taken as a game starts, so this says when the change arrives.
        say(tr("Saved. Applies when a game starts."));
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
    case Modal::appearance:
        title = tr("Appearance");
        copy = tr("Theme and launcher settings.");
        break;
    case Modal::wallpaper:
        title = tr("Wallpaper");
        copy = tr("An image of your own behind this menu.");
        break;
    case Modal::theme:
        title = tr("Theme");
        copy = tr("The look of this menu.");
        break;
    case Modal::accent:
        title = tr("Colour");
        copy = tr("The accent colour of the current theme.");
        break;
    default:
        title = tr("Diagnostics");
        copy = option_ == 0 ? tr("Detailed logs apply to the next game launch.") :
                             tr("Keeps the last line before a crash, but games stutter. From the next start.");
        break;
    }
    text_shrink(c, title, 592.0f, baseline(218.0f, 62.0f, th().size_display), th().size_display,
                th().title, 736.0f);
    text_shrink(c, copy, 592.0f, baseline(291.0f, 32.0f, th().size_small), th().size_small,
                th().copy, 736.0f);

    const int rows = dialog_rows(modal);
    // Video's rows scroll in a window of five, Performance's in one of four, Controls' in one
    // of three under its shortcuts, Colour's in one of six and Wallpaper's in one of four; the
    // other dialogs show all of theirs.
    const bool scrolls = modal == Modal::video || modal == Modal::performance ||
                         modal == Modal::controls || modal == Modal::accent ||
                         modal == Modal::wallpaper;
    const ListView &rows_view = modal == Modal::performance    ? performance_rows_
                                : modal == Modal::controls     ? controls_rows_
                                : modal == Modal::accent       ? accent_rows_
                                : modal == Modal::wallpaper    ? wallpaper_rows_
                                                               : video_rows_;
    const Rect &window = modal == Modal::performance    ? kPerformanceWindow
                         : modal == Modal::controls     ? kControlsWindow
                         : modal == Modal::accent       ? kAccentWindow
                         : modal == Modal::wallpaper    ? kWallpaperWindow
                                                        : kVideoWindow;
    const float rows_top = modal == Modal::controls ? kControlsRowsTop : kVideoRowsTop;
    const int first = scrolls ? rows_view.first_row() : 0;
    const int last = scrolls ? rows_view.last_row() : rows - 1;
    if (scrolls)
        list.push_clip({window.x - 24.0f, window.y - 6.0f, window.w + 48.0f, window.h + 12.0f});
    const float row_h = modal == Modal::accent ? kAccentRowHeight : 94.0f;
    for (int row = first; row <= last; ++row)
    {
        list.push_opacity(scrolls ? rows_view.row_alpha(row, row_h) : 1.0f);
        plate_rest(c, row_plate(), {592.0f, dialog_row_top(modal, row), 736.0f, row_h});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(),
                {592.0f,
                 scrolls ? rows_top + rows_view.cursor() - rows_view.scroll() :
                           option_cursor_.value,
                 736.0f, row_h},
                1.0f);

    // A row's name takes what its control (`taken` wide, at the right) leaves of the row.
    const auto label = [&](int row, const char *value, float taken)
    {
        text_shrink(c, value, 628.0f, baseline(dialog_row_top(modal, row), row_h, th().size_text),
                    th().size_text, th().value, 664.0f - taken - 28.0f);
    };
    const auto choice = [&](int row, const std::string &value)
    {
        return chooser(c, value, 1296.0f,
                       baseline(dialog_row_top(modal, row), row_h, th().size_text),
                       row == option_ ? 1.0f : 0.0f, th().accent_pale);
    };
    constexpr float kToggle = 64.0f;
    const auto row_centre = [&](int row) { return dialog_row_top(modal, row) + row_h * 0.5f; };
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
            std::string{}, // Overlay options: an opener
            pick(services_.post_processing_labels(), prefs_.post_processing),
            on_off(prefs_.frame_gen),
            std::string{kFrameGenMethodNames[std::clamp(prefs_.frame_gen_method, 0, kFrameGenMethods - 1)]},
            prefs_.frame_gen_target <= 0 ? std::string{tr("Auto")} :
                fill(tr("{0} Hz"), {kFrameGenTargetNames[std::clamp(prefs_.frame_gen_target, 1, kFrameGenTargets - 1)]}),
            kFrameGenMultiplierNames[std::clamp(prefs_.frame_gen_multiplier, 0, kFrameGenMultipliers - 1)],
        };
        const bool lsfg_locked = services_.frame_gen_state() != 2;
        const bool mihawkgen_locked = services_.mihawkgen_state() != 2;
        const bool frame_gen_locked = prefs_.frame_gen_method == 0 ? lsfg_locked :
                                      prefs_.frame_gen_method == 1 ? mihawkgen_locked : true;
        static constexpr const char *kNames[kVideoRows] = {
            TR("Renderer"),         TR("Output resolution"), TR("Resolution"),
            TR("Upscaling filter"), TR("ASTC textures"),     TR("Refresh rate"),
            TR("FPS overlay"),      TR("Overlay options"),   TR("Post-processing"),
            TR("Frame generation"), TR("Frame gen method"),  TR("Frame gen target"),  TR("Frame gen multiplier")};
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(video_rows_.row_alpha(row, kVideoRowHeight));
            if (row == video_overlay)
            {
                label(row, tr(kNames[row]), kToggle);
                toggle(c, 1292.0f, row_centre(row), knob);
            }
            else if (row == video_overlay_options)
            {
                // Opens the overlay's options (Modal::hud): a hint that more is inside.
                const float shown = text_shrink(c, ">", 1292.0f,
                                                baseline(dialog_row_top(modal, row), 94.0f, th().size_small),
                                                th().size_small, th().meta, 320.0f, Align::right);
                label(row, tr(kNames[row]), shown);
            }
            else if (row >= video_frame_gen && row != video_frame_gen_method && frame_gen_locked)
            {
                // Greyed out: the selected method's requirement is not met.
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
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(rows_view.row_alpha(row, kVideoRowHeight));
            if (row == kControlVibration)
            {
                label(row, tr("Vibration"), kToggle);
                toggle(c, 1292.0f, row_centre(row), knob);
            }
            else if (row == kControlStrength)
            {
                const float value =
                    text(c, percent(prefs_.vibration_strength), 1292.0f,
                         baseline(dialog_row_top(modal, row), 94.0f, th().size_text), th().size_text,
                         th().accent_pale, Align::right);
                const float gap = std::max(96.0f, value + 20.0f);
                level_bar(c, 1292.0f - gap, row_centre(row), 260.0f,
                          static_cast<float>(prefs_.vibration_strength) / 100.0f,
                          option_ == row ? 1.0f : 0.0f);
                label(row, tr("Vibration strength"), 260.0f + gap);
            }
            else
            {
                // An opener (Button mapping, Menu keybinds): as usual or changed, opened with Cross.
                const bool changed = row == kControlMapping ? prefs_.mapping != kDefaultMapping
                                                            : prefs_.menu_bind != kDefaultMenuBinding;
                const float shown = text_shrink(
                    c, changed ? tr("Changed") : tr("As usual"), 1292.0f,
                    baseline(dialog_row_top(modal, row), 94.0f, th().size_small), th().size_small,
                    changed ? th().accent_pale : th().meta, 320.0f, Align::right);
                label(row, row == kControlMapping ? tr("Button mapping") : tr("Menu keybinds"), shown);
            }
            list.pop_opacity();
        }
        break;
    }
    case Modal::performance:
    {
        static constexpr const char *kNames[kPerformanceRows] = {
            TR("Compile ahead"),        TR("Asynchronous shaders"), TR("Faster GPU emulation"),
            TR("Faster CPU emulation"), TR("Faster DMA"),           TR("Reactive flushing"),
            TR("Skip CPU invalidation"), TR("Compact memory"), TR("Memory")};
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(rows_view.row_alpha(row, kVideoRowHeight));
            if (row == speed_memory_layout)
            {
                // Not a switch: the memory the game's console is told it has. 4 GiB is what a
                // Switch has; more is for a mod that keeps a heap of its own (docs/MODS.md).
                label(row, tr(kNames[row]),
                      choice(row, kMemoryLayouts[std::clamp(prefs_.memory_layout, 0,
                                                            static_cast<int>(std::size(kMemoryLayouts)) - 1)]));
                list.pop_opacity();
                continue;
            }
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
    case Modal::appearance:
    {
        static constexpr const char *kNames[] = {TR("Theme"), TR("Launcher"), TR("Wallpaper")};
        const std::string wallpaper_name =
            prefs_.wallpaper.empty() ? std::string{tr("None")} : prefs_.wallpaper;
        for (int row = 0; row < rows; ++row)
        {
            const float shown =
                row == 0 ? text_shrink(c, th().name, 1292.0f,
                                       baseline(dialog_row_top(modal, row), 94.0f, th().size_small),
                                       th().size_small, th().meta, 320.0f, Align::right) :
                row == 2 ? text_shrink(c, wallpaper_name, 1292.0f,
                                       baseline(dialog_row_top(modal, row), 94.0f, th().size_small),
                                       th().size_small, th().meta, 320.0f, Align::right) :
                           0.0f;
            label(row, tr(kNames[row]), shown);
        }
        const char *about = option_ == 0    ? tr("Choose the colours and appearance of this menu.") :
                            option_ == 1    ? tr("Use another home screen, library and menu layout.") :
                                              tr("Show an image of your own behind the menu.");
        text_block(c, about, 592.0f, baseline(700.0f, 30.0f, th().size_small), th().size_small,
                   30.0f, th().meta, 736.0f, 2, kShrink);
        break;
    }
    case Modal::wallpaper:
    {
        // "None" and every image the folder holds: what is in use carries the mark (as the
        // theme and colour dialogs mark theirs).
        const int current = WallpaperRow(prefs_.wallpaper, wallpaper_files_);
        const auto row_name = [&](int row) -> const char *
        {
            if (row == 0)
                return tr("None");
            const std::size_t index = static_cast<std::size_t>(row - 1);
            return index < wallpaper_files_.size() ? wallpaper_files_[index].c_str() : "";
        };
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(rows_view.row_alpha(row, kVideoRowHeight));
            label(row, row_name(row), 64.0f);
            if (row == current)
            {
                list.circle(1268.0f, row_centre(row), 17.0f, th().accent);
                const bool contrast = look().high_contrast;
                look().high_contrast = false;
                icon(c, Icon::check, 1268.0f, row_centre(row), 22.0f, th().on_accent);
                look().high_contrast = contrast;
            }
            list.pop_opacity();
        }
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
    case Modal::accent:
    {
        const auto presets = accent_presets();
        const int active = accent_index();
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(rows_view.row_alpha(row, row_h));
            const char *name = row == 0 ? tr("Default") : presets[static_cast<std::size_t>(row - 1)].name;
            label(row, name, 64.0f);
            const bool current = (row == 0 && active < 0) || (row > 0 && row - 1 == active);
            if (current)
            {
                list.circle(1268.0f, row_centre(row), 17.0f, th().accent);
                const bool contrast = look().high_contrast;
                look().high_contrast = false;
                icon(c, Icon::check, 1268.0f, row_centre(row), 22.0f, th().on_accent);
                look().high_contrast = contrast;
            }
            list.pop_opacity();
        }
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
        // The folder stays available after the startup notice is gone.
        const std::string reports = services_.crash_reports_folder();
        text_block(c, reports.empty() ? std::string{tr(kAbout[std::clamp(option_, 0, 1)])} :
                       fill(tr("Crash reports (FTP): {0}\nAttach crash-*.txt and its matching logs to your issue."), {reports}),
                   592.0f, baseline(716.0f, 26.0f, th().size_small),
                   th().size_small, 26.0f, th().body, 736.0f, 3, kShrink);
        break;
    }
    }

    if (scrolls)
    {
        list.pop_clip();
        scrollbar(c, rows_view, 1340.0f, window.y, window.h);
    }
    if (modal == Modal::performance && option_ < kPerformanceSwitches)
    {
        // What the highlighted switch does, under the window.
        text_block(c, tr(performance_about(option_)), 592.0f,
                   baseline(window.y + window.h + 22.0f, 30.0f, th().size_small), th().size_small, 30.0f,
                   th().meta, 736.0f, 2, kShrink);
    }
    if (modal == Modal::wallpaper)
    {
        // The folder the images are put in (as Performance says what its switches do).
        const std::string folder = services_.wallpapers_folder();
        text_block(c, folder.empty() ?
                          std::string{tr("Put images in the wallpapers folder to show them here.")} :
                          fill(tr("Images in this folder: {0}"), {short_path(folder, 64)}),
                   592.0f, baseline(window.y + window.h + 22.0f, 30.0f, th().size_small), th().size_small,
                   30.0f, th().meta, 736.0f, 2, kShrink);
    }

    // Under the rows: Video's five, Performance's four with their line of text, and Controls'
    // that scroll under its shortcuts all end lower than the other dialogs' rows.
    const float foot = scrolls ? 848.0f : 811.0f;
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
    else if (modal == Modal::controls && (option_ == kControlMapping || option_ == kControlMenuKeys))
    {
        static constexpr Hint kOpen[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kOpen, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else if (modal == Modal::video && option_ == video_overlay_options)
    {
        static constexpr Hint kOpen[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kOpen, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else if (modal == Modal::appearance)
    {
        static constexpr Hint kOpen[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kOpen, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else if (modal == Modal::theme || modal == Modal::wallpaper)
    {
        static constexpr Hint kApply[] = {
            {Pad::updown, TR("Select")}, {Pad::cross, TR("Apply")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kApply, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else if (rows > 1)
    {
        static constexpr Hint kHints[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kHints, 3, 592.0f, foot, th().copy, 736.0f);
    }
    else
    {
        static constexpr Hint kHints[] = {{Pad::cross, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kHints, 2, 592.0f, foot, th().copy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::open_hud()
{
    hud_rows_.visible = kHudRowsShown;
    hud_rows_.pitch = kVideoRowPitch;
    hud_rows_.reset(kHudRows, 0);
    modal_ = modal_shown_ = Modal::hud;
    message_.clear();
    cue(Cue::open);
}

void Launcher::press_hud(Key key)
{
    const bool adjust = key == Key::left || key == Key::right;
    const bool activate = key == Key::cross;
    const int step = key == Key::left ? -1 : 1;
    if (key == Key::circle)
    {
        // Back to Video, on the row that opened these options (video_rows_ never left it).
        modal_ = modal_shown_ = Modal::video;
        option_ = video_overlay_options;
        option_cursor_.snap(dialog_row_top(Modal::video, option_));
        message_.clear();
        cue(Cue::back);
        return;
    }
    if (key == Key::up || key == Key::down)
    {
        if (hud_rows_.move(key == Key::down ? 1 : -1))
        {
            option_ = hud_rows_.selected;
            message_.clear();
            cue(Cue::focus);
        }
        return;
    }
    if (!adjust && !activate)
        return;
    const int row = std::clamp(hud_rows_.selected, 0, kHudRows - 1);
    const Preferences before = prefs_;
    if (row == hud_colors_row)
        prefs_.hud_colors = !prefs_.hud_colors;
    else if (row == hud_size_row)
        prefs_.hud_size = (std::clamp(prefs_.hud_size, 0, kHudSizes - 1) + step + kHudSizes) % kHudSizes;
    else if (row == hud_corner_row)
        prefs_.hud_corner = (std::clamp(prefs_.hud_corner, 0, kHudCorners - 1) + step + kHudCorners) % kHudCorners;
    else
    {
        // One of the line's pieces, turned on or off (hud.h's kHudField*).
        prefs_.hud_fields ^= 1 << (row - hud_backend_row);
    }
    const bool saved = save_preferences();
    if (!saved)
        prefs_ = before;
    cue(saved ? Cue::toggle : Cue::error);
}

void Launcher::draw_hud(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f),
          1.6f);
    text_shrink(c, tr("Overlay options"), 592.0f, baseline(218.0f, 62.0f, th().size_display),
                th().size_display, th().title, 736.0f);
    text_fit(c, tr("What the FPS overlay's line says, and how the chip is drawn."), 592.0f,
             baseline(291.0f, 32.0f, th().size_small), th().size_small, th().copy, 736.0f);

    list.push_clip({kHudWindow.x - 24.0f, kHudWindow.y - 6.0f, kHudWindow.w + 48.0f, kHudWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kVideoRowsTop + kVideoRowPitch * static_cast<float>(row) - hud_rows_.scroll(); };
    for (int row = hud_rows_.first_row(); row <= hud_rows_.last_row(); ++row)
    {
        list.push_opacity(hud_rows_.row_alpha(row, kVideoRowHeight));
        plate_rest(c, row_plate(), {592.0f, row_top(row), 736.0f, kVideoRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(),
                {592.0f, kVideoRowsTop + hud_rows_.cursor() - hud_rows_.scroll(), 736.0f, kVideoRowHeight},
                1.0f);

    // A row's name takes what its control (`taken` wide, at the right) leaves of the row.
    const auto label = [&](int row, const char *value, float taken)
    {
        text_shrink(c, value, 628.0f, baseline(row_top(row), kVideoRowHeight, th().size_text),
                    th().size_text, th().value, 664.0f - taken - 28.0f);
    };
    const auto choice = [&](int row, const std::string &value)
    {
        return chooser(c, value, 1296.0f, baseline(row_top(row), kVideoRowHeight, th().size_text),
                       row == hud_rows_.selected ? 1.0f : 0.0f, th().accent_pale);
    };
    constexpr float kToggle = 64.0f;
    const auto row_centre = [&](int row) { return row_top(row) + kVideoRowHeight * 0.5f; };

    static constexpr const char *kNames[kHudRows] = {
        TR("Enable colors"), TR("Size"), TR("Corner"), TR("Backend"), TR("FPS"),
        TR("Frame time"), TR("Speed"), TR("CPU"), TR("GPU"), TR("Memory"), TR("Ping"),
        TR("CPU by core"), TR("Resolution")};
    for (int row = hud_rows_.first_row(); row <= hud_rows_.last_row(); ++row)
    {
        list.push_opacity(hud_rows_.row_alpha(row, kVideoRowHeight));
        if (row == hud_colors_row)
        {
            label(row, tr(kNames[row]), kToggle);
            toggle(c, 1292.0f, row_centre(row), prefs_.hud_colors ? 1.0f : 0.0f);
        }
        else if (row == hud_size_row)
            label(row, tr(kNames[row]),
                  choice(row, tr(kHudSizeNames[std::clamp(prefs_.hud_size, 0, kHudSizes - 1)])));
        else if (row == hud_corner_row)
            label(row, tr(kNames[row]),
                  choice(row, tr(kHudCornerNames[std::clamp(prefs_.hud_corner, 0, kHudCorners - 1)])));
        else
        {
            // One of the line's pieces, on or off.
            // "CPU by core" is on while its bit is clear: on in every setting saved before it.
            const bool on = ((prefs_.hud_fields & (1 << (row - hud_backend_row))) != 0) != (row == hud_cores_row);
            label(row, tr(kNames[row]), kToggle);
            toggle(c, 1292.0f, row_centre(row), on ? 1.0f : 0.0f);
        }
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, hud_rows_, 1340.0f, kHudWindow.y, kHudWindow.h);

    const float foot = 848.0f;
    if (!message_.empty())
        notice_block(c, message_, 592.0f, foot - 6.0f, th().size_small, 26.0f,
                     message_warning_ ? th().warning : th().accent_pale, 736.0f, 2, message_warning_);
    else
    {
        static constexpr Hint kHints[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kHints, 3, 592.0f, foot, th().copy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
