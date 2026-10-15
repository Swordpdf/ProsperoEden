// ProsperoEden - Launcher: games on download sources. The Library lists them beside the console's
// own, with the sources that have them; Cross downloads one and starts it once it is there (the
// download dialog), Square puts it in the download queue, and with several sources the source
// dialog asks which one first. Settings > Downloads shows the sources and the queue.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace pe::ui
{

using audio::Cue;

namespace
{

// The download dialog: placed as the update dialog is (update.cpp).
constexpr Rect kPanel{560.0f, 196.0f, 800.0f, 688.0f};
constexpr float kCenterX = 960.0f;
constexpr float kRingY = 384.0f;
constexpr float kRingRadius = 96.0f;
constexpr float kRingWidth = 10.0f;
constexpr float kPi = 3.14159265f;
// The source dialog and Settings > Downloads: the dialog of Settings, rows under its title.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 384.0f;
constexpr float kRowPitch = 96.0f;
constexpr float kRowHeight = 94.0f;
constexpr int kRowsShown = 4;
constexpr Rect kWindow{592.0f, kRowsTop, 736.0f, kRowPitch * (kRowsShown - 1) + kRowHeight};
constexpr float kHints = 848.0f;
// How often the queue is looked at.
constexpr float kPollSeconds = 0.25f;
// How long a message stands where Settings > Downloads shows its buttons: with one source there is
// no row to move to, which is what takes a message away in the other dialogs.
constexpr float kMessageSeconds = 4.0f;

std::string bytes_label(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 30))
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    else
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return text;
}

// A download's speed.
std::string rate_label(std::uint64_t bytes_per_second)
{
    char text[32];
    if (bytes_per_second >= (1ull << 20))
        std::snprintf(text, sizeof(text), "%.1f MB/s", static_cast<double>(bytes_per_second) / 1048576.0);
    else
        std::snprintf(text, sizeof(text), "%.0f KB/s", static_cast<double>(bytes_per_second) / 1024.0);
    return text;
}

float share(const Download &download)
{
    return download.total > 0 ? std::clamp(static_cast<float>(static_cast<double>(download.done) /
                                                              static_cast<double>(download.total)),
                                           0.0f, 1.0f) :
                                0.0f;
}

std::string percent_of(const Download &download)
{
    return fill(tr("{0}%"), {std::to_string(static_cast<int>(share(download) * 100.0f))});
}

void progress_bar(gfx::DrawList &list, const Rect &bar, float amount, Color color)
{
    list.rounded_rect(bar, bar.h * 0.5f, th().panel_edge.with_alpha(0.22f));
    if (amount > 0.0f)
        list.rounded_rect({bar.x, bar.y, std::max(bar.h, bar.w * std::clamp(amount, 0.0f, 1.0f)), bar.h},
                          bar.h * 0.5f, color);
}

// A cloud of three puffs on a flat base, `width` wide, centred on (cx, cy).
void cloud(gfx::DrawList &list, float cx, float cy, float width, Color color)
{
    const float h = width * 0.56f;
    list.rounded_rect({cx - width * 0.5f, cy, width, h * 0.42f}, h * 0.21f, color);
    list.circle(cx - width * 0.23f, cy + h * 0.06f, width * 0.19f, color);
    list.circle(cx + width * 0.03f, cy - h * 0.12f, width * 0.27f, color);
    list.circle(cx + width * 0.28f, cy + h * 0.12f, width * 0.16f, color);
}

} // namespace

void Launcher::remote_cover(Canvas &c, const std::string &path, const Rect &r, float radius, float shadow)
{
    cover(c, path, r, radius, shadow);
    // Not on the console: the picture steps back...
    c.list.rounded_rect(r, radius, th().scrim.with_alpha(0.45f));
    // ...and a cloud says where it is.
    const float size = std::max(24.0f, r.w * 0.32f);
    const float inset = std::max(4.0f, r.w * 0.05f);
    const float cx = r.x + r.w - inset - size * 0.5f;
    const float cy = r.y + r.h - inset - size * 0.5f;
    c.list.circle(cx, cy, size * 0.5f, th().panel.with_alpha(0.94f));
    c.list.ring(cx, cy, size * 0.5f, std::max(1.0f, size * 0.04f), th().accent_pale.with_alpha(0.55f));
    cloud(c.list, cx, cy - size * 0.04f, size * 0.60f, th().accent_pale);
}

