// ProsperoEden - Launcher: Settings > Save sync. Each profile with the server it keeps its save
// data on (save-sync.json). Cross pairs one with a server without typing: the server shows in a
// QR code and a short code, the player approves it on the phone signed in as the profile's user,
// and the console gets a sign-in of its own. Square, twice, unlinks a profile.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace pe::ui
{

using audio::Cue;

namespace
{

// Settings' dialog, its rows under the title.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 384.0f;
constexpr float kRowPitch = 96.0f;
constexpr float kRowHeight = 94.0f;
constexpr int kRowsShown = 4;
constexpr Rect kWindow{592.0f, kRowsTop, 736.0f, kRowPitch * (kRowsShown - 1) + kRowHeight};
constexpr float kHints = 848.0f;
// The pairing dialog: the QR code at the left, what to do at the right.
constexpr Rect kPairPanel{360.0f, 180.0f, 1200.0f, 720.0f};
constexpr Rect kQrBox{408.0f, 340.0f, 400.0f, 400.0f};
constexpr float kPollSeconds = 0.25f;
constexpr float kRefreshSeconds = 2.0f;

// A code as it is easier to read: in two halves.
std::string spaced(const std::string &code)
{
    return code.size() == 8 ? code.substr(0, 4) + " " + code.substr(4) : code;
}

std::string minutes(int seconds)
{
    const int left = std::max(0, seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", left / 60, left % 60);
    return text;
}

} // namespace

void Launcher::open_sync_setup()
{
    sync_setup_ = services_.save_sync_setup();
    pair_servers_ = services_.pair_servers();
    sync_setup_choosing_ = false;
    unlink_armed_ = -1;
    sync_setup_rows_.visible = kRowsShown;
    sync_setup_rows_.pitch = kRowPitch;
    sync_setup_rows_.wrap = false;
    sync_setup_rows_.reset(static_cast<int>(sync_setup_.profiles.size()), 0);
    open_modal(Modal::sync_setup);
}

void Launcher::refresh_sync_setup(float dt)
{
    // While Settings shows the save sync's summary, it follows save-sync.json (edited over FTP).
    sync_setup_wait_ += dt;
    if (sync_setup_wait_ < kRefreshSeconds || screen_ != Screen::settings || modal_ != Modal::none)
        return;
    sync_setup_wait_ = 0.0f;
    sync_setup_ = services_.save_sync_setup();
}

void Launcher::start_pairing(int profile, int server)
{
    sync_setup_profile_ = profile;
    pair_server_ = server;
    if (!services_.start_pairing(profile, server))
    {
        // The last one may still be ending; else the profile or the server is gone (sources.json or
        // the profiles changed meanwhile): back to the list as it is now.
        const PairingStage stage = services_.pairing().stage;
        const bool running = stage == PairingStage::asking || stage == PairingStage::waiting;
        if (modal_ == Modal::pairing)
            press_pairing(Key::circle);
        say(running ? tr("A pairing is running already.") : tr("The profile or the server is no longer there."), true);
        cue(Cue::error);
        return;
    }
    pairing_ = services_.pairing();
    pairing_time_ = 0.0f;
    pairing_wait_ = 0.0f;
    pairing_qr_ = {};
    pairing_qr_text_.clear();
    modal_ = modal_shown_ = Modal::pairing;
    message_.clear();
    cue(Cue::modal_open);
}

void Launcher::press_sync_setup(Key key)
{
    const int profiles = static_cast<int>(sync_setup_.profiles.size());
    switch (key)
    {
    case Key::up:
    case Key::down:
        unlink_armed_ = -1;
        if (sync_setup_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::circle:
        if (sync_setup_choosing_)
        {
            // Back to the profiles.
            sync_setup_choosing_ = false;
            sync_setup_rows_.reset(profiles, sync_setup_profile_);
            cue(Cue::back);
            return;
        }
        close_modal();
        return;
    case Key::cross:
    {
        const int row = sync_setup_rows_.selected;
        if (sync_setup_choosing_)
            return start_pairing(sync_setup_profile_, row);
        if (row < 0 || row >= profiles)
            return;
        if (!sync_setup_.error.empty())
        {
            say(tr("save-sync.json cannot be read: correct it over FTP first."), true);
            cue(Cue::error);
            return;
        }
        if (pair_servers_.empty())
        {
            say(tr("No server to pair with: add a RomM server to sources.json, or set save-sync.json up over FTP."), true);
            cue(Cue::error);
            return;
        }
        if (pair_servers_.size() == 1)
            return start_pairing(row, 0);
        // Which server, when several can pair.
        sync_setup_choosing_ = true;
        sync_setup_profile_ = row;
        sync_setup_rows_.reset(static_cast<int>(pair_servers_.size()), 0);
        cue(Cue::select);
        return;
    }
    case Key::square:
    {
        const int row = sync_setup_rows_.selected;
        if (sync_setup_choosing_ || row < 0 || row >= profiles || !sync_setup_.profiles[static_cast<std::size_t>(row)].linked)
            return;
        if (unlink_armed_ != row)
        {
            unlink_armed_ = row;
            say(fill(tr("Square again unlinks {0}. Its save data stays on the console and on the server."),
                     {sync_setup_.profiles[static_cast<std::size_t>(row)].name}));
            cue(Cue::notify);
            return;
        }
        unlink_armed_ = -1;
        if (services_.unlink_profile(row))
        {
            sync_setup_ = services_.save_sync_setup();
            say(tr("Unlinked. Its save data is no longer synced."));
            cue(Cue::select);
        }
        else
        {
            say(tr("Could not change save-sync.json."), true);
            cue(Cue::error);
        }
        return;
    }
    default:
        return;
    }
}

void Launcher::draw_sync_setup(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f), 1.6f);
    const bool choosing = sync_setup_choosing_;
    const std::string chosen = sync_setup_profile_ < static_cast<int>(sync_setup_.profiles.size()) ?
                                   sync_setup_.profiles[static_cast<std::size_t>(sync_setup_profile_)].name :
                                   std::string{};
    text_shrink(c, choosing ? fill(tr("Pair {0} with"), {chosen}) : std::string{tr("Save sync")}, 592.0f,
                baseline(218.0f, 62.0f, th().size_display), th().size_display, th().title, 736.0f);
    const bool said = !message_.empty() && message_age_ < 6.0f;
    notice_block(c,
                 said             ? message_ :
                 !sync_setup_.error.empty() ? fill(tr("save-sync.json cannot be read ({0}). Nothing syncs until it is "
                                                     "corrected over FTP."),
                                                  {sync_setup_.error}) :
                 choosing         ? std::string{tr("Several servers can keep the save data. Which one should this "
                                                  "profile use?")} :
                                    std::string{tr("Each profile keeps its save data on a server, as its own user "
                                                   "there. Pair one with Cross: no typing, your phone does the "
                                                   "signing in.")},
                 592.0f, baseline(296.0f, 32.0f, th().size_small), th().size_small, 32.0f,
                 said && message_warning_ ? th().warning : !sync_setup_.error.empty() ? th().warning : th().copy,
                 736.0f, 3, false);

    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - sync_setup_rows_.scroll(); };
    for (int row = sync_setup_rows_.first_row(); row <= sync_setup_rows_.last_row(); ++row)
    {
        list.push_opacity(sync_setup_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, row_plate(), {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    if (sync_setup_rows_.count > 0)
        plate_focus(c, row_plate(),
                    {592.0f, kRowsTop + sync_setup_rows_.cursor() - sync_setup_rows_.scroll(), 736.0f, kRowHeight}, 1.0f);
    for (int row = sync_setup_rows_.first_row(); row <= sync_setup_rows_.last_row(); ++row)
    {
        const float top = row_top(row);
        std::string title;
        std::string line;
        std::string state;
        Color tone = th().meta;
        Color state_tone = th().meta;
        if (choosing)
        {
            if (row >= static_cast<int>(pair_servers_.size()))
                continue;
            title = pair_servers_[static_cast<std::size_t>(row)].name;
            line = pair_servers_[static_cast<std::size_t>(row)].address;
        }
        else
        {
            if (row >= static_cast<int>(sync_setup_.profiles.size()))
                continue;
            const SaveSyncProfile &profile = sync_setup_.profiles[static_cast<std::size_t>(row)];
            title = profile.name;
            if (!profile.note.empty())
            {
                line = profile.note;
                tone = th().warning;
            }
            else
            {
                line = profile.linked ? profile.server : std::string{tr("Not linked: its save data stays on the console")};
            }
            state = profile.linked ? (profile.note.empty() ? tr("Linked") : tr("Needs attention")) : tr("Not linked");
            state_tone = profile.linked && profile.note.empty() ? th().accent_pale : th().meta;
            if (profile.current)
                title += "  ·  " + std::string{tr("playing")};
        }
        list.push_opacity(sync_setup_rows_.row_alpha(row, kRowHeight));
        const float taken = state.empty() ? 0.0f :
                                            text(c, state, 1292.0f, baseline(top + 14.0f, 38.0f, th().size_small), th().size_small,
                                                 state_tone, Align::right);
        text_fit(c, title, 628.0f, baseline(top + 14.0f, 38.0f, th().size_text), th().size_text, th().value,
                 664.0f - taken - 24.0f);
        text_fit(c, line, 628.0f, baseline(top + 52.0f, 28.0f, th().size_small), th().size_small, tone, 664.0f);
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, sync_setup_rows_, 1340.0f, kWindow.y, kWindow.h);

    const bool linked = !choosing && sync_setup_rows_.selected < static_cast<int>(sync_setup_.profiles.size()) &&
                        sync_setup_.profiles[static_cast<std::size_t>(sync_setup_rows_.selected)].linked;
    static constexpr Hint kProfileHints[] = {
        {Pad::updown, TR("Select")}, {Pad::cross, TR("Pair")}, {Pad::circle, TR("Back")}};
    static constexpr Hint kLinkedHints[] = {{Pad::updown, TR("Select")},
                                            {Pad::cross, TR("Pair again")},
                                            {Pad::square, TR("Unlink")},
                                            {Pad::circle, TR("Back")}};
    static constexpr Hint kServerHints[] = {
        {Pad::updown, TR("Select")}, {Pad::cross, TR("Pair")}, {Pad::circle, TR("Back")}};
    if (choosing)
        draw_hints(c, kServerHints, 3, 592.0f, kHints, th().copy, 736.0f);
    else if (linked)
        draw_hints(c, kLinkedHints, 4, 592.0f, kHints, th().copy, 736.0f);
    else
        draw_hints(c, kProfileHints, 3, 592.0f, kHints, th().copy, 736.0f);
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::poll_pairing(float dt)
{
    if (modal_ != Modal::pairing)
        return;
    pairing_time_ += dt;
    pairing_wait_ += dt;
    if (pairing_wait_ < kPollSeconds)
        return;
    pairing_wait_ = 0.0f;
    const PairingStage was = pairing_.stage;
    pairing_ = services_.pairing();
    if (pairing_.address != pairing_qr_text_)
    {
        pairing_qr_text_ = pairing_.address;
        pairing_qr_ = pairing_.address.empty() ? QrCode{} : make_qr_code(pairing_.address);
    }
    if (pairing_.stage != was && pairing_.stage == PairingStage::done)
        cue(Cue::notify);
    else if (pairing_.stage != was && pairing_.stage == PairingStage::failed)
        cue(Cue::error);
}

void Launcher::press_pairing(Key key)
{
    const bool over = pairing_.stage == PairingStage::done || pairing_.stage == PairingStage::failed;
    if (key == Key::cross && pairing_.stage == PairingStage::failed)
    {
        // Once more, with a new code.
        services_.cancel_pairing();
        start_pairing(sync_setup_profile_, pair_server_);
        return;
    }
    if (key == Key::circle || (key == Key::cross && over))
    {
        services_.cancel_pairing();
        const bool done = pairing_.stage == PairingStage::done;
        pairing_ = {};
        // Back to Settings > Save sync, as it is now.
        sync_setup_ = services_.save_sync_setup();
        sync_setup_choosing_ = false;
        sync_setup_rows_.reset(static_cast<int>(sync_setup_.profiles.size()), sync_setup_profile_);
        modal_ = modal_shown_ = Modal::sync_setup;
        if (done)
            say(tr("Paired. The profile's save data syncs from now on."));
        cue(Cue::back);
    }
}

void Launcher::draw_pairing(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kPairPanel, 28.0f, th().panel.with_alpha(0.98f), th().accent.with_alpha(0.38f), 1.8f);
    text_shrink(c, fill(tr("Pair {0}"), {pairing_.profile}), kPairPanel.x + 48.0f,
                baseline(kPairPanel.y + 34.0f, 62.0f, th().size_display), th().size_display, th().title,
                kPairPanel.w - 96.0f);
    text_fit(c, fill(tr("with {0}"), {pairing_.server}), kPairPanel.x + 48.0f, baseline(kPairPanel.y + 98.0f, 32.0f, th().size_small),
             th().size_small, th().copy, kPairPanel.w - 96.0f);

    // The QR code: dark modules on white, with the quiet zone it needs around it. Once it is
    // over, it fades: there is nothing to scan any more.
    const bool live = pairing_.stage == PairingStage::waiting || pairing_.stage == PairingStage::asking ||
                      pairing_.stage == PairingStage::idle;
    list.push_opacity(live ? 1.0f : 0.12f);
    list.rounded_rect(kQrBox, 18.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    if (pairing_qr_.size > 0)
    {
        const int quiet = 4;
        const float module = std::floor((kQrBox.w - 24.0f) / static_cast<float>(pairing_qr_.size + 2 * quiet));
        const float side = module * static_cast<float>(pairing_qr_.size);
        // On whole pixels: no seams between the modules.
        const float x0 = std::floor(kQrBox.x + (kQrBox.w - side) * 0.5f);
        const float y0 = std::floor(kQrBox.y + (kQrBox.h - side) * 0.5f);
        const Color ink = Color{0.067f, 0.067f, 0.067f, 1.0f};
        for (int y = 0; y < pairing_qr_.size; ++y)
            for (int x = 0; x < pairing_qr_.size;)
            {
                if (!pairing_qr_.at(x, y))
                {
                    ++x;
                    continue;
                }
                // A run of dark modules as one rectangle.
                int end = x;
                while (end < pairing_qr_.size && pairing_qr_.at(end, y))
                    ++end;
                list.rounded_rect({x0 + module * static_cast<float>(x), y0 + module * static_cast<float>(y),
                                   module * static_cast<float>(end - x), module + 0.5f},
                                  0.0f, ink);
                x = end;
            }
    }
    else
    {
        // Asking the server for a code: a turning arc where the code will be.
        arc(list, kQrBox.x + kQrBox.w * 0.5f, kQrBox.y + kQrBox.h * 0.5f, 60.0f, 8.0f, pairing_time_ * 4.2f,
            3.14159265f * (0.55f + 0.45f * std::sin(pairing_time_ * 2.1f)), th().accent_deep);
    }
    list.pop_opacity();

    // What to do, at the right.
    const float x = kQrBox.x + kQrBox.w + 56.0f;
    const float width = kPairPanel.x + kPairPanel.w - 48.0f - x;
    const auto step = [&](const char *number, const std::string &value, float top)
    {
        text(c, number, x, baseline(top, 34.0f, th().size_text), th().size_text, th().accent);
        notice_block(c, value, x + 40.0f, baseline(top, 34.0f, th().size_small), th().size_small, 30.0f, th().copy,
                     width - 40.0f, 2, false);
    };
    switch (pairing_.stage)
    {
    case PairingStage::idle:
    case PairingStage::asking:
        text_shrink(c, tr("Asking the server for a code..."), x, baseline(360.0f, 40.0f, th().size_text), th().size_text,
                    th().value, width);
        break;
    case PairingStage::waiting:
        step("1", tr("Scan the code with your phone, or open the address below."), 340.0f);
        step("2", fill(tr("Sign in to {0} as the user this profile's save data belongs to."), {pairing_.server}), 416.0f);
        step("3", tr("Check that the code there is this one, and approve."), 492.0f);
        text(c, spaced(pairing_.code), x + 40.0f, baseline(556.0f, 72.0f, th().size_display), th().size_display,
             th().title, Align::left, 6.0f);
        text_fit(c, pairing_.address, x + 40.0f, baseline(636.0f, 30.0f, th().size_small), th().size_small, th().meta,
                 width - 40.0f);
        text_fit(c, fill(tr("Waiting for the approval  ·  the code works {0} more"), {minutes(pairing_.seconds_left)}),
                 x + 40.0f, baseline(684.0f, 30.0f, th().size_small), th().size_small, th().accent_pale, width - 40.0f);
        break;
    case PairingStage::done:
        text_shrink(c, tr("Paired"), x, baseline(360.0f, 52.0f, th().size_heading), th().size_heading, th().accent, width);
        notice_block(c,
                     pairing_.user.empty() ?
                         std::string{tr("The profile's save data syncs with the server from now on.")} :
                         fill(tr("The profile's save data syncs as {0} from now on."), {pairing_.user}),
                     x, baseline(424.0f, 32.0f, th().size_small), th().size_small, 32.0f, th().copy, width, 3, false);
        break;
    case PairingStage::failed:
        text_shrink(c, tr("Not paired"), x, baseline(360.0f, 52.0f, th().size_heading), th().size_heading,
                    th().warning, width);
        notice_block(c,
                     pairing_.denied  ? std::string{tr("It was declined on the server.")} :
                     pairing_.expired ? std::string{tr("Nobody approved the code in time.")} :
                                        pairing_.error,
                     x, baseline(424.0f, 32.0f, th().size_small), th().size_small, 32.0f, th().warning, width, 3, false);
        break;
    }

    static constexpr Hint kWaiting[] = {{Pad::circle, TR("Cancel")}};
    static constexpr Hint kDone[] = {{Pad::cross, TR("Done")}};
    static constexpr Hint kFailed[] = {{Pad::cross, TR("Try again")}, {Pad::circle, TR("Back")}};
    const float hints = kPairPanel.y + kPairPanel.h - 54.0f;
    if (pairing_.stage == PairingStage::done)
        draw_hints(c, kDone, 1, x, hints, th().copy, width);
    else if (pairing_.stage == PairingStage::failed)
        draw_hints(c, kFailed, 2, x, hints, th().copy, width);
    else
        draw_hints(c, kWaiting, 1, x, hints, th().copy, width);
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
