// ProsperoEden - Launcher library: the game list, its details and per-game settings.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include "pe/core/log.hpp"

#include <cctype>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

namespace pe::ui
{

using audio::Cue;

namespace
{

// The Library: covers four to a row at the left, the selected game's details at the right.
constexpr int kColumns = 4;
constexpr float kCover = 240.0f;
constexpr float kCellPitchX = kCover + 32.0f;
constexpr float kCellPitchY = 346.0f; // a cover, its name on up to two lines, a line under it, and the gap
constexpr float kGridTop = 152.0f;
constexpr float kGridBottom = 972.0f;
constexpr float kGridWidth = kCellPitchX * (kColumns - 1) + kCover;
constexpr Rect kDetail{1304.0f, 136.0f, 520.0f, 836.0f};

// A game's settings: a page of three parts beside the rail.
constexpr Rect kKinds{layout::kLeft, 160.0f, 400.0f, 792.0f};
constexpr float kKindPitch = 80.0f;
constexpr float kKindHeight = 72.0f;
constexpr Rect kMiddle{624.0f, 160.0f, 760.0f, 792.0f};
constexpr Rect kExplain{1424.0f, 160.0f, 400.0f, 792.0f};

// The game page's rows on the left. Video to Language open what the game does differently from
// Settings (game_options.cpp).
enum GameRow : int
{
    row_mode,
    row_video,
    row_performance,
    row_audio,
    row_controls,
    row_system,
    row_language,
    row_mods,
    row_save,   // in builds that move saves
    row_delete, // a game a download source has: deleted from the console
};
constexpr const char *kKindNames[] = {TR("Console mode"), TR("Video"),    TR("Performance"),
                                      TR("Audio"),        TR("Controls"), TR("System"),
                                      TR("Language"),     TR("Mods"),     TR("Save data"),
                                      TR("Delete from console")};

// How many settings a game has of its own.
int changes_of(const GameSettings &game)
{
    return (game.renderer >= 0) + (game.resolution >= 0) + (game.filter >= 0) + (game.astc >= 0) +
           (game.refresh >= 0) + (game.hud >= 0) + (game.hud_size >= 0) + (game.hud_corner >= 0) +
           (game.frame_gen >= 0) + (game.frame_gen_target >= 0) + (game.frame_gen_multiplier >= 0) +
           static_cast<int>(std::count_if(game.performance.begin(), game.performance.end(),
                                          [](int value) { return value >= 0; })) +
           (game.volume >= 0) + (game.mute >= 0) + (game.vibration >= 0) + (game.own_mapping ? 1 : 0) +
           (game.controller >= 0) + (game.rng_seed >= 0) + (game.language >= 0);
}

// "3 changed", or that the game follows Settings.
std::string changes_text(int changes)
{
    return changes == 0 ? std::string{tr("Follows Settings")} : fill(tr("{0} changed"), {std::to_string(changes)});
}

} // namespace

void Launcher::start_scan()
{
    if (!scan_.valid() && home_.setup_ready)
        scan_ = std::async(std::launch::async, [this] { return services_.games(); });
}

// A game's name as it is shown: with the characters a font has, the launcher's own or the
// console's. One with nothing left that says anything (a name in a script no font here has, or
// on a console whose fonts could not be read) is replaced by the name its file gives, rather than
// drawn as question marks.
static std::string shown_name(const gfx::Font *font, const std::string &name, const std::string &file)
{
    if (font == nullptr || name.empty() || font->can_draw(name))
        return name;
    std::string kept;
    for (const char c : font->drawable(name))
        if (c != ' ' || (!kept.empty() && kept.back() != ' '))
            kept += c;
    while (!kept.empty() && kept.back() == ' ')
        kept.pop_back();
    const bool says = std::any_of(kept.begin(), kept.end(), [](char c)
                                  { return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                                           static_cast<unsigned char>(c) >= 0x80; });
    if (says)
        return kept;
    const std::string stem = file.substr(0, file.find_last_of('.'));
    return stem.empty() ? name : stem;
}

void Launcher::finish_scan(bool wait)
{
    if (!scan_.valid())
        return;
    if (!wait && scan_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try
    {
        apply_games(scan_.get());
    }
    catch (const std::exception &error)
    {
        sys::log("game list: %s", error.what());
        games_loaded_ = true; // the list stays as it was
    }
}

void Launcher::apply_games(std::vector<Game> games)
{
    // The same games in the same order (the usual case) keep the list where it is; otherwise
    // the selection follows its game.
    bool same = games_loaded_ && games.size() == games_.size();
    for (std::size_t i = 0; same && i < games.size(); ++i)
        same = games[i].file == games_[i].file;
    const std::string selected = library_.selected < static_cast<int>(games_.size()) ?
                                     games_[static_cast<std::size_t>(library_.selected)].file :
                                     std::string{};
    games_ = std::move(games);
    for (Game &game : games_)
        game.name = shown_name(fonts_.font, game.name, game.file);
    games_loaded_ = true;
    for (Game &game : games_)
        if (game.title_id != 0)
            count_mods(game, services_.mods(game.title_id));
    if (!same)
    {
        int index = 0;
        for (int i = 0; i < static_cast<int>(games_.size()); ++i)
            if (games_[static_cast<std::size_t>(i)].file == selected)
                index = i;
        library_.reset(static_cast<int>(games_.size()), index);
        refresh_selected_game();
        mode_.snap(selected_docked_ ? 0.0f : 1.0f);
    }
    name_home_games();
}

void Launcher::name_home_games()
{
    // Games carry their own names; until one has been read the home screen names it by its file.
    for (const Game &game : games_)
    {
        if (game.file == home_.last_file)
            home_.last_title = game.name;
        for (Recent &recent : home_.recents)
            if (recent.file == game.file)
                recent.title = game.name;
    }
}

void Launcher::read_home()
{
    home_ = services_.home();
    home_.last_title = shown_name(fonts_.font, home_.last_title, home_.last_file);
    for (Recent &recent : home_.recents)
        recent.title = shown_name(fonts_.font, recent.title, recent.file);
    if (home_.last_title_id == 0)
        return;
    const std::vector<Mod> mods = services_.mods(home_.last_title_id);
    home_.last_mods = static_cast<int>(mods.size());
    // With the game's Mods switch off none of them is on.
    const bool enabled = mods.empty() || services_.mods_enabled(home_.last_title_id);
    home_.last_mods_on = !enabled ? 0 : static_cast<int>(
        std::count_if(mods.begin(), mods.end(), [](const Mod &mod) { return mod.enabled; }));
}

void Launcher::check_games_present()
{
    // The home screen: its game, or one of its recent ones, is gone. Its focus stays where it can.
    if (screen_ == Screen::home && modal_ == Modal::none)
    {
        bool gone = home_.last_exists && !services_.game_exists(home_.last_file);
        for (const Recent &recent : home_.recents)
            gone = gone || !services_.game_exists(recent.file);
        if (gone)
        {
            read_home();
            const int recents = static_cast<int>(home_.recents.size());
            // (2..9 are recent games, 10 is "view all", 1 is game settings: home.cpp.)
            if (home_focus_ >= 2 && home_focus_ < 10 && home_focus_ - 2 >= recents)
                home_focus_ = recents > 0 ? 1 + recents : 10;
            if (home_focus_ == 1 && !home_.last_exists)
                home_focus_ = 0;
        }
    }
    // The Library's list, whatever screen shows: a game's own dialogs stay with their game.
    if (modal_ == Modal::none)
        drop_missing_games();
}

bool Launcher::drop_missing_games()
{
    if (!games_loaded_)
        return false;
    const std::string selected =
        library_.selected < static_cast<int>(games_.size()) ? games_[static_cast<std::size_t>(library_.selected)].file :
                                                              std::string{};
    // A game on download sources has no file yet: it leaves when their lists do not have it.
    const auto gone = std::remove_if(games_.begin(), games_.end(), [this](const Game &game)
                                     { return !game.remote && !services_.game_exists(game.file); });
    if (gone == games_.end())
        return false;
    games_.erase(gone, games_.end());
    // The selection stays on its game, or on the one that took its place.
    int index = std::min(library_.selected, std::max(0, static_cast<int>(games_.size()) - 1));
    for (int i = 0; i < static_cast<int>(games_.size()); ++i)
        if (games_[static_cast<std::size_t>(i)].file == selected)
            index = i;
    library_.reset(static_cast<int>(games_.size()), index);
    refresh_selected_game();
    return true;
}

void Launcher::count_mods(Game &game, const std::vector<Mod> &mods)
{
    game.mods = static_cast<int>(mods.size());
    // The game's Mods switch (the Library's) comes first: off, none of its mods is used, whatever
    // their own switches say.
    game.mods_enabled = mods.empty() || services_.mods_enabled(game.title_id);
    game.mods_on = !game.mods_enabled ? 0 : static_cast<int>(
        std::count_if(mods.begin(), mods.end(), [](const Mod &mod) { return mod.enabled; }));
    // The home screen says the same of its game.
    if (game.file == home_.last_file && home_.last_title_id != 0)
    {
        home_.last_mods = game.mods;
        home_.last_mods_on = game.mods_on;
    }
}

void Launcher::read_mods(Game &game)
{
    mods_ = services_.mods(game.title_id);
    count_mods(game, mods_);
    mod_list_.clear();
    for (int mod = 0; mod < static_cast<int>(mods_.size()); ++mod)
    {
        mod_list_.push_back({mod, -1});
        for (int cheat = 0; cheat < static_cast<int>(mods_[static_cast<std::size_t>(mod)].cheats.size());
             ++cheat)
            mod_list_.push_back({mod, cheat});
    }
}

std::string Launcher::addons_line(const std::string &addons, int mods, int mods_on, bool brief)
{
    std::string line = addons;
    if (mods > 0)
    {
        // Mods that are switched off are still there: "1 of 2 mods on", or "1/2 mods" where there
        // is little room.
        const std::string count = std::to_string(mods);
        const char *counted = mods == 1 ? tr("{0} mod") : tr("{0} mods");
        line += (line.empty() ? "" : ", ") +
                (mods_on == mods ? fill(counted, {count}) :
                 brief ? fill(counted, {std::to_string(mods_on) + "/" + count}) :
                         fill(tr("{0} of {1} mods on"), {std::to_string(mods_on), count}));
    }
    return line.empty() ? std::string{tr("None")} : line;
}

std::string Launcher::hertz(int refresh)
{
    return fill(tr("{0} Hz"), {refresh == 1 ? "120" : "60"});
}

void Launcher::enter_library()
{
    if (games_loaded_)
    {
        // Games copied to the console since the list was read appear in a moment; games taken
        // away leave it at once.
        finish_scan(false);
        drop_missing_games();
        start_scan();
    }
    else
    {
        start_scan();
        finish_scan(true);
    }
    library_.wrap = false;
    library_.reset(static_cast<int>(games_.size()), 0);
    refresh_selected_game();
    sync_grid(true);
    detail_.snap(1.0f);
    mode_.snap(selected_docked_ ? 0.0f : 1.0f);
}

void Launcher::refresh_selected_game()
{
    const std::uint64_t id =
        games_.empty() ? 0 : games_[static_cast<std::size_t>(library_.selected)].title_id;
    selected_docked_ = id == 0 || services_.docked(id);
    // Its Mods switch shows its state at once; it only animates when changed.
    mods_switch_.snap(!games_.empty() && games_[static_cast<std::size_t>(library_.selected)].mods_enabled ?
                          1.0f : 0.0f);
    detail_.value = 0.0f;
    detail_.velocity = 0.0f;
    // What it has of its own is read once the selection rests (read_selected_details).
    selected_read_ = false;
    selected_changes_ = 0;
    selected_session_.clear();
    sync_grid(false);
}

void Launcher::sync_grid(bool snap)
{
    const int count = static_cast<int>(games_.size());
    const int rows = (count + kColumns - 1) / kColumns;
    const int row = count > 0 ? library_.selected / kColumns : 0;
    const bool panel = th().top_nav;
    grid_rows_.visible = panel ? 3 : 2;
    grid_rows_.pitch = panel ? 248.0f : kCellPitchY;
    grid_rows_.wrap = false;
    if (snap || grid_rows_.count != rows)
        grid_rows_.reset(rows, row);
    else
        grid_rows_.select(row);
    grid_x_.target = static_cast<float>(count > 0 ? library_.selected % kColumns : 0) * kCellPitchX;
    grid_y_.target = static_cast<float>(row) * kCellPitchY;
    // The highlight glides only where it can be seen doing it.
    if (snap || screen_ != Screen::library || transition_.running)
    {
        grid_x_.snap(grid_x_.target);
        grid_y_.snap(grid_y_.target);
    }
}

void Launcher::read_selected_details()
{
    selected_read_ = true;
    selected_changes_ = 0;
    selected_session_.clear();
    if (games_.empty())
        return;
    const Game &game = games_[static_cast<std::size_t>(library_.selected)];
    if (game.title_id == 0 || game.remote)
        return;
    const GameSettings own = services_.game_settings(game.title_id);
    selected_changes_ = changes_of(own);
    selected_session_ = session_line(own, selected_docked_);
}

void Launcher::press_library(Key key)
{
    const int count = static_cast<int>(games_.size());
    const Game *game = count > 0 ? &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
    const auto go = [&](int next)
    {
        if (!library_.select(next))
            return;
        message_.clear();
        refresh_selected_game();
        mode_.snap(selected_docked_ ? 0.0f : 1.0f);
        cue(Cue::focus);
    };
    switch (key)
    {
    case Key::circle:
        go_section(0);
        return;
    case Key::up:
    case Key::down:
    {
        if (count == 0)
            return;
        int next = library_.selected + (key == Key::down ? kColumns : -kColumns);
        // Down onto a place the last row does not have: its last game.
        if (next >= count && library_.selected / kColumns < (count - 1) / kColumns)
            next = count - 1;
        if (next >= 0 && next < count)
            go(next);
        return;
    }
    case Key::left:
        if (count == 0 || library_.selected % kColumns == 0)
            return focus_rail();
        go(library_.selected - 1);
        return;
    case Key::right:
        if (count > 0 && library_.selected % kColumns != kColumns - 1 && library_.selected + 1 < count)
            go(library_.selected + 1);
        return;
    case Key::square:
    {
        // A game on download sources: into the download queue, or out of it.
        if (game != nullptr && game->remote)
        {
            queue_remote(*game);
            return;
        }
        // The Mods switch: all of the game's mods on or off at once. Their own switches (Game
        // settings > Mods) keep their state behind it.
        if (game == nullptr || game->title_id == 0 || game->mods == 0)
        {
            if (game != nullptr)
                cue(Cue::error);
            return;
        }
        Game &chosen = games_[static_cast<std::size_t>(library_.selected)];
        const bool saved = services_.set_mods_enabled(chosen.title_id, !chosen.mods_enabled);
        if (saved)
            count_mods(chosen, services_.mods(chosen.title_id));
        say(saved ? tr("Saved for this game. Applies on next launch.") :
                    tr("Could not save. Please try again."),
            !saved);
        cue(saved ? Cue::toggle : Cue::error);
        return;
    }
    case Key::triangle:
        if (game == nullptr)
            return;
        if (game->remote)
        {
            say(tr("Download the game to change its settings."), true);
            cue(Cue::error);
            return;
        }
        if (game->title_id == 0)
        {
            say(tr("This game's settings cannot be saved (no title ID)."), true);
            cue(Cue::error);
            return;
        }
        game_settings_ = services_.game_settings(game->title_id);
        game_docked_ = selected_docked_;
        game_from_home_ = false;
        import_source_ = services_.save_transfer_available() ?
                             services_.save_import_source(game->title_id) : SaveSource::none;
        import_armed_ = false;
        delete_armed_ = false;
        read_mods(games_[static_cast<std::size_t>(library_.selected)]);
        open_modal(Modal::game);
        pane_.snap(0.0f);
        game_rows_.visible = 10;
        game_rows_.pitch = kKindPitch;
        game_rows_.reset(dialog_rows(Modal::game), 0);
        return;
    case Key::cross:
        if (game == nullptr || !home_.setup_ready)
        {
            cue(Cue::error);
            return;
        }
        if (game->remote)
        {
            play_remote(*game);
            return;
        }
        launch_from_ = {layout::kLeft + static_cast<float>(library_.selected % kColumns) * kCellPitchX,
                        kGridTop + static_cast<float>(library_.selected / kColumns) * kCellPitchY - grid_rows_.scroll(),
                        kCover, kCover};
        launch(game->file, game->name, game->cover);
        return;
    default:
        return;
    }
}

void Launcher::draw_library(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const int count = static_cast<int>(games_.size());

    if (t.top_nav)
    {
        constexpr float kPanelLeft = 960.0f;
        constexpr float kPanelWidth = 864.0f;
        constexpr float kPanelCover = 176.0f;
        constexpr float kPanelCellX = (kPanelWidth - kPanelCover) / 3.0f;
        constexpr float kPanelRowPitch = 248.0f;
        constexpr float kPanelTop = 110.0f;
        const float kPanelBottom = home_.status.empty() ? 980.0f : 804.0f;
        kicker(c, tr("LIBRARY"), kPanelLeft, baseline(80.0f, 30.0f, t.size_small), t.accent_pale, kPanelWidth);
        if (count == 0)
        {
            text(c, games_loaded_ || !home_.setup_ready ? tr("No ROM files found.") : tr("Reading..."),
                 kPanelLeft + kPanelWidth * 0.5f, baseline(400.0f, 38.0f, t.size_text), t.size_text,
                 t.copy, Align::center);
        }
        else
        {
            list.push_clip({kPanelLeft - 16.0f, kPanelTop - 8.0f, kPanelWidth + 32.0f,
                            kPanelBottom - kPanelTop + 8.0f});
            const float scroll = grid_rows_.scroll();
            const float scale_x = kPanelCellX / kCellPitchX;
            const float scale_y = kPanelRowPitch / kCellPitchY;
            const Rect ring{kPanelLeft + grid_x_.value * scale_x,
                            kPanelTop + grid_y_.value * scale_y - scroll,
                            kPanelCover, kPanelCover};
            const int first = grid_rows_.first_row() * kColumns;
            const int last = std::min(count - 1, (grid_rows_.last_row() + 1) * kColumns - 1);
            for (int i = first; i <= last; ++i)
            {
                const Game &game = games_[static_cast<std::size_t>(i)];
                const Rect r{kPanelLeft + static_cast<float>(i % kColumns) * kPanelCellX,
                             kPanelTop + static_cast<float>(i / kColumns) * kPanelRowPitch - scroll,
                             kPanelCover, kPanelCover};
                const float near = tween::clamp01(
                    1.0f - (std::fabs(r.x - ring.x) + std::fabs(r.y - ring.y)) / 150.0f);
                const bool grid_active = screen_ == Screen::home ? home_on_grid_ : !rail_focus_;
                const float f = grid_active ? near : 0.0f;
                const float here = arrive(i - first, 0.018f);
                list.push_opacity(here);
                list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - here) * 14.0f);
                const float sc = 1.0f + (0.04f * f - 0.035f * press_ * f) * motion();
                list.push_transform(sc, r.x + r.w * 0.5f, r.y + r.h, 0.0f, 0.0f);
                focus_ring(c, r, t.radius_card, f);
                if (game.remote)
                    remote_cover(c, game.cover, r, t.radius_card, 0.3f + 0.5f * f);
                else
                    cover(c, game.cover, r, t.radius_card, 0.3f + 0.5f * f);
                list.pop_transform();
                text_shrink(c, game.name, r.x, baseline(r.y + kPanelCover + 6.0f, 24.0f, 18.0f),
                            18.0f, gfx::mix(t.body, t.title, f), kPanelCover);
                list.pop_transform();
                list.pop_opacity();
            }
            list.pop_clip();
            if (grid_rows_.count > grid_rows_.visible && !look().high_contrast)
                list.gradient_rect({kPanelLeft - 16.0f, kPanelBottom - 100.0f, kPanelWidth + 32.0f, 100.0f},
                                   0.0f, t.base.with_alpha(0.0f), t.base.with_alpha(0.72f));
            scrollbar(c, grid_rows_, kPanelLeft + kPanelWidth + 12.0f, kPanelTop,
                      kPanelBottom - kPanelTop - 80.0f);
        }
        if (screen_ != Screen::home)
        {
            static constexpr Hint kHints[] = {{Pad::cross, TR("Play")},
                                              {Pad::triangle, TR("Game settings")},
                                              {Pad::circle, TR("Home")},
                                              {Pad::l1, TR("Sections"), Pad::r1}};
            draw_footer(c, kHints, 4, count > 0 ? list_position(library_.selected + 1, count) : std::string{});
        }
        return;
    }