const Download *Launcher::download_of(const std::string &key) const
{
    for (const Download &download : downloads_)
        if (!key.empty() && download.key == key)
            return &download;
    return nullptr;
}

std::string Launcher::remote_state(const Game &game, Color *color) const
{
    const Download *download = download_of(game.key);
    if (download == nullptr)
    {
        // Where it is: its source, or how many have it.
        *color = th().meta;
        return game.sources.size() == 1 ? game.sources.front() :
                                          fill(tr("{0} sources"), {std::to_string(game.sources.size())});
    }
    switch (download->state)
    {
    case DownloadState::queued:
        *color = th().accent_pale;
        return tr("Queued");
    case DownloadState::downloading:
        *color = th().accent_pale;
        return percent_of(*download);
    case DownloadState::verifying:
        *color = th().accent_pale;
        return fill(tr("Checking {0}"), {percent_of(*download)});
    case DownloadState::failed:
        break;
    }
    *color = th().warning;
    return tr("Download failed");
}

void Launcher::poll_sources(float dt)
{
    sources_wait_ += dt;
    download_time_ += dt;
    if (sources_wait_ >= kPollSeconds)
    {
        sources_wait_ = 0.0f;
        downloads_ = services_.downloads();
        sources_ = services_.sources();
        // A source's games, a cover or a downloaded game changed: the Library reads its list again.
        if (sources_.generation != sources_generation_)
        {
            sources_generation_ = sources_.generation;
            rescan_ = true;
        }
        if (source_rows_.count != source_row_count())
            source_rows_.reset(source_row_count(), std::min(source_rows_.selected, source_row_count() - 1));
        // The lists the player asked for are in: what came of them.
        if (lists_asked_ && std::none_of(sources_.list.begin(), sources_.list.end(),
                                         [](const SourceInfo &source) { return source.refreshing; }))
        {
            lists_asked_ = false;
            int games = 0;
            std::vector<const SourceInfo *> failed;
            for (const SourceInfo &source : sources_.list)
            {
                if (source.online)
                    games += source.games;
                else
                    failed.push_back(&source);
            }
            if (failed.empty())
                say(fill(games == 1 ? tr("The game lists are read: {0} game.") : tr("The game lists are read: {0} games."),
                         {std::to_string(games)}));
            else if (failed.size() == 1)
                say(fill(tr("The game list of {0} could not be read."), {failed.front()->name}), true);
            else
                say(fill(tr("{0} game lists could not be read."), {std::to_string(failed.size())}), true);
        }
    }
    // The new list once the one being read is in.
    if (rescan_ && games_loaded_ && !scan_.valid())
    {
        rescan_ = false;
        start_scan();
    }
    const Download *shown = download_open_ ? download_of(download_game_.key) : nullptr;
    // The file it comes as: the name it has on the source it comes from.
    if (shown != nullptr && !shown->file.empty())
        download_file_ = shown->file;
    download_fraction_.target = shown != nullptr ? share(*shown) : download_fraction_.target;
    download_fraction_.update(dt, 9.0f);
    // The game being played is started once all of it is there.
    if (download_open_ && modal_ == Modal::download && shown == nullptr && selected_game_.empty())
    {
        download_open_ = false;
        modal_ = Modal::none;
        if (services_.game_exists(download_file_))
            launch(download_file_, download_game_.name, download_game_.cover);
        else
            cue(Cue::modal_close); // cancelled in the meantime
    }
}

