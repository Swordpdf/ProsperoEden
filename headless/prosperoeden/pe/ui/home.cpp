// ProsperoEden - Launcher home: the game to continue, its cover, the controllers, recent games.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

// What has the focus: the main action, the game's settings, the recent games, "view all".
constexpr int kMain = 0;
constexpr int kGameSettings = 1;
constexpr int kFirstRecent = 2;
constexpr int kRecentsCards = 4;  // Canopy: glass tile cards (fixed by card_rect layout)
constexpr int kRecentsIcons = 6;  // Signal minimal: small cover icons (fits the left column)
constexpr int kRecents = kRecentsIcons;
constexpr int kViewAll = kFirstRecent + kRecents;

// The library grid on the home screen shares these with library.cpp.
constexpr int kGridColumns = 4;

// The words at the left, the cover at the right.
constexpr float kWords = 1120.0f;
constexpr Rect kCover{1384.0f, 176.0f, 440.0f, 440.0f};
constexpr float kHeroLine = 96.0f; // the title's line height
// The shelf of recent games.
constexpr float kShelfTop = 780.0f;
constexpr float kCardGap = 20.0f;
constexpr float kCardWidth = (layout::kWidth - kCardGap * 3.0f) / 4.0f;
constexpr float kCardHeight = 128.0f;

Rect card_rect(int index)
{
    return {layout::kLeft + (kCardWidth + kCardGap) * static_cast<float>(index), kShelfTop + 48.0f, kCardWidth,
            kCardHeight};
}

// The four controllers, under the cover.
Rect pad_rect(int player)
{
    constexpr float kPad = 72.0f;
    const float gap = (kCover.w - kPad * 4.0f) / 3.0f;
    return {kCover.x + (kPad + gap) * static_cast<float>(player), 668.0f, kPad, 50.0f};
}

} // namespace

void Launcher::open_game_settings_from_home(const std::string &file)
{
    // The page belongs to the Library's list: the Library opens at that game, its page over it.
    open(Screen::library, true);
    enter_library();
    for (int i = 0; i < static_cast<int>(games_.size()); ++i)
    {
        if (games_[static_cast<std::size_t>(i)].file != file)
            continue;
        library_.reset(library_.count, i);
        refresh_selected_game();
        sync_grid(true);
        press_library(Key::triangle);
        game_from_home_ = modal_ == Modal::game;
        return;
    }
    // Not in the list any more: the Library shows what there is.
}

