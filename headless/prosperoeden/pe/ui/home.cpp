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
constexpr int kViewAll = 6;
constexpr int kRecents = 4;

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
    const bool continue_ready = ready && home_.last_exists;
    // The status panel takes the place of the recent games.
    const bool recents_shown = home_.status.empty() && ready;
    const int recent_count = recents_shown ? std::min(kRecents, static_cast<int>(home_.recents.size())) : 0;
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
        // The settings of the game the focus is on: the one to continue, or a recent one.
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

void Launcher::draw_home(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const bool ready = home_.setup_ready;
    const bool continue_ready = ready && home_.last_exists;
    const auto focus = [&](int index) { return home_springs_[static_cast<std::size_t>(index)].value; };
    // Each part of the screen arrives a moment after the one before it.
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

    // ---- top bar: the app, its version, who plays; the room joined ----
    begin_part(0, -16.0f);
    {
        const float line = baseline(layout::kTop, 64.0f, t.size_text);
        float x = left + text(c, "PROSPEROEDEN", left, line, t.size_text, t.title, Align::left, 7.0f) + 28.0f;
        x += text(c, version_, x, line, t.size_small, t.muted);
        // Who is playing is named once there is more than one profile to be.
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

    // ---- the game to continue ----
    begin_part(1, 26.0f);
    kicker(c, tr("CONTINUE PLAYING"), left, baseline(180.0f, 30.0f, t.size_small), t.accent, kWords);
    int lines = 1;
    {
        const Heavy heavy(c);
        lines = text_block(c, home_.last_file.empty() ? tr("Your next adventure") : home_.last_title, left,
                           baseline(222.0f, kHeroLine, t.size_hero), t.size_hero, kHeroLine, t.title, kWords, 2,
                           0.62f);
    }
    end_part();
    const float below = 222.0f + kHeroLine * static_cast<float>(std::max(1, lines));

    // What is known of it, as chips: its state, what it comes with, its language.
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
        for (const auto &[words, tone] : chips)
        {
            if (words.empty())
                continue;
            // A chip that has no room left is not shown; the first always is.
            const float room = left + kWords - x;
            if (x > left && chip_width(c, words, tone, 2000.0f) > room)
                break;
            x += chip(c, words, x, below + 50.0f, tone, room) + 12.0f;
        }
    }
    end_part();

    // ---- its actions ----
    begin_part(3, 22.0f);
    {
        const char *first = continue_ready ? tr("Play") : ready ? tr("Open library") : tr("Settings");
        const char *second = tr("Game settings");
        const float top = below + 112.0f;
        const float first_width = std::clamp(text_width(c, first, 28.0f) + 152.0f, 240.0f, 520.0f);
        const float second_width = std::clamp(text_width(c, second, 28.0f) + 140.0f, 240.0f, 520.0f);
        primary_button(c, {left, top, first_width, 88.0f}, first, translate_hint(Pad::cross), focus(kMain), press_);
        secondary_button(c, {left + first_width + 20.0f, top, second_width, 88.0f}, second,
                         translate_hint(Pad::triangle),
                         focus(kGameSettings), press_, continue_ready);
        // What it runs with.
        if (continue_ready && !home_session_.empty())
        {
            const float line = baseline(top + 116.0f, 30.0f, 22.0f);
            icon(c, home_docked_ ? Icon::docked : Icon::handheld, left + 15.0f, top + 131.0f, 30.0f, t.label);
            text_shrink(c, home_session_, left + 44.0f, line, 22.0f, t.meta, kWords - 44.0f);
        }
    }
    end_part();

    // ---- its cover, floating over the artwork; the controllers under it ----
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

    // ---- recently played, or what needs attention ----
    begin_part(5, 26.0f);
    if (!home_.status.empty())
    {
        const Rect card{left, kShelfTop + 12.0f, layout::kWidth, 164.0f};
        const Color accent = home_.launch_failed ? t.warning : t.accent;
        glass(c, card, t.radius_card, t.panel.with_alpha(0.78f), accent.with_alpha(0.45f), 0.6f);
        notice_block(c, home_.status, left + 32.0f, baseline(card.y + 26.0f, 36.0f, t.size_text), t.size_text, 36.0f,
                     t.text, layout::kWidth - 64.0f, 3, home_.launch_failed);
    }
    else
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
        for (int i = 0; i < static_cast<int>(home_.recents.size()) && i < kRecents; ++i)
        {
            const Recent &recent = home_.recents[static_cast<std::size_t>(i)];
            const Rect r = card_rect(i);
            const float f = focus(kFirstRecent + i);
            // The cards come one after the other.
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

    // ---- hints ----
    begin_part(6, 0.0f);
    static constexpr Hint kHints[] = {{Pad::cross, TR("Select")},
                                      {Pad::triangle, TR("Game settings")},
                                      {Pad::l1, TR("Sections"), Pad::r1}};
    draw_footer(c, kHints, 3, home_.system_status);
    end_part();
}

} // namespace pe::ui