void Launcher::download_from(const Game &game, int source, bool play)
{
    if (!services_.download(game, source, play))
    {
        say(tr("The source no longer has this game."), true);
        cue(Cue::error);
        return;
    }
    downloads_ = services_.downloads();
    if (!play)
    {
        say(tr("Added to the downloads. Settings, Downloads shows the queue."));
        cue(Cue::select);
        return;
    }
    download_game_ = game;
    download_file_ = game.file;
    download_open_ = true;
    download_time_ = 0.0f;
    const Download *download = download_of(game.key);
    download_fraction_.snap(download != nullptr ? share(*download) : 0.0f);
    modal_ = modal_shown_ = Modal::download;
    message_.clear();
    cue(Cue::modal_open);
}

void Launcher::play_remote(const Game &game)
{
    // Already coming: from the source it comes from, now first.
    if (const Download *download = download_of(game.key))
    {
        const auto at = std::find(game.sources.begin(), game.sources.end(), download->source);
        download_from(game, at != game.sources.end() ? static_cast<int>(at - game.sources.begin()) : 0, true);
        return;
    }
    if (game.sources.size() > 1)
        choose_source(game, true);
    else
        download_from(game, 0, true);
}

void Launcher::queue_remote(const Game &game)
{
    if (download_of(game.key) != nullptr)
    {
        const bool cancelled = services_.cancel_download(game.key);
        downloads_ = services_.downloads();
        say(cancelled ? tr("Download cancelled.") : tr("Could not cancel the download."), !cancelled);
        cue(cancelled ? Cue::back : Cue::error);
        return;
    }
    if (game.sources.size() > 1)
        choose_source(game, false);
    else
        download_from(game, 0, false);
}

// ---------------------------------------------------------------- which source

void Launcher::choose_source(const Game &game, bool play)
{
    choice_game_ = game;
    choice_play_ = play;
    open_modal(Modal::source);
    choice_rows_.visible = kRowsShown;
    choice_rows_.pitch = kRowPitch;
    choice_rows_.reset(static_cast<int>(game.sources.size()), 0);
}

void Launcher::press_choice(Key key)
{
    switch (key)
    {
    case Key::circle:
        close_modal();
        return;
    case Key::up:
    case Key::down:
        if (choice_rows_.move(key == Key::down ? 1 : -1))
            cue(Cue::focus);
        return;
    case Key::cross:
    {
        const Game game = choice_game_;
        modal_ = Modal::none;
        download_from(game, choice_rows_.selected, choice_play_);
        return;
    }
    default:
        return;
    }
}

