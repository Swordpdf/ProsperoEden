// ProsperoEden - Launcher: the save sync. When the profile playing keeps its save data on a server
// (save-sync.json), a game's save data is synced before it starts (the save sync dialog: the game
// starts once it is in step) and after it ended (once the menu is back, with a notice at the top
// right). When both sides changed since they were last the same, the dialog asks which one stays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

// While it works: the download dialog's panel and ring (remote.cpp).
constexpr Rect kPanel{560.0f, 196.0f, 800.0f, 688.0f};
constexpr float kCenterX = 960.0f;
constexpr float kRingY = 384.0f;
constexpr float kRingRadius = 96.0f;
constexpr float kRingWidth = 10.0f;
constexpr float kPi = 3.14159265f;
// A question (a conflict, a failure): the dialog of Settings, its answers as rows.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 480.0f;
constexpr float kRowPitch = 112.0f;
constexpr float kRowHeight = 102.0f;
constexpr float kHints = 848.0f;
constexpr float kPollSeconds = 0.2f;
constexpr float kNoticeSeconds = 6.0f;

// A time as the player reads it, on the console's clock; empty when not known.
std::string when(std::int64_t seconds)
{
    if (seconds <= 0)
        return {};
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr)
        return {};
    char text[64]{};
    (void)std::strftime(text, sizeof(text), tr("%Y-%m-%d %H:%M"), &local);
    return text;
}

// The answers the dialog offers: to a conflict, to a failure before the game starts, and to a
// server too old for the save sync (confirmed, before the game or after it).
enum class Answer : std::uint8_t
{
    keep_console,
    take_server,
    change_nothing,
    play_anyway,
    try_again,
    back,
    understood,
};

std::vector<Answer> answers(const SaveSync &sync, bool launching)
{
    if (sync.stage == SaveSyncStage::conflict)
        return {Answer::keep_console, Answer::take_server, Answer::change_nothing};
    if (sync.stage == SaveSyncStage::failed && sync.too_old)
        return launching ? std::vector<Answer>{Answer::play_anyway, Answer::back} : std::vector<Answer>{Answer::understood};
    if (sync.stage == SaveSyncStage::failed && launching)
        return {Answer::play_anyway, Answer::try_again, Answer::back};
    return {};
}

bool asks(const SaveSync &sync, bool launching)
{
    return !answers(sync, launching).empty();
}

} // namespace

void Launcher::poll_save_sync(float dt)
{
    sync_time_ += dt;
    if (sync_notice_left_ > 0.0f)
        sync_notice_left_ = std::max(0.0f, sync_notice_left_ - dt);
    sync_notice_in_.target = sync_notice_left_ > 0.4f ? 1.0f : 0.0f;
    sync_notice_in_.update(dt, 14.0f);

    save_sync_wait_ += dt;
    if (save_sync_wait_ < kPollSeconds)
        return;
    save_sync_wait_ = 0.0f;
    const SaveSync was = save_sync_;
    save_sync_ = services_.save_sync();

    // The end of a sync after a game: a notice, unless nothing changed. A server too old for the
    // save sync is a dialog instead, to be confirmed.
    if (!save_sync_.before && save_sync_.stage == SaveSyncStage::failed && save_sync_.too_old)
    {
        if (modal_ == Modal::none && selected_game_.empty() && !transition_.running)
        {
            sync_launch_ = false;
            modal_ = modal_shown_ = Modal::save_sync;
            message_.clear();
            cue(Cue::modal_open);
        }
        else if (sync_launch_)
        {
            // Another game is about to start: a notice, as other ends of a sync after a game (the
            // game's own sync meets the same server and says it in its dialog).
            sync_notice_ = save_sync_;
            sync_notice_left_ = kNoticeSeconds;
            cue(Cue::error);
            services_.end_save_sync();
            save_sync_ = services_.save_sync();
        }
    }
    // A sync before a game the player went back from, once it stopped.
    else if (save_sync_.before && !sync_launch_ &&
             (save_sync_.stage == SaveSyncStage::done || save_sync_.stage == SaveSyncStage::failed))
    {
        services_.end_save_sync();
        save_sync_ = services_.save_sync();
    }
    else if (!save_sync_.before &&
             (save_sync_.stage == SaveSyncStage::done || save_sync_.stage == SaveSyncStage::failed))
    {
        if (save_sync_.stage == SaveSyncStage::failed || save_sync_.outcome != SaveSyncOutcome::same)
        {
            sync_notice_ = save_sync_;
            sync_notice_left_ = kNoticeSeconds;
            cue(save_sync_.stage == SaveSyncStage::failed ? Cue::error : Cue::notify);
        }
        services_.end_save_sync();
        save_sync_ = services_.save_sync();
        if (modal_ == Modal::save_sync && !sync_launch_)
            close_modal();
    }

    if (sync_launch_ && modal_ == Modal::save_sync)
    {
        // The game's own sync, once the one after the last game (it may be the same game) is over.
        if (!sync_started_ && !(save_sync_.stage == SaveSyncStage::working || save_sync_.stage == SaveSyncStage::conflict))
        {
            services_.start_save_sync(sync_file_, true);
            sync_started_ = true;
            save_sync_ = services_.save_sync();
        }
        if (sync_started_ && save_sync_.before && save_sync_.stage == SaveSyncStage::done)
        {
            services_.end_save_sync();
            save_sync_ = services_.save_sync();
            sync_launch_ = false;
            modal_ = Modal::none;
            start_game(sync_file_, sync_title_, sync_cover_);
            return;
        }
    }
    // A conflict after a game asks once the menu is free.
    else if (save_sync_.stage == SaveSyncStage::conflict && modal_ == Modal::none && selected_game_.empty() &&
             !transition_.running)
    {
        sync_launch_ = false;
        modal_ = modal_shown_ = Modal::save_sync;
        message_.clear();
        cue(Cue::modal_open);
    }
    // A question: its answers, the first in focus.
    const int count = static_cast<int>(answers(save_sync_, sync_launch_).size());
    if (count > 0 && (save_sync_.stage != was.stage || sync_rows_.count != count))
    {
        sync_rows_.visible = 3;
        sync_rows_.pitch = kRowPitch;
        sync_rows_.wrap = false;
        sync_rows_.reset(count, 0);
        cue(Cue::notify);
    }
}

