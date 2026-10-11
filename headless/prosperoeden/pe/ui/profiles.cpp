// ProsperoEden - Launcher: the profiles (Settings > Profiles): who is playing, a new one, another
// name, one taken off the list.
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

// Placed as the button mapping is (game_options.cpp): five rows show, the list scrolls.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 334.0f;
constexpr float kRowPitch = 96.0f;
constexpr float kRowHeight = 94.0f;
constexpr int kRowsShown = 5;
constexpr Rect kWindow{592.0f, kRowsTop, 736.0f, kRowPitch * (kRowsShown - 1) + kRowHeight};
constexpr float kHints = 848.0f;
constexpr int kMostProfiles = 8;

} // namespace

void Launcher::read_profiles()
{
    profiles_ = services_.profiles();
    playing_.clear();
    for (const Profile &profile : profiles_)
        if (profile.playing)
            playing_ = profile.name;
}

int Launcher::profile_row_count() const
{
    // The profiles, then "New profile" while there is room for one.
    const int count = static_cast<int>(profiles_.size());
    return count < kMostProfiles ? count + 1 : count;
}

void Launcher::open_profiles()
{
    read_profiles();
    int playing = 0;
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        if (profiles_[i].playing)
            playing = static_cast<int>(i);
    profile_rows_.visible = kRowsShown;
    profile_rows_.pitch = kRowPitch;
    profile_rows_.reset(profile_row_count(), playing);
    profile_remove_ = -1;
    modal_ = modal_shown_ = Modal::profiles;
    message_.clear();
    cue(Cue::modal_open);
}

void Launcher::press_profiles(Key key)
{
    const int count = static_cast<int>(profiles_.size());
    const int row = std::clamp(profile_rows_.selected, 0, std::max(profile_row_count() - 1, 0));
    const bool on_profile = row < count;
    // Taking a profile off the list is asked twice; anything else forgets the first press.
    const int armed = profile_remove_;
    profile_remove_ = -1;
    switch (key)
    {
    case Key::circle:
        modal_ = Modal::none;
        message_.clear();
        cue(Cue::back);
        return;
    case Key::up:
    case Key::down:
        if (profile_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::cross:
        if (!on_profile)
        {
            const int added = services_.add_profile();
            if (added < 0)
            {
                say(tr("Could not save. Please try again."), true);
                cue(Cue::error);
                return;
            }
            read_profiles();
            profile_rows_.reset(profile_row_count(), added);
            say(tr("Added. Cross plays as this profile; left and right change its name."));
            cue(Cue::toggle);
            return;
        }
        if (profiles_[static_cast<std::size_t>(row)].playing)
        {
            say(fill(tr("Playing as {0}"), {profiles_[static_cast<std::size_t>(row)].name}));
            cue(Cue::focus);
            return;
        }
        if (services_.room_status().phase != Eden::Multiplayer::Phase::Idle) {
            services_.leave_room();
            say(tr("Leaving the room. Select the profile again when disconnected."));
            return;
        }
        if (!services_.choose_profile(row))
        {
            say(tr("Could not save. Please try again."), true);
            cue(Cue::error);
            return;
        }
        read_profiles();
        // Its settings, its game on the home screen and its recently played games are its own.
        prefs_ = services_.preferences();
        apply_look();
        read_home();
        say(fill(tr("Playing as {0}"), {playing_}));
        cue(Cue::toggle);
        return;
    case Key::left:
    case Key::right:
        if (!on_profile)
            return;
        if (!services_.rename_profile(row, key == Key::right ? 1 : -1))
        {
            say(tr("Could not save. Please try again."), true);
            cue(Cue::error);
            return;
        }
        read_profiles();
        message_.clear();
        cue(Cue::toggle);
        return;
    case Key::square:
        if (!on_profile)
            return;
        if (profiles_[static_cast<std::size_t>(row)].playing || count < 2)
        {
            say(tr("The profile that is playing stays. Choose another one first."), true);
            cue(Cue::error);
            return;
        }
        if (armed != row)
        {
            profile_remove_ = row;
            say(fill(tr("Square again takes {0} off the list. Its save data stays on the console."),
                     {profiles_[static_cast<std::size_t>(row)].name}),
                true);
            cue(Cue::notify);
            return;
        }
        if (!services_.remove_profile(row))
        {
            say(tr("Could not save. Please try again."), true);
            cue(Cue::error);
            return;
        }
        read_profiles();
        profile_rows_.reset(profile_row_count(), std::min(row, static_cast<int>(profiles_.size()) - 1));
        say(tr("Taken off the list. Its save data stays on the console."));
        cue(Cue::toggle);
        return;
    default:
        return;
    }
}

void Launcher::draw_profiles(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, 26.0f, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Profiles"), 592.0f, baseline(218.0f, 62.0f, theme::kDisplay), theme::kDisplay,
                theme::kTitle, 736.0f);
    text_shrink(c, tr("Each profile keeps its own save data, settings and recent games."), 592.0f,
                baseline(291.0f, 32.0f, theme::kSmall), theme::kSmall, Color::rgb(0xbecbb9), 736.0f);

    const int count = static_cast<int>(profiles_.size());
    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - profile_rows_.scroll(); };
    for (int row = profile_rows_.first_row(); row <= profile_rows_.last_row(); ++row)
    {
        list.push_opacity(profile_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, kRowPlate, {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, kRowPlate, {592.0f, kRowsTop + profile_rows_.cursor() - profile_rows_.scroll(), 736.0f, kRowHeight},
                1.0f);
    for (int row = profile_rows_.first_row(); row <= profile_rows_.last_row(); ++row)
    {
        const float top = row_top(row);
        list.push_opacity(profile_rows_.row_alpha(row, kRowHeight));
        if (row >= count)
        {
            // A plus, and what the row does.
            const float cx = 644.0f;
            const float cy = top + kRowHeight * 0.5f;
            list.line(cx - 10.0f, cy, cx + 10.0f, cy, 2.6f, theme::kLime);
            list.line(cx, cy - 10.0f, cx, cy + 10.0f, 2.6f, theme::kLime);
            text_shrink(c, tr("New profile"), 676.0f, baseline(top, kRowHeight, theme::kText24), theme::kText24,
                        theme::kValue, 628.0f);
        }
        else
        {
            const Profile &profile = profiles_[static_cast<std::size_t>(row)];
            // Who is playing is said at the right, in the colour of things that are on.
            float taken = 0.0f;
            if (profile.playing)
                taken = text_shrink(c, tr("Playing"), 1296.0f, baseline(top, kRowHeight, theme::kSmall),
                                    theme::kSmall, theme::kLime, 220.0f, Align::right);
            text_shrink(c, profile.name, 628.0f, baseline(top, kRowHeight, theme::kText24), theme::kText24,
                        theme::kValue, 668.0f - taken - 28.0f);
        }
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, profile_rows_, 1340.0f, kWindow.y, kWindow.h);

    if (!message_.empty())
    {
        notice_block(c, message_, 592.0f, kHints - 6.0f, theme::kSmall, 26.0f,
                     message_warning_ ? theme::kWarning : theme::kLimePale, 736.0f, 2, message_warning_);
    }
    else if (profile_rows_.selected >= count)
    {
        static constexpr Hint kHintsRow[] = {{Pad::cross, TR("Add")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kHintsRow, 2, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    else
    {
        static constexpr Hint kHintsRow[] = {{Pad::cross, TR("Play as")},
                                             {Pad::leftright, TR("Rename")},
                                             {Pad::square, TR("Remove")},
                                             {Pad::circle, TR("Back")}};
        draw_hints(c, kHintsRow, 4, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