void Launcher::draw_choice(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Download from"), 592.0f, baseline(218.0f, 62.0f, th().size_display), th().size_display,
                th().title, 736.0f);
    text_fit(c, choice_game_.name, 592.0f, baseline(291.0f, 32.0f, th().size_small), th().size_small,
             th().copy, 736.0f);
    text_shrink(c, tr("Several sources have this game. Which one should it come from?"), 592.0f,
                baseline(328.0f, 34.0f, th().size_small), th().size_small, th().copy, 736.0f);

    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - choice_rows_.scroll(); };
    for (int row = choice_rows_.first_row(); row <= choice_rows_.last_row(); ++row)
    {
        list.push_opacity(choice_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, row_plate(), {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(),
                {592.0f, kRowsTop + choice_rows_.cursor() - choice_rows_.scroll(), 736.0f, kRowHeight}, 1.0f);
    for (int row = choice_rows_.first_row(); row <= choice_rows_.last_row(); ++row)
    {
        if (row >= static_cast<int>(choice_game_.sources.size()))
            continue;
        const std::string &name = choice_game_.sources[static_cast<std::size_t>(row)];
        const float top = row_top(row);
        list.push_opacity(choice_rows_.row_alpha(row, kRowHeight));
        // Its name, and where it is under it.
        std::string address;
        for (const SourceInfo &source : sources_.list)
            if (source.name == name)
                address = source.address;
        text_fit(c, name, 628.0f, baseline(top + 14.0f, 38.0f, th().size_text), th().size_text, th().value, 664.0f);
        text_fit(c, address, 628.0f, baseline(top + 52.0f, 28.0f, th().size_small), th().size_small, th().meta, 664.0f);
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, choice_rows_, 1340.0f, kWindow.y, kWindow.h);

    static constexpr Hint kChoice[] = {
        {Pad::updown, TR("Select")}, {Pad::cross, TR("Download")}, {Pad::circle, TR("Back")}};
    draw_menu_hints(c, kChoice, 3, 592.0f, kHints, th().copy, 736.0f);
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------- the download dialog

void Launcher::press_download(Key key)
{
    const Download *download = download_of(download_game_.key);
    switch (key)
    {
    case Key::circle:
        // It goes on in the background; the game is not started.
        download_open_ = false;
        close_modal();
        return;
    case Key::square:
        if (download != nullptr)
        {
            (void)services_.cancel_download(download_game_.key);
            downloads_ = services_.downloads();
        }
        download_open_ = false;
        say(tr("Download cancelled."));
        modal_ = Modal::none;
        cue(Cue::back);
        return;
    case Key::cross:
        if (download != nullptr && download->state == DownloadState::failed)
        {
            // From the source it came from.
            const auto at = std::find(download_game_.sources.begin(), download_game_.sources.end(), download->source);
            (void)services_.download(download_game_,
                                     at != download_game_.sources.end() ?
                                         static_cast<int>(at - download_game_.sources.begin()) : 0,
                                     true);
            downloads_ = services_.downloads();
            download_time_ = 0.0f;
            cue(Cue::select);
        }
        return;
    default:
        return;
    }
}

void Launcher::draw_download(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.04f * (1.0f - open) * motion(), kCenterX, 540.0f, 0.0f,
                        (1.0f - open) * 30.0f * motion());
    glass(c, kPanel, 28.0f, th().panel.with_alpha(0.97f), th().accent.with_alpha(0.38f), 1.8f);
    const float hints = kPanel.y + kPanel.h - 54.0f;
    const Download *download = download_of(download_game_.key);
    const bool failed = download != nullptr && download->state == DownloadState::failed;
    const bool checking = download != nullptr && download->state == DownloadState::verifying;
    const bool running = download != nullptr && (download->state == DownloadState::downloading || checking);
    const Color accent = failed ? th().warning : th().accent;
    const auto centred = [&](std::string_view value, float top, float line, float size, Color color)
    { text_shrink(c, value, kCenterX, baseline(top, line, size), size, color, kPanel.w - 96.0f, Align::center); };

    // The game's cover in the ring, the ring filling as it downloads.
    list.circle(kCenterX, kRingY, kRingRadius - kRingWidth, th().base.with_alpha(0.55f));
    list.ring(kCenterX, kRingY, kRingRadius, kRingWidth, th().panel_edge.with_alpha(0.22f));
    const float amount = tween::clamp01(download_fraction_.value);
    if (running || failed)
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * amount, accent);
    else
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, download_time_ * 4.2f,
            kPi * (0.55f + 0.45f * std::sin(download_time_ * 2.1f)), accent);
    const float art = (kRingRadius - kRingWidth) * 2.0f * 0.70710678f;
    cover(c, download_game_.cover, {kCenterX - art * 0.5f, kRingY - art * 0.5f, art, art}, 12.0f);

    centred(download_game_.name, 506.0f, 46.0f, th().size_heading, th().title);
    std::string state;
    if (failed)
        state = tr("The download stopped");
    else if (checking)
        state = fill(tr("Checking the game files {0}"), {percent_of(*download)});
    else if (running)
        state = fill(tr("Downloading {0}"), {percent_of(*download)});
    else if (download != nullptr)
    {
        // Another game is downloading first.
        std::string other;
        for (const Download &before : downloads_)
            if (before.state == DownloadState::downloading || before.state == DownloadState::verifying)
                other = before.name;
        state = other.empty() ? std::string{tr("Waiting to download")} : fill(tr("Waiting for {0}"), {other});
    }
    else
        state = tr("Preparing");
    centred(state, 560.0f, 32.0f, th().size_text, failed ? th().warning : th().value);

    progress_bar(list, {kPanel.x + 96.0f, 618.0f, kPanel.w - 192.0f, 8.0f}, amount, accent);
    if (failed)
    {
        notice_block(c, download->error, kPanel.x + 72.0f, baseline(642.0f, 30.0f, th().size_small), th().size_small, 28.0f,
                     th().copy, kPanel.w - 144.0f, 2, false);
    }
    else if (download != nullptr && download->total > 0)
    {
        std::string line = bytes_label(download->done) + "  /  " + bytes_label(download->total);
        if (running && !checking && download->rate > 0)
            line += "  ·  " + rate_label(download->rate);
        if (running && !checking && download->rate > 0 && download->total > download->done)
        {
            const int whole = std::max(1, static_cast<int>(std::ceil(static_cast<double>(download->total - download->done) /
                                                                     static_cast<double>(download->rate))));
            line += "  ·  ";
            line += whole < 90 ? fill(tr("About {0} s left"), {std::to_string(whole)}) :
                                 fill(tr("About {0} min left"), {std::to_string((whole + 59) / 60)});
        }
        centred(line, 642.0f, 32.0f, th().size_small, th().accent_pale);
    }
    if (!failed)
    {
        // Where it comes from, and what happens then.
        if (download != nullptr && !download->source.empty())
            centred(fill(tr("From {0}"), {download->source}), 676.0f, 28.0f, th().size_small, th().meta);
        centred(tr("The game starts when the download is done."), 706.0f, 30.0f, th().size_small, th().copy);
    }

    if (failed)
    {
        static constexpr Hint kFailed[] = {
            {Pad::cross, TR("Try again")}, {Pad::square, TR("Cancel download")}, {Pad::circle, TR("Close")}};
        draw_menu_hints(c, kFailed, 3, kPanel.x + 52.0f, hints, th().copy, kPanel.w - 104.0f);
    }
    else
    {
        static constexpr Hint kRunning[] = {{Pad::circle, TR("Download in the background")},
                                            {Pad::square, TR("Cancel download")}};
        draw_menu_hints(c, kRunning, 2, kPanel.x + 52.0f, hints, th().copy, kPanel.w - 104.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------- Settings > Downloads

int Launcher::source_row_count() const
{
    // The sources (Cross reads their lists again), then the downloads.
    return static_cast<int>(sources_.list.size() + downloads_.size());
}

void Launcher::open_sources()
{
    downloads_ = services_.downloads();
    sources_ = services_.sources();
    open_modal(Modal::sources);
    source_rows_.visible = kRowsShown;
    source_rows_.pitch = kRowPitch;
    source_rows_.reset(source_row_count(), 0);
}

void Launcher::press_sources(Key key)
{
    switch (key)
    {
    case Key::circle:
        close_modal();
        return;
    case Key::up:
    case Key::down:
        if (source_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::cross:
    case Key::square:
        break;
    default:
        return;
    }
    const int row = source_rows_.selected;
    const int sources = static_cast<int>(sources_.list.size());
    if (row < sources)
    {
        if (key != Key::cross)
            return;
        services_.refresh_sources();
        sources_ = services_.sources();
        lists_asked_ = true;
        say(tr("Reading the game lists..."));
        cue(Cue::select);
        return;
    }
    if (row - sources >= static_cast<int>(downloads_.size()))
        return;
    const Download download = downloads_[static_cast<std::size_t>(row - sources)];
    if (key == Key::square)
    {
        const bool cancelled = services_.cancel_download(download.key);
        downloads_ = services_.downloads();
        source_rows_.reset(source_row_count(), std::min(row, std::max(0, source_row_count() - 1)));
        say(cancelled ? tr("Download cancelled.") : tr("Could not cancel the download."), !cancelled);
        cue(cancelled ? Cue::back : Cue::error);
    }
    else if (download.state == DownloadState::failed)
    {
        // Again, from the source it came from.
        for (const Game &game : games_)
            if (game.key == download.key)
            {
                const auto at = std::find(game.sources.begin(), game.sources.end(), download.source);
                (void)services_.download(game, at != game.sources.end() ? static_cast<int>(at - game.sources.begin()) : 0,
                                         false);
            }
        downloads_ = services_.downloads();
        source_rows_.reset(source_row_count(), std::min(row, std::max(0, source_row_count() - 1)));
        say(tr("Trying again."));
        cue(Cue::select);
    }
}

void Launcher::draw_sources(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, th().radius_panel, th().panel.with_alpha(0.97f), th().panel_edge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Downloads"), 592.0f, baseline(218.0f, 62.0f, th().size_display), th().size_display, th().title,
                736.0f);
    text_shrink(c, tr("Games on your network, downloaded when you play them."), 592.0f,
                baseline(291.0f, 32.0f, th().size_small), th().size_small, th().copy, 736.0f);
    if (sources_.configured)
        text_shrink(c, fill(tr("Downloads need an FTP server running on the console, on port {0}."),
                            {std::to_string(sources_.ftp_port)}),
                    592.0f, baseline(326.0f, 30.0f, th().size_small), th().size_small, th().meta, 736.0f);

    if (!sources_.configured)
    {
        // How to set up a source: a file, as with the keys, here with a RomM server (README).
        text_block(c,
                   fill(tr("No download source is set up. Save a file named {0} that lists your sources, as this one "
                           "does with a RomM server, then open this menu again. Downloads need an FTP server running "
                           "on the console, on port {1}."),
                        {sources_.setup_file, std::to_string(sources_.ftp_port)}),
                   592.0f, baseline(332.0f, 34.0f, th().size_text), th().size_text, 34.0f, th().body, 736.0f, 5,
                   kShrink);
        if (!sources_.error.empty())
            notice_block(c, sources_.error, 592.0f, baseline(508.0f, 30.0f, th().size_small), th().size_small, 28.0f,
                         th().warning, 736.0f, 2, true);
        list.bordered_rect({592.0f, 570.0f, 736.0f, 202.0f}, 14.0f, gfx::mix(th().panel, th().row, 0.3f).with_alpha(0.75f), 1.0f,
                           th().row_edge.with_alpha(0.6f));
        static constexpr const char *kExample[] = {"{ \"sources\": [",
                                                   "    { \"type\": \"romm\", \"name\": \"Home\",",
                                                   "      \"url\": \"http://192.168.1.20:3000\",",
                                                   "      \"token\": \"rmm_...\" }",
                                                   "] }"};
        for (int i = 0; i < 5; ++i)
            text(c, kExample[i], 620.0f, baseline(588.0f + 34.0f * static_cast<float>(i), 34.0f, th().size_small),
                 th().size_small, th().accent_pale);
        static constexpr Hint kBack[] = {{Pad::circle, TR("Back")}};
        draw_menu_hints(c, kBack, 1, 592.0f, kHints, th().copy, 736.0f);
        list.pop_transform();
        list.pop_opacity();
        return;
    }

    const int sources = static_cast<int>(sources_.list.size());
    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - source_rows_.scroll(); };
    for (int row = source_rows_.first_row(); row <= source_rows_.last_row(); ++row)
    {
        list.push_opacity(source_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, row_plate(), {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(),
                {592.0f, kRowsTop + source_rows_.cursor() - source_rows_.scroll(), 736.0f, kRowHeight}, 1.0f);
    for (int row = source_rows_.first_row(); row <= source_rows_.last_row(); ++row)
    {
        const float top = row_top(row);
        list.push_opacity(source_rows_.row_alpha(row, kRowHeight));
        if (row < sources)
        {
            // A source: its name and state, where it is under it.
            const SourceInfo &source = sources_.list[static_cast<std::size_t>(row)];
            const bool warning = !source.online && !source.refreshing && !source.error.empty();
            const std::string state =
                source.refreshing ? std::string{tr("Reading...")} :
                source.online     ? fill(source.games == 1 ? tr("{0} game") : tr("{0} games"), {std::to_string(source.games)}) :
                warning           ? std::string{tr("Not reachable")} :
                                    std::string{tr("Not connected yet")};
            const float taken = text_shrink(c, state, 1292.0f, baseline(top + 14.0f, 38.0f, th().size_small), th().size_small,
                                            warning ? th().warning : th().accent_pale, 300.0f, Align::right);
            text_fit(c, source.name, 628.0f, baseline(top + 14.0f, 38.0f, th().size_text), th().size_text, th().value,
                     664.0f - taken - 28.0f);
            notice(c, warning ? source.error : source.address, 628.0f, baseline(top + 52.0f, 28.0f, th().size_small),
                   th().size_small, warning ? th().warning : th().meta, 664.0f, warning);
            list.pop_opacity();
            continue;
        }
        // The queue is read again in other places too: a row it no longer has stays empty until the
        // rows follow it.
        if (row - sources >= static_cast<int>(downloads_.size()))
        {
            list.pop_opacity();
            continue;
        }
        const Download &download = downloads_[static_cast<std::size_t>(row - sources)];
        std::string detail;
        Color tone = th().meta;
        switch (download.state)
        {
        case DownloadState::queued:
            detail = fill(tr("Queued, from {0}"), {download.source});
            break;
        case DownloadState::downloading:
            detail = bytes_label(download.done) + " / " + bytes_label(download.total);
            if (download.rate > 0)
                detail += "  ·  " + rate_label(download.rate);
            tone = th().accent_pale;
            break;
        case DownloadState::verifying:
            tone = th().accent_pale;
            break;
        case DownloadState::failed:
            detail = download.error;
            tone = th().warning;
            break;
        }
        const bool moving = download.state == DownloadState::downloading || download.state == DownloadState::verifying;
        const float right =
            moving ? text(c,
                          download.state == DownloadState::verifying ? fill(tr("Checking {0}"), {percent_of(download)}) :
                                                                       percent_of(download),
                          1292.0f, baseline(top + 14.0f, 38.0f, th().size_text), th().size_text, th().accent_pale,
                          Align::right) :
                     0.0f;
        text_fit(c, download.name, 628.0f, baseline(top + 14.0f, 38.0f, th().size_text), th().size_text, th().value,
                 664.0f - right - 28.0f);
        if (moving)
            progress_bar(list, {628.0f, top + 62.0f, 664.0f, 6.0f}, share(download), th().accent);
        else
            notice(c, detail, 628.0f, baseline(top + 52.0f, 28.0f, th().size_small), th().size_small, tone, 664.0f,
                   download.state == DownloadState::failed);
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, source_rows_, 1340.0f, kWindow.y, kWindow.h);
    if (downloads_.empty() && sources < kRowsShown)
        text_block(c, tr("No downloads. In the Library, Square puts a game from a source in the queue."), 592.0f,
                   baseline(kRowsTop + kRowPitch * static_cast<float>(sources) + 10.0f, 30.0f, th().size_small),
                   th().size_small, 30.0f, th().meta, 736.0f, 2, kShrink);

    // "Reading the game lists..." stands while they are read; what came of them, as any other
    // message, for a few seconds.
    if (!message_.empty() && (lists_asked_ || message_age_ < kMessageSeconds))
    {
        notice(c, message_, 592.0f, kHints + 7.0f, th().size_small,
               message_warning_ ? th().warning : th().accent_pale, 736.0f, message_warning_);
    }
    else if (source_rows_.selected < sources)
    {
        static constexpr Hint kList[] = {{Pad::cross, TR("Read the lists again")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kList, 2, 592.0f, kHints, th().copy, 736.0f);
    }
    else
    {
        const int at = source_rows_.selected - sources;
        const bool failed = at < static_cast<int>(downloads_.size()) &&
                            downloads_[static_cast<std::size_t>(at)].state == DownloadState::failed;
        static constexpr Hint kFailed[] = {
            {Pad::cross, TR("Try again")}, {Pad::square, TR("Cancel download")}, {Pad::circle, TR("Back")}};
        static constexpr Hint kQueued[] = {{Pad::square, TR("Cancel download")}, {Pad::circle, TR("Back")}};
        if (failed)
            draw_menu_hints(c, kFailed, 3, 592.0f, kHints, th().copy, 736.0f);
        else
            draw_menu_hints(c, kQueued, 2, 592.0f, kHints, th().copy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