void Launcher::press_save_sync(Key key)
{
    // While it works: wait, or go back without the game (the sync stops, nothing more changes).
    const std::vector<Answer> offered = answers(save_sync_, sync_launch_);
    if (offered.empty())
    {
        if (key == Key::circle && sync_launch_)
        {
            services_.stop_save_sync();
            sync_launch_ = false;
            cue(Cue::back);
            close_modal();
        }
        return;
    }
    switch (key)
    {
    case Key::up:
    case Key::down:
        if (sync_rows_.move(key == Key::down ? 1 : -1))
            cue(Cue::focus);
        return;
    case Key::circle:
    case Key::cross:
    {
        // Circle: the last answer (change nothing, back, understood).
        const int row = key == Key::circle ? static_cast<int>(offered.size()) - 1 :
                                             std::clamp(sync_rows_.selected, 0, static_cast<int>(offered.size()) - 1);
        const Answer answer = offered[static_cast<std::size_t>(row)];
        cue(key == Key::circle ? Cue::back : Cue::select);
        if (answer == Answer::keep_console || answer == Answer::take_server || answer == Answer::change_nothing)
        {
            services_.choose_save_data(answer == Answer::keep_console ? SaveChoice::console :
                                       answer == Answer::take_server  ? SaveChoice::server :
                                                                        SaveChoice::neither);
            save_sync_.stage = SaveSyncStage::working; // until the next look says otherwise
            sync_time_ = 0.0f;
            return;
        }
        services_.end_save_sync();
        save_sync_ = services_.save_sync();
        if (answer == Answer::try_again)
        {
            services_.start_save_sync(sync_file_, true);
            save_sync_ = services_.save_sync();
            sync_time_ = 0.0f;
            return;
        }
        const bool launching = sync_launch_;
        sync_launch_ = false;
        if (answer == Answer::play_anyway && launching)
        {
            modal_ = Modal::none;
            start_game(sync_file_, sync_title_, sync_cover_);
            return;
        }
        close_modal();
        return;
    }
    default:
        return;
    }
}

