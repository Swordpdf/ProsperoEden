// ProsperoEden - The launcher's shell: the section rail, the shade over the artwork, the clock, and
// the title and hints every screen has.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

constexpr Rect kScreen{0.0f, 0.0f, 1920.0f, 1080.0f};
constexpr float kRailWide = 372.0f; // the rail with its sections' names
constexpr float kTile = 72.0f;
constexpr float kTilePitch = 90.0f;
constexpr float kTileLeft = (layout::kRail - kTile) * 0.5f;

struct Section
{
    Icon icon;
    const char *name;
};
constexpr Section kSectionList[] = {{Icon::home, TR("Home")},
                                    {Icon::library, TR("Library")},
                                    {Icon::multiplayer, TR("Multiplayer")},
                                    {Icon::settings, TR("Settings")},
                                    {Icon::about, TR("About")}};

float tile_top(int index)
{
    constexpr float kAll = kTilePitch * 4.0f + kTile;
    return 540.0f - kAll * 0.5f + kTilePitch * static_cast<float>(index);
}

std::string pick(const std::vector<std::string> &values, int index)
{
    return index >= 0 && index < static_cast<int>(values.size()) ? values[static_cast<std::size_t>(index)] :
                                                                   std::string{};
}

} // namespace

int Launcher::section() const
{
    switch (screen_)
    {
    case Screen::home:
        return 0;
    case Screen::library:
        return 1;
    case Screen::multiplayer:
        return 2;
    case Screen::about:
        return 4;
    default: // Settings, and its own pages: game files, language
        return 3;
    }
}

bool Launcher::at_section() const
{
    if (modal_ != Modal::none || !selected_game_.empty())
        return false;
    if (screen_ == Screen::multiplayer)
        return room_page_ == RoomPage::choice && !room_edit_.valid();
    return screen_ == Screen::home || screen_ == Screen::library || screen_ == Screen::settings ||
           screen_ == Screen::about;
}

void Launcher::go_section(int target)
{
    target = std::clamp(target, 0, kSections - 1);
    const int from = section();
    const bool forward = target > from;
    rail_focus_ = false;
    switch (target)
    {
    case 0:
        if (screen_ == Screen::home)
            return;
        open(Screen::home, forward);
        home_session_ = session_line(home_.last_title_id);
        return;
    case 1:
        if (screen_ == Screen::library)
            return;
        open(Screen::library, forward);
        enter_library();
        return;
    case 2:
        if (screen_ == Screen::multiplayer)
            return;
        open(Screen::multiplayer, forward);
        enter_multiplayer();
        return;
    case 3:
        if (screen_ == Screen::settings)
            return;
        // From one of its own pages (game files, language) it is the way back.
        open(Screen::settings, from != 3 && forward);
        prefs_ = services_.preferences();
        section_.snap(1.0f);
        return;
    default:
        if (screen_ == Screen::about)
            return;
        open(Screen::about, forward);
        return;
    }
}

float Launcher::arrive(int index, float step) const
{
    if (look().reduce_motion || !drawing_shown_)
        return 1.0f;
    return tween::cubic_out((screen_time_ - 0.06f - step * static_cast<float>(index)) / 0.36f);
}

void Launcher::focus_rail()
{
    rail_focus_ = true;
    rail_index_ = section();
    message_.clear();
    cue(Cue::focus);
}

void Launcher::press_rail(Key key)
{
    switch (key)
    {
    case Key::up:
    case Key::down:
    {
        const int next = std::clamp(rail_index_ + (key == Key::down ? 1 : -1), 0, kSections - 1);
        if (next != rail_index_)
        {
            rail_index_ = next;
            cue(Cue::focus);
        }
        return;
    }
    case Key::l1:
    case Key::r1:
        rail_index_ = (rail_index_ + (key == Key::r1 ? 1 : kSections - 1)) % kSections;
        cue(Cue::focus);
        return;
    case Key::cross:
    case Key::right:
        if (rail_index_ == section())
        {
            rail_focus_ = false;
            cue(Cue::focus);
            return;
        }
        press_ = 1.0f;
        go_section(rail_index_);
        return;
    case Key::circle:
    case Key::left:
        rail_focus_ = false;
        cue(Cue::back);
        return;
    default:
        return;
    }
}