void Launcher::press_home(Key key)
{
    const bool ready = home_.setup_ready;
    const bool minimal = th().minimal_home;
    const int count = static_cast<int>(games_.size());

    if (minimal && home_on_grid_)
    {
        const auto go = [&](int next)
        {
            library_.selected = next;
            refresh_selected_game();
            sync_grid(false);
            cue(Cue::focus);
        };
        switch (key)
        {
        case Key::up:
        case Key::down:
        {
            if (count == 0)
                return;
            int next = library_.selected + (key == Key::down ? kGridColumns : -kGridColumns);
            if (next >= count && library_.selected / kGridColumns < (count - 1) / kGridColumns)
                next = count - 1;
            if (next >= 0 && next < count)
                go(next);
            return;
        }
        case Key::left:
            if (count == 0 || library_.selected % kGridColumns == 0)
            {
                home_on_grid_ = false;
                cue(Cue::focus);
                return;
            }
            go(library_.selected - 1);
            return;
        case Key::right:
            if (count > 0 && library_.selected % kGridColumns != kGridColumns - 1 && library_.selected + 1 < count)
                go(library_.selected + 1);
            return;
        case Key::cross:
        {
            if (count == 0 || !ready)
            {
                cue(Cue::error);
                return;
            }
            press_ = 1.0f;
            const Game &game = games_[static_cast<std::size_t>(library_.selected)];
            if (game.remote)
            {
                queue_remote(game);
                return;
            }
            launch(game.file, game.name, game.cover);
            return;
        }
        case Key::triangle:
        {
            if (count == 0)
                return;
            const Game &game = games_[static_cast<std::size_t>(library_.selected)];
            if (game.remote || game.title_id == 0)
            {
                cue(Cue::error);
                return;
            }
            game_settings_ = services_.game_settings(game.title_id);
            game_docked_ = selected_docked_;
            game_from_home_ = true;
            import_source_ = services_.save_transfer_available() ?
                                 services_.save_import_source(game.title_id) : SaveSource::none;
            import_armed_ = false;
            delete_armed_ = false;
            read_mods(games_[static_cast<std::size_t>(library_.selected)]);
            open_modal(Modal::game);
            pane_.snap(0.0f);
            game_rows_.visible = 11;
            game_rows_.pitch = game_kind_pitch();
            game_rows_.reset(dialog_rows(Modal::game), 0);
            return;
        }
        case Key::square:
        {
            if (count == 0)
                return;
            const Game &game = games_[static_cast<std::size_t>(library_.selected)];
            if (game.remote)
            {
                queue_remote(game);
                return;
            }
            if (game.title_id == 0 || game.mods == 0)
            {
                cue(Cue::error);
                return;
            }
            Game &chosen = games_[static_cast<std::size_t>(library_.selected)];
            const bool saved = services_.set_mods_enabled(chosen.title_id, !chosen.mods_enabled);
            if (saved)
                count_mods(chosen, game_mods(chosen.title_id));
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

    if (minimal)
    {
        const bool has_game = count > 0;
        const Game *game = has_game ? &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
        switch (key)
        {
        case Key::up:
            if (home_focus_ == kGameSettings)
            {
                home_focus_ = kMain;
                cue(Cue::focus);
            }
            return;
        case Key::down:
            if (home_focus_ == kMain && has_game && game->title_id != 0)
            {
                home_focus_ = kGameSettings;
                cue(Cue::focus);
            }
            return;
        case Key::right:
            if (count > 0)
            {
                home_on_grid_ = true;
                cue(Cue::focus);
            }
            return;
        case Key::left:
            return;
        case Key::cross:
            press_ = 1.0f;
            if (home_focus_ == kMain && has_game && !game->remote)
                launch(game->file, game->name, game->cover);
            else if (home_focus_ == kMain && !ready)
                go_section(3);
            else if (home_focus_ == kGameSettings && has_game && game->title_id != 0)
                open_game_settings_from_home(game->file);
            else
                cue(Cue::error);
            return;
        case Key::triangle:
            if (has_game && !game->remote && game->title_id != 0)
                open_game_settings_from_home(game->file);
            else
                cue(Cue::error);
            return;
        default:
            return;
        }
    }

    const bool continue_ready = ready && home_.last_exists;
    const bool recents_shown = home_.status.empty() && ready;
    const int max_recents = kRecentsCards;
    const int recent_count = recents_shown ? std::min(max_recents, static_cast<int>(home_.recents.size())) : 0;
    const int before = home_focus_;
    int &focus = home_focus_;

    switch (key)
    {
    case Key::up:
        if (focus >= kFirstRecent)
            focus = kMain;
        break;
    case Key::down:
        if (focus <= kGameSettings && recents_shown)
            focus = recent_count > 0 ? kFirstRecent : kViewAll;
        break;
    case Key::left:
        if (focus == kGameSettings)
            focus = kMain;
        else if (focus == kMain || focus == kFirstRecent || (focus == kViewAll && recent_count == 0))
            return focus_rail();
        else if (focus == kViewAll)
            focus = kFirstRecent + recent_count - 1;
        else
            focus -= 1;
        break;
    case Key::right:
        if (focus == kMain)
        {
            if (continue_ready)
                focus = kGameSettings;
        }
        else if (focus >= kFirstRecent && focus < kViewAll)
        {
            focus = focus - kFirstRecent + 1 < recent_count ? focus + 1 : kViewAll;
        }
        break;
    case Key::triangle:
        if (focus <= kGameSettings && continue_ready)
            open_game_settings_from_home(home_.last_file);
        else if (focus >= kFirstRecent && focus < kFirstRecent + recent_count)
            open_game_settings_from_home(home_.recents[static_cast<std::size_t>(focus - kFirstRecent)].file);
        else
            cue(Cue::error);
        return;
    case Key::cross:
        press_ = 1.0f;
        if (focus == kMain && continue_ready)
        {
            launch_from_ = kCover;
            launch(home_.last_file, home_.last_title, home_.last_cover);
        }
        else if (focus == kMain)
            go_section(ready ? 1 : 3);
        else if (focus == kGameSettings && continue_ready)
            open_game_settings_from_home(home_.last_file);
        else if (focus == kViewAll && ready)
            go_section(1);
        else if (focus >= kFirstRecent && focus < kFirstRecent + recent_count)
        {
            const Recent &recent = home_.recents[static_cast<std::size_t>(focus - kFirstRecent)];
            const Rect card = card_rect(focus - kFirstRecent);
            launch_from_ = {card.x + 16.0f, card.y + 16.0f, 96.0f, 96.0f};
            launch(recent.file, recent.title, recent.cover);
        }
        else
            cue(Cue::error);
        return;
    default:
        return;
    }
    if (focus != before)
        cue(Cue::focus);
}

void Launcher::update_controllers(float dt)
{
    const unsigned now = services_.controllers() & 0xfu;
    bool joined = false;
    bool left = false;
    for (int player = 0; player < 4; ++player)
    {
        const std::size_t index = static_cast<std::size_t>(player);
        const bool on = (now >> player & 1u) != 0;
        controller_lit_[index].target = on ? 1.0f : 0.0f;
        if (!controllers_known_)
        {
            controller_lit_[index].snap(controller_lit_[index].target);
        }
        else if (on != ((controllers_ >> player & 1u) != 0))
        {
            (on ? joined : left) = true;
            if (on)
                controller_pop_[index] = 1.0f;
        }
        controller_lit_[index].update(dt, 10.0f);
        controller_pop_[index] = std::max(0.0f, controller_pop_[index] - dt / 0.5f);
    }
    // One sound however many changed at once; none while a game is starting.
    if ((joined || left) && selected_game_.empty())
        cue(joined ? Cue::saved : Cue::modal_close);
    controllers_ = now;
    controllers_known_ = true;
}

void Launcher::draw_controllers(Canvas &c)
{
    if (textures_.controller() == 0)
        return;
    const Theme &t = th();
    kicker(c, tr("CONTROLLERS"), kCover.x, baseline(632.0f, 30.0f, t.size_small), t.label, kCover.w);
    for (int player = 0; player < 4; ++player)
    {
        const std::size_t index = static_cast<std::size_t>(player);
        const Rect r = pad_rect(player);
        const float lit = tween::clamp01(controller_lit_[index].value);
        // A controller that joins lands with a small bounce.
        const float pop = controller_pop_[index];
        const float bounce = std::sin(3.14159265f * (1.0f - pop)) * pop * motion();
        c.list.push_transform(1.0f + 0.34f * bounce, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f,
                              -5.0f * bounce);
        controller_icon(c, r, lit);
        c.list.pop_transform();
        const char number[] = {static_cast<char>('1' + player), 0};
        text(c, number, r.x + r.w * 0.5f, baseline(722.0f, 26.0f, t.size_small), t.size_small,
             gfx::mix(t.faint.with_alpha(0.6f), t.accent, lit), Align::center);
    }
}

void Launcher::draw_home_status(Canvas &c)
{
    if (home_.status.empty())
        return;
    const Theme &t = th();
    const bool over_library = t.top_nav && (screen_ == Screen::home || screen_ == Screen::library);
    const float sx = over_library ? layout::kPanelLeft : layout::kLeft;
    const float sy = over_library ?
                         layout::kPanelTop + (layout::kPanelBottom - layout::kPanelTop - 164.0f) * 0.5f :
                         (t.minimal_home ? 960.0f : kShelfTop + 12.0f);
    const float sw = over_library ? layout::kPanelWidth :
                                    (t.minimal_home ? 520.0f : layout::kWidth);
    const Rect scard{sx, sy, sw, 164.0f};
    const Color accent = home_.launch_failed ? t.warning : t.accent;
    glass(c, scard, t.radius_card, t.panel.with_alpha(0.78f), accent.with_alpha(0.45f), 0.6f);
    notice_block(c, home_.status, scard.x + 32.0f, baseline(scard.y + 26.0f, 36.0f, t.size_text), t.size_text, 36.0f,
                 t.text, scard.w - 64.0f, 3, home_.launch_failed);
}

void Launcher::draw_home(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const bool ready = home_.setup_ready;
    const bool continue_ready = ready && home_.last_exists;
    const bool minimal = t.minimal_home;
    const auto focus = [&](int index) { return home_springs_[static_cast<std::size_t>(index)].value; };
    const auto arrive = [&](int part)
    { return tween::cubic_out((intro_ - 0.10f - t.stagger * static_cast<float>(part)) / 0.55f); };
    const auto begin_part = [&](int part, float rise, float slide = 0.0f)
    {
        const float e = arrive(part);
        list.push_opacity(e);
        list.push_transform(1.0f, 0.0f, 0.0f, (1.0f - e) * slide * motion(), (1.0f - e) * rise * motion());
    };
    const auto end_part = [&]
    {
        list.pop_transform();
        list.pop_opacity();
    };
    const float left = layout::kLeft;
    const float words = minimal ? 520.0f : kWords;

    // ---- top bar: the app, its version, who plays; the room joined ----
    // (Skipped with top_nav: the section tabs already fill this row.)
    if (!t.top_nav)
    {
        begin_part(0, -16.0f);
        {
            const float line = baseline(layout::kTop, 64.0f, t.size_text);
            float x = left + text(c, "PROSPEROEDEN", left, line, t.size_text, t.title, Align::left, 7.0f) + 28.0f;
            x += text(c, version_, x, line, t.size_small, t.muted);
            if (profiles_.size() > 1 && !playing_.empty())
                text_shrink(c, "/  " + playing_, x + 16.0f, line, t.size_small, t.accent_pale, 320.0f);
            if (services_.room_status().phase == Eden::Multiplayer::Phase::Connected)
            {
                const std::string room = std::string{tr("Multiplayer")} + ": " + tr("Connected");
                const float clock = text_width(c, clock_, t.size_heading) + 28.0f;
                const float width = chip_width(c, room, Tone::accent, 520.0f);
                chip(c, room, layout::kRight - clock - width, layout::kTop + 32.0f, Tone::accent, 520.0f);
            }
        }
        end_part();
    }

    // ---- the selected game: a detail card like Library's, on the left side ----
    if (minimal)
    {
        const int count = static_cast<int>(games_.size());
        const Game *game = count > 0 ? &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
        const bool has_game = game != nullptr && !game->remote;
        constexpr float kCardTop = 110.0f;
        constexpr float kCardHeight = 836.0f;
        const Rect card{left, kCardTop, words, kCardHeight};
        const float cx = left + 36.0f;
        const float cw = words - 72.0f;
        begin_part(1, 22.0f);
        glass(c, card, t.radius_panel, t.panel.with_alpha(0.90f), t.panel_edge.with_alpha(0.42f), 1.2f);
        {
            const Rect band{card.x + 1.0f, card.y + 1.0f, card.w - 2.0f, 148.0f};
            const std::string &cover_path = has_game ? game->cover : home_.last_cover;
            if (!cover_path.empty())
            {
                const Cover picture = c.textures.cover(cover_path, card.w);
                if (picture.texture != 0)
                {
                    const Rect tall{band.x, band.y, band.w, band.h + 60.0f};
                    list.push_clip(band);
                    list.rounded_image(picture.texture, tall, {0.0f, 0.27f, 1.0f, tall.h / tall.w},
                                       t.radius_panel - 1.0f, Color{1.0f, 1.0f, 1.0f, 0.9f});
                    list.pop_clip();
                }
                const Color ground = look().high_contrast ? gfx::mix(t.scrim, t.panel, 0.4f) :
                                                            t.panel.with_alpha(0.97f);
                list.gradient_rect({band.x, band.y + 40.0f, band.w, band.h - 39.0f}, 0.0f,
                                   ground.with_alpha(0.0f), ground);
            }
            const float shown = tween::clamp01(detail_.value);
            list.push_opacity(shown);
            kicker(c, tr("GAME DETAILS"), cx, baseline(card.y + 162.0f, 30.0f, t.size_small), t.accent, cw);
            float y = card.y + 198.0f;
            {
                const Heavy heavy(c);
                const std::string &name = has_game ? game->name :
                    (home_.last_file.empty() ? std::string{tr("No ROM selected")} : home_.last_title);
                const int lines = text_block(c, name, cx, baseline(y, 46.0f, t.size_lead), t.size_lead,
                                             46.0f, t.title, cw, 2, 0.8f);
                y += 46.0f * static_cast<float>(std::max(1, lines)) + 8.0f;
            }
            if (has_game)
            {
                text_shrink(c, game->format + "  /  " + game->size, cx, baseline(y, 30.0f, 22.0f), 22.0f, t.copy, cw);
                y += 30.0f;
                text_fit(c, game->file, cx, baseline(y, 26.0f, 18.0f), 18.0f, t.faint, cw);
                y += 38.0f;
            }
            if (has_game)
            {
                float ax = cx;
                const auto add_chip = [&](const std::string &w, Tone tone)
                {
                    const float room = cx + cw - ax;
                    if (w.empty() || (ax > cx && chip_width(c, w, tone, 2000.0f) > room))
                        return;
                    ax += chip(c, w, ax, y + 22.0f, tone, room) + 10.0f;
                };
                add_chip(game->addons_short.empty() ? game->addons : game->addons_short, Tone::plain);
                add_chip(game->language, game->language_note.empty() ? Tone::plain : Tone::warning);
                y += 56.0f;
                if (!game->language_note.empty())
                {
                    notice(c, game->language_note, cx, baseline(y, 26.0f, 18.0f), 18.0f, t.warning, cw, false);
                    y += 30.0f;
                }
            }
            if (has_game)
            {
                const bool can_configure = game->title_id != 0;
                const bool has_mods = can_configure && game->mods > 0;
                struct Line { const char *name; std::string value; Color color; };
                const Line lines[] = {
                    {tr("Console mode"),
                     can_configure ? std::string{selected_docked_ ? tr("Docked") : tr("Handheld")} :
                                     std::string{tr("Unavailable")},
                     can_configure ? t.value : t.meta},
                    {tr("Mods"),
                     !has_mods ? std::string{can_configure ? tr("No mods") : tr("Unavailable")} :
                     game->mods_enabled ? fill(tr("{0} of {1} on"),
                         {std::to_string(game->mods_on), std::to_string(game->mods)}) :
                         std::string{tr("Off")},
                     has_mods && game->mods_enabled ? t.accent_pale : t.meta},
                    {tr("Game settings"),
                     can_configure ? (selected_changes_ > 0 ?
                         fill(tr("{0} changed"), {std::to_string(selected_changes_)}) :
                         std::string{tr("Follows Settings")}) : std::string{tr("Unavailable")},
                     selected_changes_ > 0 ? t.accent_pale : t.meta},
                };
                for (const Line &line : lines)
                {
                    const float vw = text_shrink(c, line.value, cx + cw, baseline(y, 46.0f, t.size_small),
                                                 t.size_small, line.color, cw * 0.6f, Align::right);
                    text_shrink(c, line.name, cx, baseline(y, 46.0f, t.size_small), t.size_small, t.label,
                                cw - vw - 20.0f);
                    list.rounded_rect({cx, y + 45.0f, cw, 1.0f}, 0.0f, t.rule.with_alpha(0.45f));
                    y += 46.0f;
                }
                if (!selected_session_.empty() && y + 44.0f <= card.y + card.h - 36.0f - 68.0f - 14.0f - 76.0f - 8.0f)
                {
                    icon(c, selected_docked_ ? Icon::docked : Icon::handheld, cx + 13.0f, y + 28.0f, 26.0f, t.label);
                    text_shrink(c, selected_session_, cx + 38.0f, baseline(y + 12.0f, 32.0f, 18.0f), 18.0f, t.meta,
                                cw - 38.0f);
                }
            }
            list.pop_opacity();
        }
        {
            const Rect first{cx, card.y + card.h - 36.0f - 68.0f - 14.0f - 76.0f, cw, 76.0f};
            const Rect second{cx, card.y + card.h - 36.0f - 68.0f, cw, 68.0f};
            primary_button(c, first, tr("Play"), translate_hint(Pad::cross), focus(kMain), press_,
                           has_game && ready);
            secondary_button(c, second, tr("Game settings"), translate_hint(Pad::triangle),
                             focus(kGameSettings), press_, has_game && game->title_id != 0);
        }
        end_part();
    }
    else
    {
        const float hero_top = 180.0f;
        const float hero_line = kHeroLine;
        begin_part(1, 26.0f);
        kicker(c, tr("CONTINUE PLAYING"), left, baseline(hero_top, 30.0f, t.size_small), t.accent, words);
        int lines = 1;
        {
            const Heavy heavy(c);
            const float title_top = hero_top + 36.0f;
            lines = text_block(c, home_.last_file.empty() ? tr("Your next adventure") : home_.last_title, left,
                               baseline(title_top, hero_line, t.size_hero), t.size_hero, hero_line, t.title, words, 2,
                               0.62f);
        }
        end_part();
        const float below = hero_top + 36.0f + hero_line * static_cast<float>(std::max(1, lines));

        begin_part(2, 22.0f);
        {
            std::vector<std::pair<std::string, Tone>> chips;
            chips.emplace_back(home_.last_file.empty() ? std::string{tr("Choose a game from your library.")} :
                                                          home_.last_caption,
                               home_.last_caption_warning ? Tone::warning : Tone::plain);
            if (!home_.last_language.empty())
            {
                if (!home_.last_addons.empty())
                    chips.emplace_back(home_.last_addons, Tone::plain);
                if (home_.last_mods > 0)
                    chips.emplace_back(addons_line({}, home_.last_mods, home_.last_mods_on), Tone::plain);
                chips.emplace_back(std::string{tr("Language")} + ": " + home_.last_language, Tone::plain);
            }
            float x = left;
            const float chip_y = below + 50.0f;
            for (const auto &[text_words, tone] : chips)
            {
                if (text_words.empty())
                    continue;
                const float room = left + words - x;
                if (x > left && chip_width(c, text_words, tone, 2000.0f) > room)
                    break;
                x += chip(c, text_words, x, chip_y, tone, room) + 12.0f;
            }
        }
        end_part();

        begin_part(3, 22.0f);
        {
            const char *first = continue_ready ? tr("Play") : ready ? tr("Open library") : tr("Settings");
            const char *second = tr("Game settings");
            const float top = below + 112.0f;
            const float first_width = std::clamp(text_width(c, first, 28.0f) + 152.0f, 160.0f, 520.0f);
            const float second_width = std::clamp(text_width(c, second, 28.0f) + 140.0f, 160.0f, 520.0f);
            primary_button(c, {left, top, first_width, 88.0f}, first, translate_hint(Pad::cross), focus(kMain), press_);
            secondary_button(c, {left + first_width + 12.0f, top, second_width, 88.0f}, second,
                             translate_hint(Pad::triangle),
                             focus(kGameSettings), press_, continue_ready);
            if (continue_ready && !home_session_.empty())
            {
                const float line = baseline(top + 116.0f, 30.0f, 22.0f);
                icon(c, home_docked_ ? Icon::docked : Icon::handheld, left + 15.0f, top + 131.0f, 30.0f, t.label);
                text_shrink(c, home_session_, left + 44.0f, line, 22.0f, t.meta, words - 44.0f);
            }
        }
        end_part();

        begin_part(2, 0.0f, 40.0f);
        {
            const float drift = 6.0f * std::sin(c.time * 0.7f) * motion();
            const Rect art{kCover.x, kCover.y + drift, kCover.w, kCover.h};
            if (!look().high_contrast)
                list.shadow({art.x - 20.0f, art.y, art.w + 40.0f, art.h + 40.0f}, 60.0f, 120.0f,
                            t.highlight.with_alpha(0.14f));
            cover(c, home_.last_cover, art, t.radius_panel, 2.2f);
            list.bordered_rect(art, t.radius_panel, t.accent_pale.with_alpha(0.0f), 1.0f, t.accent_pale.with_alpha(0.30f));
            draw_controllers(c);
        }
        end_part();
    }

    // ---- recently played (Canopy only), or what needs attention ----
    begin_part(5, 26.0f);
    if (!home_.status.empty())
    {
        if (!th().top_nav)
            draw_home_status(c);
    }
    else if (!minimal)
    {
        kicker(c, tr("RECENTLY PLAYED"), left, baseline(kShelfTop, 30.0f, t.size_small), t.accent_pale, 900.0f);
        {
            const float f = focus(kViewAll);
            const float width = text_width(c, tr("VIEW ALL GAMES"), t.size_small, 3.0f) + 48.0f;
            const Rect r{layout::kRight - width + 24.0f, kShelfTop - 6.0f, width, 42.0f};
            list.push_opacity(ready ? 1.0f : 0.4f);
            plate(c, nav_plate(), r, f);
            text(c, tr("VIEW ALL GAMES"), r.x + r.w - 24.0f, baseline(r.y, r.h, t.size_small), t.size_small,
                 gfx::mix(t.label, t.title, f), Align::right, 3.0f);
            list.pop_opacity();
        }
        if (home_.recents.empty())
            text(c, tr("Games you launch will appear here."), left, baseline(kShelfTop + 64.0f, 36.0f, t.size_text),
                 t.size_text, t.muted);
        for (int i = 0; i < static_cast<int>(home_.recents.size()) && i < kRecentsCards; ++i)
        {
            const Recent &recent = home_.recents[static_cast<std::size_t>(i)];
            const Rect r = card_rect(i);
            const float f = focus(kFirstRecent + i);
            const float e = tween::cubic_out((intro_ - 0.10f - t.stagger * (5.0f + static_cast<float>(i))) / 0.5f);
            list.push_opacity(e);
            const float scale = 1.0f + (0.03f * f - 0.035f * press_ * f) * motion();
            list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, (1.0f - e) * 18.0f * motion());
            plate(c, tile_plate(), r, f);
            cover(c, recent.cover, {r.x + 16.0f, r.y + 16.0f, 96.0f, 96.0f}, 14.0f, 0.5f);
            text_block(c, recent.title, r.x + 132.0f, baseline(r.y + 32.0f, 32.0f, t.size_text), t.size_text, 32.0f,
                       gfx::mix(t.body, t.title, f), r.w - 132.0f - 20.0f, 2);
            list.pop_transform();
            list.pop_opacity();
        }
    }
    end_part();

    // ---- hints (only when home is the active section; other sections draw their own) ----
    if (screen_ == Screen::home)
    {
        begin_part(6, 0.0f);
        if (minimal && home_on_grid_)
        {
            static constexpr Hint kGridHints[] = {{Pad::cross, TR("Play")},
                                                  {Pad::triangle, TR("Game settings")},
                                                  {Pad::square, TR("Mods")},
                                                  {Pad::l1, TR("Sections"), Pad::r1}};
            draw_footer(c, kGridHints, 4, home_.system_status);
        }
        else
        {
            static constexpr Hint kHints[] = {{Pad::cross, TR("Select")},
                                              {Pad::triangle, TR("Game settings")},
                                              {Pad::l1, TR("Sections"), Pad::r1}};
            draw_footer(c, kHints, 3, home_.system_status);
        }
        end_part();
    }
}

} // namespace pe::ui