    const float left = layout::kLeft;
    draw_frame(c, tr("Library"), count > 0 ? fill(tr("{0} games"), {std::to_string(count)}).c_str() : "");

    // ---- the covers ----
    if (count == 0)
    {
        const float cx = left + kGridWidth * 0.5f;
        icon(c, Icon::library, cx, 460.0f, 96.0f, t.rule);
        text_shrink(c, games_loaded_ || !home_.setup_ready ? tr("No ROM files found.") : tr("Reading..."), cx,
                    baseline(540.0f, 38.0f, t.size_text), t.size_text, t.copy, kGridWidth, Align::center);
    }
    else
    {
        // The window is wider than its covers so the highlight's light is not cut at the sides.
        list.push_clip({left - 30.0f, kGridTop - 24.0f, kGridWidth + 60.0f, kGridBottom - kGridTop + 24.0f});
        const float scroll = grid_rows_.scroll();
        const Rect ring{left + grid_x_.value, kGridTop + grid_y_.value - scroll, kCover, kCover};
        const int first = grid_rows_.first_row() * kColumns;
        const int last = std::min(count - 1, (grid_rows_.last_row() + 1) * kColumns - 1);
        for (int i = first; i <= last; ++i)
        {
            const Game &game = games_[static_cast<std::size_t>(i)];
            const Rect r{left + static_cast<float>(i % kColumns) * kCellPitchX,
                         kGridTop + static_cast<float>(i / kColumns) * kCellPitchY - scroll, kCover, kCover};
            // How much of the gliding highlight lies on this cover.
            const float near = tween::clamp01(1.0f - (std::fabs(r.x - ring.x) + std::fabs(r.y - ring.y)) / 150.0f);
            const float f = rail_focus_ ? 0.0f : near;
            const float scale = 1.0f + (0.04f * f - 0.035f * press_ * f) * motion();
            const float here = arrive(i - first, 0.022f);
            list.push_opacity(here);
            list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - here) * 20.0f);
            list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h, 0.0f, 0.0f);
            focus_ring(c, r, t.radius_card, f);
            if (game.remote)
                remote_cover(c, game.cover, r, t.radius_card, 0.5f + 0.7f * f);
            else
                cover(c, game.cover, r, t.radius_card, 0.5f + 0.7f * f);
            const Download *download = game.remote ? download_of(game.key) : nullptr;
            if (download != nullptr &&
                (download->state == DownloadState::downloading || download->state == DownloadState::verifying) &&
                download->total > 0)
            {
                // What is done of its download, over the cover.
                const float done = std::clamp(static_cast<float>(static_cast<double>(download->done) /
                                                                 static_cast<double>(download->total)),
                                              0.0f, 1.0f);
                list.gradient_rect({r.x, r.y + r.h - 96.0f, r.w, 96.0f}, t.radius_card, t.scrim.with_alpha(0.0f),
                                   t.scrim.with_alpha(0.86f));
                text(c, fill(tr("{0}%"), {std::to_string(static_cast<int>(done * 100.0f))}), r.x + r.w - 20.0f,
                     r.y + r.h - 40.0f, t.size_small, t.title, Align::right);
                progress_bar(c, {r.x + 20.0f, r.y + r.h - 28.0f, r.w - 40.0f, 8.0f}, done);
            }
            list.pop_transform();
            // Its name on up to two lines (real names are long), and a line about it under that.
            const float line = look().large_text ? 32.0f : 28.0f;
            const int name_lines = text_block(c, game.name, r.x, baseline(r.y + kCover + 12.0f, line, 22.0f), 22.0f,
                                              line, gfx::mix(t.body, t.title, f), kCover, 2);
            const float under = r.y + kCover + 12.0f + line * static_cast<float>(std::max(1, name_lines));
            if (game.remote)
            {
                Color tone = t.meta;
                const std::string state = remote_state(game, &tone);
                text_shrink(c, state, r.x, baseline(under, 24.0f, 18.0f), 18.0f, tone, kCover);
            }
            else
            {
                text_shrink(c, game.format + "  /  " + game.size, r.x, baseline(under, 24.0f, 18.0f), 18.0f,
                            gfx::mix(t.faint, t.accent_pale, f), kCover);
            }
            list.pop_transform();
            list.pop_opacity();
        }
        list.pop_clip();
        // More rows wait under the window: it fades out over them.
        if (grid_rows_.count > grid_rows_.visible && !look().high_contrast)
            list.gradient_rect({left - 30.0f, kGridBottom - 140.0f, kGridWidth + 60.0f, 140.0f}, 0.0f,
                               t.base.with_alpha(0.0f), t.base.with_alpha(0.92f));
        scrollbar(c, grid_rows_, left + kGridWidth + 22.0f, kGridTop, kGridBottom - kGridTop - 152.0f);
    }

    // ---- the selected game ----
    const Game *game = count > 0 ? &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
    const bool remote = game != nullptr && game->remote;
    const float x = kDetail.x + 36.0f;
    const float width = kDetail.w - 72.0f;
    glass(c, kDetail, t.radius_panel, t.panel.with_alpha(0.90f), t.panel_edge.with_alpha(0.42f), 1.2f);
    const float shown = tween::clamp01(detail_.value);
    list.push_opacity(shown);
    // Its cover again as a band across the top, running out into the panel.
    const Rect band{kDetail.x + 1.0f, kDetail.y + 1.0f, kDetail.w - 2.0f, 148.0f};
    if (game != nullptr && !game->cover.empty())
    {
        const Cover picture = c.textures.cover(game->cover, kDetail.w);
        if (picture.texture != 0)
        {
            // Drawn taller than the band and cut at its foot, so only its upper corners are round.
            const Rect tall{band.x, band.y, band.w, band.h + 60.0f};
            list.push_clip(band);
            list.rounded_image(picture.texture, tall, {0.0f, 0.27f, 1.0f, tall.h / tall.w}, t.radius_panel - 1.0f,
                               Color{1.0f, 1.0f, 1.0f, remote ? 0.55f : 0.9f});
            list.pop_clip();
        }
        // (With high contrast the panel is solid, and of its own colour: glass() in widgets.cpp.)
        const Color ground = look().high_contrast ? gfx::mix(t.scrim, t.panel, 0.4f) : t.panel.with_alpha(0.97f);
        list.gradient_rect({band.x, band.y + 40.0f, band.w, band.h - 39.0f}, 0.0f, ground.with_alpha(0.0f), ground);
    }
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - shown) * 10.0f * motion());
    kicker(c, tr("GAME DETAILS"), x, baseline(kDetail.y + 162.0f, 30.0f, t.size_small), t.accent, width);
    float y = kDetail.y + 198.0f;
    {
        const Heavy heavy(c);
        const int lines = text_block(c, game != nullptr ? game->name : tr("No ROM selected"), x,
                                     baseline(y, 46.0f, t.size_lead), t.size_lead, 46.0f, t.title, width, 2, 0.8f);
        y += 46.0f * static_cast<float>(std::max(1, lines)) + 8.0f;
    }
    if (game != nullptr)
    {
        text_shrink(c, game->format + "  /  " + game->size, x, baseline(y, 30.0f, 22.0f), 22.0f, t.copy, width);
        y += 30.0f;
        text_fit(c, game->file, x, baseline(y, 26.0f, 18.0f), 18.0f, t.faint, width);
        y += 38.0f;
    }
    // What it comes with and the language it will use; where it is, for one still on its sources.
    if (game != nullptr)
    {
        float cx = x;
        const auto add = [&](const std::string &words, Tone tone)
        {
            const float room = x + width - cx;
            if (words.empty() || (cx > x && chip_width(c, words, tone, 2000.0f) > room))
                return;
            cx += chip(c, words, cx, y + 22.0f, tone, room) + 10.0f;
        };
        if (remote)
        {
            Color tone = t.value;
            const std::string state = remote_state(*game, &tone);
            const Download *download = download_of(game->key);
            add(download != nullptr ? state : std::string{tr("Not on the console")},
                download != nullptr && download->state == DownloadState::failed ? Tone::warning : Tone::plain);
            std::string sources;
            for (const std::string &name : game->sources)
                sources += (sources.empty() ? "" : ", ") + name;
            add(sources, Tone::plain);
        }
        else
        {
            add(game->addons_short.empty() ? game->addons : game->addons_short, Tone::plain);
            add(game->language, game->language_note.empty() ? Tone::plain : Tone::warning);
        }
        y += 56.0f;
        if (!remote && !game->language_note.empty())
        {
            notice(c, game->language_note, x, baseline(y, 26.0f, 18.0f), 18.0f, t.warning, width, false);
            y += 30.0f;
        }
    }
    // What it is set to: one line each.
    if (game != nullptr && !remote)
    {
        const bool can_configure = game->title_id != 0;
        const bool has_mods = can_configure && game->mods > 0;
        struct Line
        {
            const char *name;
            std::string value;
            Color color;
        };
        const Line lines[] = {
            {tr("Console mode"),
             can_configure ? std::string{selected_docked_ ? tr("Docked") : tr("Handheld")} : std::string{tr("Unavailable")},
             can_configure ? t.value : t.meta},
            {tr("Mods"),
             !has_mods ? std::string{can_configure ? tr("No mods") : tr("Unavailable")} :
             game->mods_enabled ? fill(tr("{0} of {1} on"), {std::to_string(game->mods_on), std::to_string(game->mods)}) :
                                  std::string{tr("Off")},
             has_mods && game->mods_enabled ? t.accent_pale : t.meta},
            {tr("Game settings"), can_configure ? changes_text(selected_changes_) : std::string{tr("Unavailable")},
             selected_changes_ > 0 ? t.accent_pale : t.meta},
        };
        for (const Line &line : lines)
        {
            const float value = text_shrink(c, line.value, x + width, baseline(y, 46.0f, t.size_small), t.size_small,
                                            line.color, width * 0.6f, Align::right);
            text_shrink(c, line.name, x, baseline(y, 46.0f, t.size_small), t.size_small, t.label,
                        width - value - 20.0f);
            list.rounded_rect({x, y + 45.0f, width, 1.0f}, 0.0f, t.rule.with_alpha(0.45f));
            y += 46.0f;
        }
        // What it runs with, once that is read, where a long name and a note leave it room over
        // the buttons.
        if (!selected_session_.empty() && y + 44.0f <= kDetail.y + kDetail.h - 36.0f - 68.0f - 14.0f - 76.0f - 8.0f)
        {
            icon(c, selected_docked_ ? Icon::docked : Icon::handheld, x + 13.0f, y + 28.0f, 26.0f, t.label);
            text_shrink(c, selected_session_, x + 38.0f, baseline(y + 12.0f, 32.0f, 18.0f), 18.0f, t.meta,
                        width - 38.0f);
        }
    }
    list.pop_transform();
    list.pop_opacity();

    // Its actions, as the buttons that do them.
    const Rect first{x, kDetail.y + kDetail.h - 36.0f - 68.0f - 14.0f - 76.0f, width, 76.0f};
    const Rect second{x, kDetail.y + kDetail.h - 36.0f - 68.0f, width, 68.0f};
    const Download *queued = remote ? download_of(game->key) : nullptr;
    if (remote)
    {
        primary_button(c, first, queued != nullptr ? tr("Play when downloaded") : tr("Download and play"),
                       translate_hint(Pad::cross), 0.0f, 0.0f);
        secondary_button(c, second, queued != nullptr ? tr("Cancel download") : tr("Download"),
                         translate_hint(Pad::square), 0.0f, 0.0f);
    }
    else
    {
        primary_button(c, first, tr("Play"), translate_hint(Pad::cross), 0.0f, 0.0f,
                       game != nullptr && home_.setup_ready);
        secondary_button(c, second, tr("Game settings"), translate_hint(Pad::triangle), 0.0f, 0.0f,
                         game != nullptr && game->title_id != 0);
    }

    if (remote)
    {
        // What a failed download said takes the status's place.
        const std::string failed = queued != nullptr && queued->state == DownloadState::failed ? queued->error :
                                                                                               std::string{};
        static constexpr Hint kRemote[] = {{Pad::cross, TR("Download and play")},
                                           {Pad::square, TR("Download")},
                                           {Pad::circle, TR("Back")},
                                           {Pad::l1, TR("Sections"), Pad::r1}};
        static constexpr Hint kQueued[] = {{Pad::cross, TR("Play when downloaded")},
                                           {Pad::square, TR("Cancel download")},
                                           {Pad::circle, TR("Back")},
                                           {Pad::l1, TR("Sections"), Pad::r1}};
        draw_footer(c, queued != nullptr ? kQueued : kRemote, 4, failed);
        return;
    }
    static constexpr Hint kHints[] = {{Pad::cross, TR("Play")},
                                      {Pad::triangle, TR("Game settings")},
                                      {Pad::square, TR("Mods")},
                                      {Pad::circle, TR("Back")},
                                      {Pad::l1, TR("Sections"), Pad::r1}};
    draw_footer(c, kHints, 5, count > 0 ? list_position(library_.selected + 1, count) : std::string{});
}

