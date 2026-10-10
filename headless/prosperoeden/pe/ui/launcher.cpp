// ProsperoEden - The launcher: home, library, settings and their dialogs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>

namespace pe::ui
{

using audio::Cue;

namespace
{
// How long the notification of a newer release stays.
constexpr float kUpdateNoticeSeconds = 10.0f;
// The update dialog's full height (update.cpp).
constexpr float kUpdatePanelHeight = 688.0f;
} // namespace

namespace
{

constexpr Rect kScreen{0.0f, 0.0f, 1920.0f, 1080.0f};

} // namespace

Launcher::Launcher(Services &services, Textures &textures, const Fonts &fonts, bool first_start)
    : services_(services), textures_(textures), fonts_(fonts), first_start_(first_start)
{
    version_ = services_.version();
    clock_ = services_.clock();
    prefs_ = services_.preferences();
    sources_ = services_.sources();
    sources_generation_ = sources_.generation; // the list read below has the sources' games
    downloads_ = services_.downloads();
    apply_look();
    read_profiles();
    read_home();
    const bool continue_ready = home_.setup_ready && home_.last_exists;
    home_focus_ = continue_ready ? 0 : home_.setup_ready ? 1 : 2;
    home_springs_[static_cast<std::size_t>(home_focus_)].snap(1.0f);
    settings_.visible = 12;
    settings_.pitch = 52.0f;
    settings_.reset(12, 0);
    section_.snap(1.0f);
    detail_.snap(1.0f);
    cue(home_.launch_failed ? Cue::notify : first_start ? Cue::welcome : Cue::resume);
    start_scan();
}

Launcher::~Launcher()
{
    room_edit_stop_ = true;
    if (room_edit_.valid()) room_edit_.wait();
    // The game list may still be reading; it uses the services this launcher was given.
    if (scan_.valid())
        scan_.wait();
}

std::vector<Cue> Launcher::take_cues()
{
    std::vector<Cue> result;
    result.swap(cues_);
    return result;
}

void Launcher::say(const std::string &text, bool warning)
{
    message_ = text;
    message_warning_ = warning;
    message_age_ = 0.0f;
}

void Launcher::open(Screen screen, bool forward)
{
    leaving_ = screen_;
    screen_ = screen;
    forward_ = forward;
    transition_.start(theme::kScreenSeconds);
    message_.clear();
    cue(forward ? Cue::open : Cue::back);
}

void Launcher::open_modal(Modal modal)
{
    modal_ = modal_shown_ = modal;
    option_ = 0;
    option_cursor_.snap(dialog_row_top(modal, 0));
    message_.clear();
    // Switches show their state at once; they only animate when changed.
    const std::array<bool, 7> states = switch_states(modal);
    for (std::size_t i = 0; i < states.size(); ++i)
        switches_[i].snap(states[i] ? 1.0f : 0.0f);
    cue(Cue::modal_open);
}

std::array<bool, 7> Launcher::switch_states(Modal modal) const
{
    switch (modal)
    {
    case Modal::video:
        return {prefs_.hud, false, false};
    case Modal::performance:
        return {prefs_.block_list,  prefs_.async_shaders,     prefs_.fast_gpu, prefs_.unsafe_cpu,
                prefs_.unsafe_dma,  prefs_.reactive_flushing, prefs_.skip_invalidation};
    case Modal::audio:
        return {prefs_.mute, false, false};
    case Modal::controls:
        return {prefs_.vibration, false, false};
    case Modal::accessibility:
        return {prefs_.large_text, prefs_.high_contrast, prefs_.reduce_motion};
    default:
        return {prefs_.detailed_logging, prefs_.immediate_logs, false};
    }
}

void Launcher::close_modal()
{
    modal_ = Modal::none;
    message_.clear();
    cue(Cue::modal_close);
}

void Launcher::apply_look()
{
    look() = {prefs_.large_text, prefs_.high_contrast, prefs_.reduce_motion};
}

bool Launcher::save_preferences(bool quiet)
{
    const bool saved = services_.set_preferences(prefs_);
    if (!saved)
        say(tr("Could not save settings. Please try again."), true);
    else if (quiet)
        message_.clear();
    else
        say(tr("Saved. Applies when a game starts."));
    return saved;
}

void Launcher::launch(const std::string &file, const std::string &title, const std::string &cover)
{
    // A game whose file was taken away since the menu showed it is not started: it leaves the
    // menu instead.
    if (!services_.game_exists(file))
    {
        read_home();
        drop_missing_games();
        say(tr("ROM missing from the game files folder"), true);
        cue(Cue::error);
        return;
    }
    // Its save data first, when the profile keeps it on a server.
    if (services_.save_sync_wanted(file))
    {
        sync_file_ = file;
        sync_title_ = title;
        sync_cover_ = cover;
        sync_launch_ = true;
        sync_started_ = false;
        sync_time_ = 0.0f;
        save_sync_wait_ = 1.0f; // looked at in this frame's update
        modal_ = modal_shown_ = Modal::save_sync;
        message_.clear();
        cue(Cue::modal_open);
        return;
    }
    start_game(file, title, cover);
}

void Launcher::start_game(const std::string &file, const std::string &title, const std::string &cover)
{
    if (!services_.can_launch_game()) {
        say(tr("Join a room successfully or leave it before starting a game."), true);
        cue(Cue::error);
        return;
    }
    services_.will_play(file);
    selected_game_ = services_.game_path(file);
    launch_title_ = title;
    launch_cover_ = cover;
    launch_.start(theme::kLaunchSeconds);
    press_ = 1.0f;
    cue(Cue::launch);
}

void Launcher::press(Key key)
{
    if (!selected_game_.empty())
        return; // a game is starting
    if (modal_ == Modal::game)
        return press_game(key);
    if (modal_ == Modal::mods)
        return press_mods(key);
    if (modal_ == Modal::game_options)
        return press_game_options(key);
    if (modal_ == Modal::mapping)
        return press_mapping(key);
    if (modal_ == Modal::profiles)
        return press_profiles(key);
    if (modal_ == Modal::update)
        return press_update(key);
    if (modal_ == Modal::download)
        return press_download(key);
    if (modal_ == Modal::source)
        return press_choice(key);
    if (modal_ == Modal::sources)
        return press_sources(key);
    if (modal_ == Modal::save_sync)
        return press_save_sync(key);
    if (modal_ == Modal::multiplayer)
        return press_multiplayer(key);
    if (modal_ == Modal::sync_setup)
        return press_sync_setup(key);
    if (modal_ == Modal::pairing)
        return press_pairing(key);
    if (modal_ != Modal::none)
        return press_dialog(key);
    switch (screen_)
    {
    case Screen::home:
        return press_home(key);
    case Screen::library:
        return press_library(key);
    case Screen::settings:
        return press_settings(key);
    case Screen::files:
        return press_files(key);
    case Screen::language:
        return press_language(key);
    case Screen::about:
        if (key == Key::circle)
            open(Screen::home, false);
        return;
    }
}

void Launcher::update(float dt)
{
    time_ += dt;
    intro_ += dt;
    message_age_ += dt;
    backdrop_.update(dt);
    textures_.pump(dt);
    finish_scan(false);
    poll_sources(dt);
    poll_save_sync(dt);
    poll_pairing(dt);
    refresh_sync_setup(dt);
    update_multiplayer();
    update_controllers(dt);
    transition_.update(dt);
    press_ = std::max(0.0f, press_ - dt / 0.18f);

    modal_open_.target = modal_ != Modal::none ? 1.0f : 0.0f;
    modal_open_.update(dt, 20.0f);
    if (modal_ == Modal::none && modal_open_.value < 0.01f)
        modal_shown_ = Modal::none;
    dim_.target = screen_ == Screen::home ? 0.0f : 0.30f;
    dim_.update(dt, 8.0f);

    for (std::size_t i = 0; i < home_springs_.size(); ++i)
    {
        home_springs_[i].target = static_cast<int>(i) == home_focus_ ? 1.0f : 0.0f;
        home_springs_[i].update(dt, theme::kFocusSpring);
    }
    library_.update(dt);
    settings_.update(dt);
    files_.update(dt);
    language_.update(dt);
    video_rows_.update(dt);
    performance_rows_.update(dt);
    game_rows_.update(dt);
    mod_rows_.update(dt);
    option_rows_.update(dt);
    mapping_rows_.update(dt);
    profile_rows_.update(dt);
    source_rows_.update(dt);
    choice_rows_.update(dt);
    sync_rows_.update(dt);
    sync_setup_rows_.update(dt);
    mode_.target = selected_docked_ ? 0.0f : 1.0f;
    mode_.update(dt, 22.0f);
    const bool mods_on = library_.selected >= 0 && library_.selected < static_cast<int>(games_.size()) &&
                         games_[static_cast<std::size_t>(library_.selected)].mods_enabled;
    mods_switch_.target = mods_on ? 1.0f : 0.0f;
    mods_switch_.update(dt, 22.0f);
    detail_.target = 1.0f;
    detail_.update(dt, 14.0f);
    section_.target = 1.0f;
    section_.update(dt, 14.0f);
    if (modal_ != Modal::none)
        option_cursor_.target = dialog_row_top(modal_, option_);
    option_cursor_.update(dt, theme::kCursorSpring);
    const std::array<bool, 7> states = switch_states(modal_shown_);
    for (std::size_t i = 0; i < states.size(); ++i)
    {
        switches_[i].target = states[i] ? 1.0f : 0.0f;
        switches_[i].update(dt, 22.0f);
    }

    // Games removed from the game files folder while the menu is open leave it within a moment.
    presence_wait_ += dt;
    if (presence_wait_ >= 2.0f && selected_game_.empty())
    {
        presence_wait_ = 0.0f;
        check_games_present();
    }

    // A newer release: the update dialog, or a notification when the app cannot install it.
    update_offer(dt);
    update_install(dt);
    if (update_notice_left_ > 0.0f)
        update_notice_left_ = std::max(0.0f, update_notice_left_ - dt);
    // It slides away over its last moments.
    update_notice_in_.target = update_notice_left_ > 0.4f ? 1.0f : 0.0f;
    update_notice_in_.update(dt, 14.0f);

    clock_wait_ += dt;
    if (clock_wait_ >= 1.0f)
    {
        clock_wait_ = 0.0f;
        clock_ = services_.clock();
    }

    if (!selected_game_.empty())
    {
        launch_.update(dt);
        if (!launch_.running)
            done_ = true;
    }
}

void Launcher::update_offer(float dt)
{
    (void)dt;
    if (!update_waiting_ && update_notice_left_ <= 0.0f && selected_game_.empty() && update_.version.empty())
    {
        UpdateOffer offer;
        if (services_.take_update(&offer) && !offer.version.empty())
        {
            // "v1.000.050" and "1.000.050" both read as the number.
            const std::string &newer = offer.version;
            update_version_ = newer.size() > 1 && (newer[0] == 'v' || newer[0] == 'V') ? newer.substr(1) : newer;
            if (offer.installable && first_start_)
            {
                // Asked each time the app opens (not on returning from a game).
                update_ = offer;
                notes_laid_out_ = false;
                update_waiting_ = true;
            }
            else
            {
                update_notice_left_ = kUpdateNoticeSeconds;
                cue(Cue::notify);
            }
        }
    }
    // The dialog waits for the welcome and for whatever dialog is open to close.
    if (update_waiting_ && modal_ == Modal::none && modal_shown_ == Modal::none && !transition_.running &&
        selected_game_.empty() && intro_ > 1.2f)
    {
        update_waiting_ = false;
        update_stage_ = UpdateStage::offer;
        update_stage_time_ = 0.0f;
        update_choice_ = 0;
        update_choice_x_.snap(0.0f);
        update_height_.snap(kUpdatePanelHeight);
        modal_ = modal_shown_ = Modal::update;
        message_.clear();
        cue(Cue::notify);
    }
}

void Launcher::draw_screen(Canvas &c, Screen screen)
{
    switch (screen)
    {
    case Screen::home:
        return draw_home(c);
    case Screen::library:
        return draw_library(c);
    case Screen::settings:
        return draw_settings(c);
    case Screen::files:
        return draw_files(c);
    case Screen::language:
        return draw_language(c);
    case Screen::about:
        return draw_about(c);
    }
}

void Launcher::draw_frame(Canvas &c, const char *title, const char *copy)
{
    text_shrink(c, title, 108.0f, baseline(62.0f, 64.0f, theme::kDisplay), theme::kDisplay,
                theme::kTitle, 1704.0f);
    text_shrink(c, copy, 110.0f, baseline(130.0f, 30.0f, theme::kSmall), theme::kSmall, theme::kCopy,
                1700.0f);
}

void Launcher::draw_footer(Canvas &c, const Hint *hints, int count)
{
    c.list.rounded_rect({108.0f, 955.0f, 1704.0f, 1.0f}, 0.0f, Color::rgb(0x586d5a, 0.9f));
    draw_hints(c, hints, count, 108.0f, 987.0f, theme::kCopy, 1704.0f);
}

void Launcher::draw_update_notice(Canvas &c)
{
    const float shown = tween::clamp01(update_notice_in_.value);
    if (shown <= 0.01f || update_version_.empty())
        return;
    gfx::DrawList &list = c.list;
    // Top right, over whatever the menu shows; it comes in from the right edge.
    const Rect panel{1352.0f, 44.0f, 520.0f, 108.0f};
    list.push_opacity(shown);
    list.push_transform(1.0f, 0.0f, 0.0f, (1.0f - shown) * 72.0f * motion(), 0.0f);
    glass(c, panel, 20.0f, theme::kPanel.with_alpha(0.97f), theme::kLime.with_alpha(0.55f), 1.4f);
    // A mark at the left: a lime disc with an arrow up.
    const float cx = panel.x + 48.0f;
    const float cy = panel.y + 50.0f;
    list.circle(cx, cy, 20.0f, theme::kLime.with_alpha(0.22f));
    list.line(cx, cy + 9.0f, cx, cy - 9.0f, 2.6f, theme::kLime);
    list.line(cx - 8.0f, cy - 2.0f, cx, cy - 10.0f, 2.6f, theme::kLime);
    list.line(cx + 8.0f, cy - 2.0f, cx, cy - 10.0f, 2.6f, theme::kLime);
    text_shrink(c, tr("Update available"), panel.x + 88.0f, baseline(panel.y + 18.0f, 34.0f, theme::kText24),
                theme::kText24, theme::kTitle, panel.w - 112.0f);
    text_shrink(c, fill(tr("Version {0} is on homebrew.page"), {update_version_}), panel.x + 88.0f,
                baseline(panel.y + 54.0f, 30.0f, theme::kSmall), theme::kSmall, theme::kCopy,
                panel.w - 112.0f);
    // The time it has left.
    const float left = tween::clamp01(update_notice_left_ / kUpdateNoticeSeconds);
    list.rounded_rect({panel.x + 20.0f, panel.y + panel.h - 12.0f, (panel.w - 40.0f) * left, 3.0f}, 1.5f,
                      theme::kLime.with_alpha(0.8f));
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::draw_launch(Canvas &c)
{
    const float t = launch_.progress();
    // The menu dims, the game's cover steps forward, then everything goes dark.
    const float black = tween::cubic_in_out((t - 0.45f) / 0.55f);
    const float veil = std::max(0.90f * tween::cubic_out(t / 0.22f), black);
    c.list.rounded_rect(kScreen, 0.0f, Color::rgb(0x020705, veil));

    const float appear = tween::back_out(t / 0.38f);
    const float leave = 1.0f - tween::cubic_in_out((t - 0.60f) / 0.40f);
    c.list.push_opacity(tween::clamp01(t / 0.18f) * leave);
    c.list.push_transform(1.0f - 0.14f * (1.0f - appear) * motion(), 960.0f, 470.0f, 0.0f, 0.0f);
    const Rect art{810.0f, 300.0f, 300.0f, 300.0f};
    c.list.shadow({art.x - 10.0f, art.y - 4.0f, art.w + 20.0f, art.h + 20.0f}, 30.0f, 70.0f,
                  theme::kLime.with_alpha(0.22f));
    cover(c, launch_cover_, art, 20.0f, 1.0f);
    text(c, tr("STARTING"), 960.0f, 668.0f, theme::kSmall, theme::kLime, Align::center, 4.0f);
    text_fit(c, launch_title_, 960.0f, 716.0f, theme::kHeading, theme::kTitle, 1300.0f,
             Align::center);
    c.list.pop_transform();
    c.list.pop_opacity();
}

void Launcher::draw(gfx::DrawList &list)
{
    Canvas c{list, fonts_, textures_, backdrop_, time_};
    backdrop_.draw(list, textures_, dim_.value);

    const bool launching = !selected_game_.empty();
    const float zoom =
        launching ? 1.0f + 0.045f * tween::cubic_in_out(launch_.progress()) * motion() : 1.0f;
    list.push_transform(zoom, 960.0f, 540.0f, 0.0f, 0.0f);
    if (transition_.running)
    {
        // The old screen slides away as the new one arrives from the other side.
        const float t = transition_.progress();
        const float e = tween::cubic_in_out(t);
        const float direction = forward_ ? 1.0f : -1.0f;
        list.push_opacity(1.0f - tween::cubic_out(t / 0.55f));
        list.push_transform(1.0f, 0.0f, 0.0f, -direction * 56.0f * e * motion(), 0.0f);
        draw_screen(c, leaving_);
        list.pop_transform();
        list.pop_opacity();
        list.push_opacity(tween::cubic_out((t - 0.2f) / 0.8f));
        list.push_transform(1.0f, 0.0f, 0.0f, direction * 56.0f * (1.0f - e) * motion(), 0.0f);
        draw_screen(c, screen_);
        list.pop_transform();
        list.pop_opacity();
    }
    else
    {
        draw_screen(c, screen_);
    }
    if (modal_shown_ != Modal::none)
    {
        const float opened = tween::clamp01(modal_open_.value);
        list.rounded_rect(kScreen, 0.0f, theme::kScrim.with_alpha(0.69f * opened));
        if (modal_shown_ == Modal::game)
            draw_game(c, opened);
        else if (modal_shown_ == Modal::mods)
            draw_mods(c, opened);
        else if (modal_shown_ == Modal::game_options)
            draw_game_options(c, opened);
        else if (modal_shown_ == Modal::mapping)
            draw_mapping(c, opened);
        else if (modal_shown_ == Modal::profiles)
            draw_profiles(c, opened);
        else if (modal_shown_ == Modal::update)
            draw_update(c, opened);
        else if (modal_shown_ == Modal::download)
            draw_download(c, opened);
        else if (modal_shown_ == Modal::source)
            draw_choice(c, opened);
        else if (modal_shown_ == Modal::save_sync)
            draw_save_sync(c, opened);
        else if (modal_shown_ == Modal::multiplayer)
            draw_multiplayer(c, opened);
        else if (modal_shown_ == Modal::sync_setup)
            draw_sync_setup(c, opened);
        else if (modal_shown_ == Modal::pairing)
            draw_pairing(c, opened);
        else if (modal_shown_ == Modal::sources)
            draw_sources(c, opened);
        else
            draw_dialog(c, modal_shown_, opened);
    }
    list.pop_transform();
    draw_update_notice(c);
    draw_save_sync_notice(c);
    if (launching)
        draw_launch(c);
    // Closing for the update: the screen goes dark over the last moments.
    if (modal_shown_ == Modal::update && update_stage_ == UpdateStage::closing)
    {
        const float dark = tween::cubic_in_out((update_stage_time_ - 2.2f) / 0.8f);
        if (dark > 0.0f)
            list.rounded_rect(kScreen, 0.0f, Color{0.0f, 0.0f, 0.0f, dark});
    }

    // The launcher arrives out of the dark.
    const float arrival = 1.0f - tween::cubic_out(intro_ / (first_start_ ? 0.7f : 0.45f));
    if (arrival > 0.0f)
        list.rounded_rect(kScreen, 0.0f, Color{0.0f, 0.0f, 0.0f, arrival});
}

} // namespace pe::ui