// The artwork shows where a screen leaves it room: the home screen keeps its right side clear, the
// others lie over an even shade.
void Launcher::draw_scrims(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Color shade = th().base;
    const float away = tween::clamp01(dim_.value);
    const float home = 1.0f - away;
    if (home > 0.01f)
    {
        list.hgradient_rect({0.0f, 0.0f, 653.0f, 1080.0f}, 0.0f, shade.with_alpha(0.94f * home),
                            shade.with_alpha(0.78f * home));
        list.hgradient_rect({653.0f, 0.0f, 538.0f, 1080.0f}, 0.0f, shade.with_alpha(0.78f * home),
                            shade.with_alpha(0.16f * home));
        list.hgradient_rect({1191.0f, 0.0f, 729.0f, 1080.0f}, 0.0f, shade.with_alpha(0.16f * home),
                            shade.with_alpha(0.30f * home));
        list.gradient_rect({0.0f, 640.0f, 1920.0f, 440.0f}, 0.0f, shade.with_alpha(0.0f),
                           shade.with_alpha(0.90f * home));
    }
    if (away > 0.01f)
        list.hgradient_rect(kScreen, 0.0f, shade.with_alpha(0.92f * away), shade.with_alpha(0.76f * away));
}

void Launcher::draw_rail(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const float open = tween::clamp01(rail_open_.value);
    const float width = layout::kRail + (kRailWide - layout::kRail) * open;

    // Its body: opens over the screen beside it when the focus is on it.
    if (open > 0.01f)
        list.shadow({0.0f, 0.0f, width, 1080.0f}, 0.0f, 60.0f, Color{0.0f, 0.0f, 0.0f, 0.5f * open});
    if (look().high_contrast)
        list.rounded_rect({0.0f, 0.0f, width, 1080.0f}, 0.0f, gfx::mix(t.scrim, t.panel, 0.4f));
    else
        list.gradient_rect({0.0f, 0.0f, width, 1080.0f}, 0.0f, t.panel.with_alpha(0.90f + 0.08f * open),
                           t.base.with_alpha(0.94f + 0.05f * open));
    list.rounded_rect({width - 1.0f, 0.0f, 1.0f, 1080.0f}, 0.0f, t.panel_edge.with_alpha(0.22f + 0.2f * open));

    // During the opening the mark is on its way here (draw_opening).
    if (textures_.brand() != 0 && !(opening_on_ && !look().reduce_motion))
    {
        const Rect mark{(layout::kRail - 64.0f) * 0.5f, 40.0f, 64.0f, 64.0f};
        list.shadow({mark.x + 2.0f, mark.y + 10.0f, mark.w - 4.0f, mark.h - 4.0f}, 18.0f, 22.0f,
                    Color{0.0f, 0.0f, 0.0f, 0.5f});
        list.rounded_image(textures_.brand(), mark, {0.0f, 0.0f, 1.0f, 1.0f}, 18.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (open > 0.01f)
    {
        list.push_opacity(open);
        text(c, "PROSPEROEDEN", 112.0f, baseline(40.0f, 64.0f, t.size_small), t.size_small, t.title, Align::left,
             4.0f);
        list.pop_opacity();
    }

    for (int i = 0; i < kSections; ++i)
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float active = tween::clamp01(rail_active_[index].value);
        const float focus = tween::clamp01(rail_springs_[index].value);
        const Rect tile{kTileLeft, tile_top(i), kTile + (width - layout::kRail), kTile};
        const float dip = 1.0f - 0.04f * press_ * focus * motion();
        list.push_transform(dip, tile.x + kTile * 0.5f, tile.y + kTile * 0.5f, 0.0f, 0.0f);
        // The section shown keeps a quiet plate of the accent; the highlight is the ring.
        if (active > 0.01f && !look().high_contrast)
            list.bordered_rect(tile, 20.0f, t.accent.with_alpha(0.16f * active), 1.0f,
                               t.accent.with_alpha(0.55f * active));
        else if (active > 0.01f)
            list.bordered_rect(tile, 20.0f, gfx::mix(t.scrim, t.row_focus, 0.7f).with_alpha(active), 2.0f,
                               t.accent.with_alpha(active));
        plate_focus(c, nav_plate(), tile, focus);
        const Color ink = gfx::mix(gfx::mix(t.label, t.accent, active), t.title, focus * (1.0f - active));
        icon(c, kSectionList[i].icon, tile.x + kTile * 0.5f, tile.y + kTile * 0.5f, 32.0f, ink);
        if (open > 0.01f)
        {
            list.push_opacity(open);
            text_shrink(c, tr(kSectionList[i].name), tile.x + kTile + 8.0f, baseline(tile.y, kTile, t.size_text),
                        t.size_text, gfx::mix(t.copy, t.title, std::max(active, focus)),
                        kRailWide - (tile.x + kTile + 8.0f) - 40.0f);
            list.pop_opacity();
        }
        list.pop_transform();
    }

    // Who is playing.
    const float cx = layout::kRail * 0.5f;
    const float cy = 1010.0f;
    list.circle(cx, cy, 30.0f, gfx::mix(t.accent_deep, t.row, 0.45f));
    list.ring(cx, cy, 29.5f, 1.2f, t.accent.with_alpha(0.5f));
    std::string initial;
    if (!playing_.empty())
    {
        std::size_t end = 0;
        (void)gfx::next_codepoint(playing_, &end);
        initial = playing_.substr(0, end);
        if (fonts_.font == nullptr || !fonts_.font->can_draw(initial))
            initial.clear();
    }
    if (initial.empty())
    {
        icon(c, Icon::profile, cx, cy, 30.0f, t.accent_pale);
    }
    else
    {
        const Heavy heavy(c);
        text(c, initial, cx, cy + t.size_text * 0.35f, t.size_text, t.accent_pale, Align::center);
    }
    if (open > 0.01f && !playing_.empty())
    {
        list.push_opacity(open);
        text_fit(c, playing_, 112.0f, cy + t.size_small * 0.35f, t.size_small, t.copy, kRailWide - 112.0f - 32.0f);
        list.pop_opacity();
    }
}

void Launcher::draw_clock(Canvas &c)
{
    text(c, clock_, layout::kRight, baseline(layout::kTop, 64.0f, th().size_heading), th().size_heading,
         th().title, Align::right);
}

void Launcher::draw_frame(Canvas &c, const char *title, const char *copy)
{
    const Theme &t = th();
    const float line = baseline(layout::kTop, 64.0f, t.size_display);
    const float clock = text_width(c, clock_, t.size_heading) + 48.0f;
    const float room = layout::kWidth - clock;
    float used = 0.0f;
    {
        const Heavy heavy(c);
        used = text_shrink(c, title, layout::kLeft, line, t.size_display, t.title, room * 0.6f);
    }
    if (copy != nullptr && copy[0] != 0)
        text_shrink(c, copy, layout::kLeft + used + 24.0f, line, 22.0f, t.muted, room - used - 24.0f);
}

void Launcher::draw_footer(Canvas &c, const Hint *hints, int count, std::string_view status)
{
    const Theme &t = th();
    const float line = layout::kFooter + t.size_small * 0.35f;
    // What the menu last said takes the status's place (a dialog says its own).
    float taken = 0.0f;
    if (!message_.empty() && modal_shown_ == Modal::none)
        taken = notice(c, message_, layout::kRight, line, t.size_small,
                       message_warning_ ? t.warning : t.accent_pale, 900.0f, message_warning_, Align::right);
    else if (!status.empty())
        taken = text_shrink(c, status, layout::kRight, line, t.size_small, t.muted, 700.0f, Align::right);
    draw_hints(c, hints, count, layout::kLeft, layout::kFooter, t.copy,
               layout::kWidth - (taken > 0.0f ? taken + 48.0f : 0.0f));
}

const char *Launcher::output_name(int output)
{
    static constexpr const char *kOutputs[] = {"1080p", "1440p", "2160p"};
    return kOutputs[std::clamp(output, 0, 2)];
}

std::string Launcher::session_line(std::uint64_t title_id)
{
    if (title_id == 0)
        return {};
    // (Asked for the home screen's game: its mode is kept for the mark before the line.)
    const bool docked = services_.docked(title_id);
    if (title_id == home_.last_title_id)
        home_docked_ = docked;
    return session_line(services_.game_settings(title_id), docked);
}

std::string Launcher::session_line(const GameSettings &game, bool docked)
{
    std::string line = docked ? tr("Docked") : tr("Handheld");
    const auto add = [&line](const std::string &part)
    {
        if (!part.empty())
            line += "  /  " + part;
    };
    add(pick(services_.resolution_keys(), game.resolution >= 0 ? game.resolution : prefs_.resolution));
    add(pick(services_.filter_labels(), game.filter >= 0 ? game.filter : prefs_.filter));
    add(output_name(prefs_.output));
    add(hertz(game.refresh >= 0 ? game.refresh : prefs_.refresh));
    const bool generated = game.frame_gen >= 0 ? game.frame_gen != 0 : prefs_.frame_gen;
    const int renderer = game.renderer >= 0 ? game.renderer : prefs_.renderer;
    if (generated && renderer != 0 && services_.frame_gen_state() == 2)
        add(tr("Frame generation"));
    return line;
}

} // namespace pe::ui