void Launcher::draw_save_sync(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.04f * (1.0f - open) * motion(), kCenterX, 540.0f, 0.0f,
                        (1.0f - open) * 30.0f * motion());
    const std::string &game = save_sync_.game.empty() ? sync_title_ : save_sync_.game;
    const auto centred = [&](std::string_view value, float top, float line, float size, Color color)
    { text_shrink(c, value, kCenterX, baseline(top, line, size), size, color, kPanel.w - 96.0f, Align::center); };

    if (!asks(save_sync_, sync_launch_))
    {
        // It works: the game's cover in a turning ring.
        glass(c, kPanel, 28.0f, th().panel.with_alpha(0.97f), th().accent.with_alpha(0.38f), 1.8f);
        list.circle(kCenterX, kRingY, kRingRadius - kRingWidth, th().base.with_alpha(0.55f));
        list.ring(kCenterX, kRingY, kRingRadius, kRingWidth, th().panel_edge.with_alpha(0.22f));
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, sync_time_ * 4.2f,
            kPi * (0.55f + 0.45f * std::sin(sync_time_ * 2.1f)), th().accent);
        const float art = (kRingRadius - kRingWidth) * 2.0f * 0.70710678f;
        if (sync_launch_)
            cover(c, sync_cover_, {kCenterX - art * 0.5f, kRingY - art * 0.5f, art, art}, 12.0f);
        centred(game, 506.0f, 46.0f, th().size_heading, th().title);
        centred(save_sync_.before || sync_launch_ ? tr("Syncing save data") : tr("Backing up save data"), 560.0f,
                32.0f, th().size_text, th().value);
        if (!save_sync_.profile.empty())
            centred(fill(tr("Profile {0}"), {save_sync_.profile}), 606.0f, 28.0f, th().size_small, th().meta);
        centred(sync_launch_ ? tr("The game starts when its save data is in step.") :
                               tr("Your save data is checked against the server's."),
                706.0f, 30.0f, th().size_small, th().copy);
        if (sync_launch_)
        {
            static constexpr Hint kWaiting[] = {{Pad::circle, TR("Back")}};
            draw_menu_hints(c, kWaiting, 1, kPanel.x + 52.0f, kPanel.y + kPanel.h - 54.0f, th().copy, kPanel.w - 104.0f);
        }
        list.pop_transform();
        list.pop_opacity();
        return;
    }

    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f), 1.6f);
    const bool conflict = save_sync_.stage == SaveSyncStage::conflict;
    const bool too_old = !conflict && save_sync_.too_old;
    text_shrink(c, conflict ? tr("Save data changed on both sides") :
                   too_old  ? tr("Server too old for save sync") :
                              tr("Save data not synced"),
                592.0f, baseline(218.0f, 62.0f, th().size_display), th().size_display, th().title, 736.0f);
    text_fit(c, save_sync_.profile.empty() ? game : fill(tr("{0} · Profile {1}"), {game, save_sync_.profile}), 592.0f,
             baseline(291.0f, 32.0f, th().size_small), th().size_small, th().copy, 736.0f);
    notice_block(c,
                 conflict ? std::string{tr("This console and the server both have save data the other does not know. "
                                           "Which one should stay? The other is kept as a backup.")} :
                 too_old  ? fill(tr("The server runs RomM {0}; the save sync needs RomM {1} or newer. Update the "
                                    "server: until then no save data is synced with it."),
                                 {save_sync_.server_version, save_sync_.needed_version}) :
                            save_sync_.error,
                 592.0f, baseline(332.0f, 32.0f, th().size_small), th().size_small, 32.0f,
                 conflict ? th().copy : th().warning, 736.0f, 3, false);

    // The answers.
    const std::vector<Answer> offered = answers(save_sync_, sync_launch_);
    const std::string console = when(save_sync_.console_time);
    std::string server = when(save_sync_.server_time);
    if (!save_sync_.server_device.empty())
        server = server.empty() ? save_sync_.server_device : fill(tr("{0} on {1}"), {server, save_sync_.server_device});
    const auto row_top = [&](int row) { return kRowsTop + static_cast<float>(row) * kRowPitch; };
    for (std::size_t row = 0; row < offered.size(); ++row)
        plate_rest(c, row_plate(), {592.0f, row_top(static_cast<int>(row)), 736.0f, kRowHeight});
    plate_focus(c, row_plate(), {592.0f, kRowsTop + sync_rows_.cursor(), 736.0f, kRowHeight}, 1.0f);
    for (std::size_t row = 0; row < offered.size(); ++row)
    {
        std::string title;
        std::string note;
        switch (offered[row])
        {
        case Answer::keep_console:
            title = tr("Keep this console's");
            note = console.empty() ? tr("It goes to the server.") : fill(tr("Saved {0}. It goes to the server."), {console});
            break;
        case Answer::take_server:
            title = tr("Take the server's");
            note = server.empty() ? tr("It replaces this console's.") : fill(tr("Saved {0}. It replaces this console's."), {server});
            break;
        case Answer::change_nothing:
            title = tr("Change nothing");
            note = sync_launch_ && save_sync_.before ? tr("Play with this console's; you are asked again next time.") :
                                  tr("You are asked again next time.");
            break;
        case Answer::play_anyway:
            title = tr("Play anyway");
            note = tr("With this console's save data; it is synced after the game.");
            if (too_old)
                note = tr("With this console's save data; it is not synced.");
            break;
        case Answer::try_again:
            title = tr("Try again");
            note = tr("Sync the save data once more.");
            break;
        case Answer::back:
            title = tr("Back");
            note = tr("The game does not start.");
            break;
        case Answer::understood:
            title = tr("Understood");
            note = tr("You are told again at the next sync.");
            break;
        }
        const float top = row_top(static_cast<int>(row));
        text_fit(c, title, 628.0f, baseline(top + 16.0f, 38.0f, th().size_text), th().size_text, th().value, 664.0f);
        text_fit(c, note, 628.0f, baseline(top + 56.0f, 30.0f, th().size_small), th().size_small, th().meta, 664.0f);
    }

    static constexpr Hint kAnswers[] = {
        {Pad::updown, TR("Select")}, {Pad::cross, TR("Choose")}, {Pad::circle, TR("Back")}};
    static constexpr Hint kConflict[] = {
        {Pad::updown, TR("Select")}, {Pad::cross, TR("Choose")}, {Pad::circle, TR("Change nothing")}};
    static constexpr Hint kConfirm[] = {{Pad::cross, TR("Understood")}};
    if (offered.size() == 1)
        draw_menu_hints(c, kConfirm, 1, 592.0f, kHints, th().copy, 736.0f);
    else
        draw_menu_hints(c, conflict ? kConflict : kAnswers, 3, 592.0f, kHints, th().copy, 736.0f);
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::draw_save_sync_notice(Canvas &c)
{
    const float shown = tween::clamp01(sync_notice_in_.value);
    if (shown <= 0.01f)
        return;
    gfx::DrawList &list = c.list;
    // Top right, as the update notice; under it when that shows too.
    const float top = update_notice_in_.value > 0.01f ? 164.0f : 44.0f;
    const Rect panel{1352.0f, top, 520.0f, 108.0f};
    const bool failed = sync_notice_.stage == SaveSyncStage::failed;
    const bool fine = !failed && (sync_notice_.outcome == SaveSyncOutcome::uploaded ||
                                  sync_notice_.outcome == SaveSyncOutcome::downloaded);
    const Color accent = fine ? th().accent : th().warning;
    list.push_opacity(shown);
    list.push_transform(1.0f, 0.0f, 0.0f, (1.0f - shown) * 72.0f * motion(), 0.0f);
    glass(c, panel, 20.0f, th().panel.with_alpha(0.97f), accent.with_alpha(0.55f), 1.4f);
    // A mark at the left: a disc with an arrow up (to the server) or down (from it).
    const float cx = panel.x + 48.0f;
    const float cy = panel.y + 50.0f;
    const float tip = sync_notice_.outcome == SaveSyncOutcome::downloaded ? 1.0f : -1.0f;
    list.circle(cx, cy, 20.0f, accent.with_alpha(0.22f));
    list.line(cx, cy - 9.0f * tip, cx, cy + 9.0f * tip, 2.6f, accent);
    list.line(cx - 8.0f, cy + 2.0f * tip, cx, cy + 10.0f * tip, 2.6f, accent);
    list.line(cx + 8.0f, cy + 2.0f * tip, cx, cy + 10.0f * tip, 2.6f, accent);
    std::string title;
    std::string line = sync_notice_.game;
    if (failed)
    {
        title = tr("Save data not backed up");
        line = tr("It is tried again when the menu opens next.");
    }
    else if (sync_notice_.outcome == SaveSyncOutcome::uploaded)
        title = tr("Save data backed up");
    else if (sync_notice_.outcome == SaveSyncOutcome::downloaded)
        title = tr("Save data from the server");
    else if (sync_notice_.outcome == SaveSyncOutcome::no_game)
    {
        title = tr("Save data not backed up");
        line = fill(tr("The server does not have {0}."), {sync_notice_.game});
    }
    else
        title = tr("Save data left as it was");
    text_shrink(c, title, panel.x + 88.0f, baseline(panel.y + 18.0f, 34.0f, th().size_text), th().size_text,
                th().title, panel.w - 112.0f);
    text_shrink(c, line, panel.x + 88.0f, baseline(panel.y + 54.0f, 30.0f, th().size_small), th().size_small, th().copy,
                panel.w - 112.0f);
    const float left = tween::clamp01(sync_notice_left_ / kNoticeSeconds);
    list.rounded_rect({panel.x + 20.0f, panel.y + panel.h - 12.0f, (panel.w - 40.0f) * left, 3.0f}, 1.5f,
                      accent.with_alpha(0.8f));
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