// ---------------------------------------------------------------- game settings dialog

int Launcher::game_row(int row) const
{
    if (row < row_save)
        return row;
    return services_.save_transfer_available() ? row : row + 1;
}

void Launcher::press_game(Key key)
{
    Game &game = games_[static_cast<std::size_t>(library_.selected)];
    const int kind = game_row(option_);
    switch (key)
    {
    case Key::circle:
        close_modal();
        selected_docked_ = game_docked_;
        // What the Library says of the game follows what was changed here.
        refresh_selected_game();
        if (game_from_home_)
        {
            // Opened from the home screen: back there.
            game_from_home_ = false;
            open(Screen::home, false);
            home_session_ = session_line(home_.last_title_id);
        }
        return;
    case Key::up:
    case Key::down:
        if (game_rows_.move(key == Key::down ? 1 : -1))
        {
            option_ = game_rows_.selected;
            message_.clear();
            import_armed_ = false;
            delete_armed_ = false;
            cue(Cue::focus);
        }
        return;
    case Key::square:
        if (kind != row_save)
            return;
        break;
    case Key::left:
    case Key::right:
    case Key::cross:
        break;
    default:
        return;
    }
    if (kind >= row_video && kind <= row_language)
    {
        // A kind of setting has its own list, in the middle of the page.
        if (key == Key::cross || key == Key::right)
            open_game_options(kind - row_video);
        return;
    }
    if (kind == row_mods)
    {
        // The game's mods have their own list. It is read again: mods may have been copied in
        // since this dialog opened.
        if (key != Key::cross && key != Key::right)
            return;
        read_mods(game);
        mod_rows_.visible = layout::kPaneRows;
        mod_rows_.pitch = layout::kPanePitch;
        mod_rows_.reset(static_cast<int>(mod_list_.size()), 0);
        modal_ = modal_shown_ = Modal::mods;
        message_.clear();
        cue(Cue::open);
        return;
    }
    if (kind == row_delete)
    {
        // Deleting a game a download source has: its file and all of its updates and DLC go, its save
        // data and settings stay. Cross asks first, then deletes.
        if (key != Key::cross)
            return;
        if (!delete_armed_)
        {
            delete_armed_ = true;
            say(tr("Press again to delete the game and its updates and DLC from the console. Save data and "
                   "settings are kept."),
                true);
            cue(Cue::notify);
            return;
        }
        delete_armed_ = false;
        std::string result;
        if (!services_.delete_game(game, &result))
        {
            say(result.empty() ? std::string{tr("Could not delete the game.")} : result, true);
            cue(Cue::error);
            return;
        }
        // On its sources only now: so it shows until the list is read again (it keeps its place).
        game.remote = true;
        game.title_id = 0;
        game.addons.clear();
        game.addons_short.clear();
        game.mods = game.mods_on = 0;
        close_modal();
        read_home();
        refresh_selected_game();
        say(result);
        cue(Cue::saved);
        return;
    }
    if (kind == row_save)
    {
        // Save data: Square copies the game's save out, Cross (twice) copies one in.
        std::string result;
        if (key == Key::square)
        {
            import_armed_ = false;
            const bool exported = services_.save_export(game.title_id, &result);
            say(result, !exported);
            cue(exported ? Cue::saved : Cue::error);
        }
        else if (key != Key::cross)
        {
            cue(Cue::error);
        }
        else if (import_source_ != SaveSource::none && !import_armed_)
        {
            // Importing replaces the save in use, so it asks first.
            import_armed_ = true;
            say(tr("Press again to replace this game's save. The current one is backed up."), true);
            cue(Cue::notify);
        }
        else
        {
            // With nothing to import the answer says where a save has to be put.
            import_armed_ = false;
            const bool imported = services_.save_import(game.title_id, &result);
            say(result, !imported && import_source_ != SaveSource::none);
            cue(imported ? Cue::saved : import_source_ != SaveSource::none ? Cue::error : Cue::notify);
        }
        return;
    }
    // The console mode.
    const bool saved = services_.set_docked(game.title_id, !game_docked_);
    if (saved)
        game_docked_ = !game_docked_;
    say(saved ? tr("Saved for this game. Applies on next launch.") : tr("Could not save. Please try again."),
        !saved);
    cue(saved ? Cue::toggle : Cue::error);
}

int Launcher::game_changes() const
{
    return changes_of(game_settings_);
}

// The page a game's settings are: its kinds at the left, the one chosen in the middle, what it
// means at the right. It lies over the Library, beside the rail.
void Launcher::draw_game_page(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    if (games_.empty())
        return;
    const Game &game = games_[static_cast<std::size_t>(library_.selected)];
    const float pane = tween::clamp01(pane_.value);
    list.push_opacity(open);
    // Its own ground over the Library: the artwork again, further back.
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, t.base);
    if (!look().high_contrast && textures_.backdrop() != 0)
        list.image(textures_.backdrop(), c.backdrop.picture(textures_), {0.0f, 0.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 0.22f});
    list.hgradient_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, t.base.with_alpha(0.55f), t.base.with_alpha(0.25f));
    list.push_transform(1.0f, 0.0f, 0.0f, (1.0f - open) * 48.0f * motion(), 0.0f);

    // ---- which game, and how much it has of its own ----
    const float hdr = t.top_nav ? 96.0f : 36.0f;
    cover(c, game.cover, {layout::kLeft, hdr, 84.0f, 84.0f}, 20.0f, 0.6f);
    kicker(c, tr("Game settings"), layout::kLeft + 108.0f, baseline(hdr + 2.0f, 28.0f, t.size_small), t.accent, 900.0f);
    const float clock = text_width(c, clock_, t.size_heading) + 28.0f;
    const std::string changed = changes_text(game_changes());
    const Tone tone = game_changes() > 0 ? Tone::accent : Tone::plain;
    const float chip_w = chip_width(c, changed, tone, 420.0f);
    chip(c, changed, layout::kRight - clock - chip_w, hdr + 32.0f, tone, 420.0f);
    {
        const Heavy heavy(c);
        text_fit(c, game.name, layout::kLeft + 108.0f, baseline(hdr + 30.0f, 54.0f, 44.0f), 44.0f, t.title,
                 layout::kRight - clock - chip_w - 32.0f - (layout::kLeft + 108.0f));
    }

    // ---- its kinds of setting ----
    const float shift = t.game_page_offset;
    const Rect kinds{kKinds.x, kKinds.y + shift, kKinds.w, kKinds.h - shift};
    const Rect middle{kMiddle.x, kMiddle.y + shift, kMiddle.w, kMiddle.h - shift};
    const Rect explain{kExplain.x, kExplain.y + shift, kExplain.w, kExplain.h - shift};
    const int rows = game_rows_.count;
    const int chosen = std::clamp(game_rows_.selected, 0, std::max(0, rows - 1));
    const int kind = game_row(chosen);
    // A kind of setting: what the game does differently from Settings, if anything.
    const auto state = [&](int which) -> std::string
    {
        switch (which)
        {
        case row_mode:
            return game_docked_ ? tr("Docked") : tr("Handheld");
        case row_mods:
            // With the game's Mods switch off (the Library's), none of them is on.
            return mods_.empty() ? std::string{tr("No mods")} :
                   !game.mods_enabled ? std::string{tr("Off")} :
                       fill(tr("{0} of {1} on"),
                            {std::to_string(std::count_if(mods_.begin(), mods_.end(),
                                                          [](const Mod &mod) { return mod.enabled; })),
                             std::to_string(mods_.size())});
        case row_save:
            return import_source_ == SaveSource::ryujinx ? tr("Ryujinx save found") :
                   import_source_ == SaveSource::folder ? tr("Save folder found") : tr("Nothing to import");
        case row_delete:
            return game.size;
        default:
            return changes_text(game_overrides(which - row_video));
        }
    };
    const auto there = [&](int which)
    {
        return which == row_mode ? true :
               which == row_mods ? !mods_.empty() && game.mods_enabled :
               which == row_save ? import_source_ != SaveSource::none :
               which == row_delete ? false : game_overrides(which - row_video) > 0;
    };
    // The highlight: whole while the focus is on the kinds, quieter once it is on the rows.
    const Rect lit{kinds.x, kinds.y + game_rows_.cursor(), kinds.w, kKindHeight};
    plate_focus(c, row_plate(), lit, 1.0f - 0.62f * pane);
    if (!look().high_contrast)
        list.rounded_rect({lit.x + 14.0f, lit.y + 20.0f, 4.0f, kKindHeight - 40.0f}, 2.0f, t.accent);
    for (int row = 0; row < rows; ++row)
    {
        const int which = game_row(row);
        const float top = kinds.y + kKindPitch * static_cast<float>(row);
        const bool on = row == chosen;
        text_shrink(c, tr(kKindNames[which]), kinds.x + 34.0f, baseline(top + 8.0f, 32.0f, t.size_text), t.size_text,
                    which == row_delete ? t.warning : on ? t.title : t.copy, kinds.w - 58.0f);
        text_shrink(c, state(which), kinds.x + 34.0f, baseline(top + 40.0f, 24.0f, 18.0f), 18.0f,
                    there(which) && which != row_mode ? t.accent : t.faint, kinds.w - 58.0f);
    }

    // ---- the kind chosen ----
    const bool active = modal_shown_ != Modal::game;
    if (kind == row_mode)
        draw_mode_pane(c, middle);
    else if (kind == row_mods)
        draw_mods_pane(c, middle, active);
    else if (kind == row_save || kind == row_delete)
        draw_save_pane(c, middle, kind == row_delete);
    else
        draw_option_pane(c, middle, kind - row_video, active);
    draw_explain_pane(c, explain, kind);

    // ---- hints ----
    const float cy = layout::kFooter;
    const auto hints = [&](const Hint *list_of, int count)
    { draw_menu_hints(c, list_of, count, layout::kLeft, cy, t.copy, layout::kWidth); };
    if (modal_shown_ == Modal::mods)
    {
        if (mods_.empty())
        {
            static constexpr Hint kEmpty[] = {{Pad::square, TR("Create the folder")}, {Pad::circle, TR("Back")}};
            hints(kEmpty, 2);
        }
        else
        {
            static constexpr Hint kMods[] = {{Pad::updown, TR("Select")},
                                             {Pad::cross, TR("Turn on or off")},
                                             {Pad::circle, TR("Back")}};
            hints(kMods, 3);
        }
    }
    else if (modal_shown_ == Modal::game_options || modal_shown_ == Modal::mapping)
    {
        // What Cross does on the focused row: nothing of its own, open the mapping, or the keyboard.
        const int action = option_action();
        if (action == 1)
        {
            static constexpr Hint kOpen[] = {
                {Pad::cross, TR("Open")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
            hints(kOpen, 3);
        }
        else if (action == 2)
        {
            static constexpr Hint kType[] = {
                {Pad::cross, TR("Edit")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
            hints(kType, 3);
        }
        else
        {
            static constexpr Hint kChange[] = {
                {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
            hints(kChange, 3);
        }
    }
    else if (kind == row_delete)
    {
        static constexpr Hint kDelete[] = {{Pad::cross, TR("Delete")}, {Pad::circle, TR("Back")}};
        hints(kDelete, 2);
    }
    else if (kind == row_save)
    {
        static constexpr Hint kTransfer[] = {{Pad::cross, TR("Import")},
                                             {Pad::square, TR("Export a copy")},
                                             {Pad::circle, TR("Back")}};
        hints(kTransfer, 3);
    }
    else if (kind != row_mode)
    {
        static constexpr Hint kOpen[] = {{Pad::updown, TR("Select")}, {Pad::cross, TR("Open")}, {Pad::circle, TR("Back")}};
        hints(kOpen, 3);
    }
    else
    {
        static constexpr Hint kHints[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        hints(kHints, 3);
    }
    list.pop_transform();
    list.pop_opacity();
}

// The console mode: the two ways a game can run, the one chosen lit.
void Launcher::draw_mode_pane(Canvas &c, const Rect &pane)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const float width = (pane.w - 20.0f) * 0.5f;
    for (int i = 0; i < 2; ++i)
    {
        const bool chosen = (i == 0) == game_docked_;
        const Rect r{pane.x + (width + 20.0f) * static_cast<float>(i), pane.y, width, 232.0f};
        if (chosen && !look().high_contrast)
            list.bordered_rect(r, t.radius_card, t.accent.with_alpha(0.14f), 1.5f, t.accent.with_alpha(0.75f));
        else if (chosen)
            list.bordered_rect(r, t.radius_card, gfx::mix(t.scrim, t.row_focus, 0.7f), 3.0f, t.title);
        else
            plate_rest(c, tile_plate(), r);
        const Color ink = chosen ? t.accent_pale : t.label;
        icon(c, i == 0 ? Icon::docked : Icon::handheld, r.x + r.w * 0.5f, r.y + 92.0f, 84.0f, ink);
        text_shrink(c, i == 0 ? tr("Docked") : tr("Handheld"), r.x + r.w * 0.5f, baseline(r.y + 160.0f, 40.0f, 28.0f),
                    28.0f, chosen ? t.title : t.copy, r.w - 48.0f, Align::center);
        if (chosen)
        {
            list.circle(r.x + r.w - 36.0f, r.y + 36.0f, 16.0f, t.accent);
            const bool contrast = look().high_contrast;
            look().high_contrast = false;
            icon(c, Icon::check, r.x + r.w - 36.0f, r.y + 36.0f, 22.0f, t.on_accent);
            look().high_contrast = contrast;
        }
    }
}

// The game's mods, each with its switch (and its cheats under it when it lists several).
void Launcher::draw_mods_pane(Canvas &c, const Rect &pane, bool active)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const Game &game = games_[static_cast<std::size_t>(library_.selected)];
    if (mods_.empty())
    {
        text_block(c,
                   fill(tr("No mods for this game yet. Copy each mod's folder to {0}, next to roms/."),
                        {services_.mods_folder(game.title_id)}),
                   pane.x + 8.0f, baseline(pane.y + 8.0f, 40.0f, t.size_text), t.size_text, 40.0f, t.body,
                   pane.w - 16.0f, 6, kShrink);
        return;
    }
    const float pitch = layout::kPanePitch;
    const float height = layout::kPaneRow;
    const float scroll = active ? mod_rows_.scroll() : 0.0f;
    const int count = static_cast<int>(mod_list_.size());
    const int first = active ? mod_rows_.first_row() : 0;
    const int last = active ? mod_rows_.last_row() : std::min(count, layout::kPaneRows) - 1;
    list.push_opacity(active ? 1.0f : 0.72f);
    list.push_clip({pane.x - 24.0f, pane.y - 8.0f, pane.w + 48.0f, pitch * (layout::kPaneRows - 1) + height + 16.0f});
    const auto row_top = [&](int row) { return pane.y + static_cast<float>(row) * pitch - scroll; };
    for (int row = first; row <= last; ++row)
    {
        list.push_opacity(active ? mod_rows_.row_alpha(row, height) : 1.0f);
        plate_rest(c, row_plate(), {pane.x, row_top(row), pane.w, height});
        list.pop_opacity();
    }
    if (active)
        plate_focus(c, row_plate(), {pane.x, pane.y + mod_rows_.cursor() - scroll, pane.w, height}, 1.0f);
    // With the game's Mods switch off (the Library's) no mod is used: their own switches keep
    // their state, drawn faint.
    const bool live = game.mods_enabled;
    const float right = pane.x + pane.w - 28.0f;
    for (int row = first; row <= last && row < count; ++row)
    {
        const ModRow at = mod_list_[static_cast<std::size_t>(row)];
        const Mod &mod = mods_[static_cast<std::size_t>(at.mod)];
        const float top = row_top(row);
        list.push_opacity(active ? mod_rows_.row_alpha(row, height) : 1.0f);
        if (at.cheat >= 0)
        {
            // One of the mod's cheats, set in under it, with its own switch: faint while its mod
            // is off.
            const Cheat &cheat = mod.cheats[static_cast<std::size_t>(at.cheat)];
            const bool used = live && mod.enabled;
            text_fit(c, cheat.name, pane.x + 68.0f, baseline(top, height, t.size_text), t.size_text,
                     cheat.enabled && used ? t.value : t.meta, pane.w - 68.0f - 120.0f);
            list.push_opacity(used ? 1.0f : 0.4f);
            toggle(c, right, top + height * 0.5f, cheat.enabled ? 1.0f : 0.0f);
            list.pop_opacity();
            list.pop_opacity();
            continue;
        }
        // Its name as the player's folder has it, what it changes under it (with how many of
        // its cheats are chosen when it lists several), its switch.
        std::string made = mod.kind;
        if (!mod.cheats.empty())
            made += ", " + fill(tr("{0} of {1} on"),
                                {std::to_string(std::count_if(mod.cheats.begin(), mod.cheats.end(),
                                                              [](const Cheat &cheat) { return cheat.enabled; })),
                                 std::to_string(mod.cheats.size())});
        text_fit(c, mod.name, pane.x + 28.0f, baseline(top + 10.0f, 36.0f, t.size_text), t.size_text,
                 mod.enabled && live ? t.value : t.meta, pane.w - 28.0f - 120.0f);
        text_shrink(c, made, pane.x + 28.0f, baseline(top + 46.0f, 26.0f, 18.0f), 18.0f, t.meta,
                    pane.w - 28.0f - 120.0f);
        list.push_opacity(live ? 1.0f : 0.4f);
        toggle(c, right, top + height * 0.5f, mod.enabled ? 1.0f : 0.0f);
        list.pop_opacity();
        list.pop_opacity();
    }
    list.pop_clip();
    if (active)
        scrollbar(c, mod_rows_, pane.x + pane.w + 14.0f, pane.y, pitch * (layout::kPaneRows - 1) + height);
    list.pop_opacity();
}

// Save data in or out, or the game off the console: what there is, and what the buttons do.
void Launcher::draw_save_pane(Canvas &c, const Rect &pane, bool removing)
{
    const Theme &t = th();
    const Game &game = games_[static_cast<std::size_t>(library_.selected)];
    const Rect r{pane.x, pane.y, pane.w, 232.0f};
    plate_rest(c, tile_plate(), r);
    icon(c, removing ? Icon::downloads : Icon::sync, r.x + 72.0f, r.y + r.h * 0.5f, 72.0f,
         removing ? t.warning : t.accent_pale);
    const float x = r.x + 144.0f;
    const float width = r.w - 144.0f - 32.0f;
    if (removing)
    {
        text_shrink(c, tr("Delete from console"), x, baseline(r.y + 70.0f, 40.0f, 28.0f), 28.0f, t.warning, width);
        text_shrink(c, game.format + "  /  " + game.size, x, baseline(r.y + 118.0f, 32.0f, 22.0f), 22.0f, t.copy,
                    width);
        return;
    }
    text_shrink(c,
                import_source_ == SaveSource::ryujinx ? tr("Ryujinx save found") :
                import_source_ == SaveSource::folder ? tr("Save folder found") : tr("Nothing to import"),
                x, baseline(r.y + 70.0f, 40.0f, 28.0f), 28.0f, import_source_ != SaveSource::none ? t.title : t.copy,
                width);
    text_shrink(c, game.name, x, baseline(r.y + 118.0f, 32.0f, 22.0f), 22.0f, t.meta, width);
}

// What the kind chosen is about, what the game has of its own in it, and what the menu last said.
void Launcher::draw_explain_pane(Canvas &c, const Rect &pane, int kind)
{
    const Theme &t = th();
    glass(c, {pane.x, pane.y, pane.w, 712.0f}, t.radius_card + 4.0f, t.panel.with_alpha(0.90f),
          t.panel_edge.with_alpha(0.40f), 0.8f);
    const float x = pane.x + 32.0f;
    const float width = pane.w - 64.0f;
    float y = pane.y + 30.0f;
    kicker(c, tr(kKindNames[kind]), x, baseline(y, 30.0f, t.size_small), kind == row_delete ? t.warning : t.accent,
           width);
    y += 48.0f;
    const bool rows_have_focus = modal_shown_ == Modal::game_options;
    const char *about = "";
    switch (kind)
    {
    case row_mode:
        about = tr("Change mode. Saved per game.");
        break;
    case row_video:
        about = tr("How games are drawn and scaled to your TV.");
        break;
    case row_performance:
        about = tr("Faster games, at some cost in accuracy.");
        break;
    case row_audio:
        about = tr("Game audio; PS5 system-menu music is unchanged.");
        break;
    case row_controls:
        about = tr("Controller shortcuts and supported features.");
        break;
    case row_language:
        about = tr("The language games use when they offer it.");
        break;
    default:
        break;
    }
    // What the focused switch does says more than what its kind is about.
    const std::string detail = rows_have_focus ? option_about(kind - row_video, option_rows_.selected) : std::string{};
    const std::string words = !detail.empty() ? detail : std::string{about};
    if (!words.empty())
    {
        const int lines = text_block(c, words, x, baseline(y, 32.0f, 22.0f), 22.0f, 32.0f, t.body, width, 7, kShrink);
        y += 32.0f * static_cast<float>(lines) + 20.0f;
    }
    if (kind >= row_video && kind <= row_language)
    {
        const int own = game_overrides(kind - row_video);
        chip(c, changes_text(own), x, y + 22.0f, own > 0 ? Tone::accent : Tone::plain, width);
    }
    // What the menu last said, at the foot.
    if (!message_.empty())
    {
        const float top = pane.y + 712.0f - 32.0f - 28.0f * 5.0f;
        c.list.rounded_rect({x, top - 18.0f, width, 1.0f}, 0.0f, t.rule.with_alpha(0.6f));
        notice_block(c, message_, x, baseline(top, 28.0f, t.size_small), t.size_small, 28.0f,
                     message_warning_ ? t.warning : t.accent_pale, width, 5, message_warning_);
    }
}

// ---------------------------------------------------------------- a game's mods

void Launcher::press_mods(Key key)
{
    Game &game = games_[static_cast<std::size_t>(library_.selected)];
    switch (key)
    {
    case Key::circle:
        // Back to the game's settings, on its Mods row.
        modal_ = modal_shown_ = Modal::game;
        message_.clear();
        cue(Cue::back);
        return;
    case Key::up:
    case Key::down:
        if (mod_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::square:
    {
        // With no mods yet: the folder they go in, named after the game's ID, is made on request.
        if (!mods_.empty())
            return;
        const bool made = services_.make_mods_folder(game.title_id);
        say(made ? fill(tr("Created {0}. Copy each mod's folder into it."),
                        {services_.mods_folder(game.title_id)}) :
                   tr("Could not create the folder. Check that the game files folder can be written."),
            !made);
        cue(made ? Cue::saved : Cue::error);
        return;
    }
    case Key::cross:
    case Key::left:
    case Key::right:
    {
        if (mods_.empty())
            return;
        const ModRow at = mod_list_[static_cast<std::size_t>(mod_rows_.selected)];
        Mod &mod = mods_[static_cast<std::size_t>(at.mod)];
        // With the game's Mods switch off (the Library's), choosing a mod turns the switch on and
        // that mod with it: nobody switches a mod in a list that is switched off.
        const bool revive = !game.mods_enabled;
        if (at.cheat >= 0)
        {
            // One of the mod's cheats. Choosing it turns on what it runs behind: its mod, and the
            // game's Mods switch. One of a group (two frame rates) takes the other's place, so
            // the list is read again.
            const Cheat &cheat = mod.cheats[static_cast<std::size_t>(at.cheat)];
            const bool enabled = revive || !mod.enabled || !cheat.enabled;
            const bool saved =
                (!revive || services_.set_mods_enabled(game.title_id, true)) &&
                (mod.enabled || services_.set_mod_enabled(game.title_id, mod.name, true)) &&
                services_.set_cheat_enabled(game.title_id, mod.name, cheat.name, enabled);
            read_mods(game);
            if (mod_rows_.count != static_cast<int>(mod_list_.size()))
                mod_rows_.reset(static_cast<int>(mod_list_.size()), 0);
            say(saved ? tr("Saved for this game. Applies on next launch.") :
                        tr("Could not save. Please try again."),
                !saved);
            cue(saved ? Cue::toggle : Cue::error);
            return;
        }
        const bool enabled = revive || !mod.enabled;
        const bool saved = (!revive || services_.set_mods_enabled(game.title_id, true)) &&
                           services_.set_mod_enabled(game.title_id, mod.name, enabled);
        if (saved)
            mod.enabled = enabled;
        count_mods(game, mods_);
        say(saved ? tr("Saved for this game. Applies on next launch.") :
                    tr("Could not save. Please try again."),
            !saved);
        cue(saved ? Cue::toggle : Cue::error);
        return;
    }
    default:
        return;
    }
}

} // namespace pe::ui
