// ProsperoEden - The launcher: home, library, multiplayer, settings and their dialogs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>

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
    // The look the player chose; a theme this build does not have leaves the first in use.
    use_theme(prefs_.theme);
    sources_ = services_.sources();
    sources_generation_ = sources_.generation; // the list read below has the sources' games
    downloads_ = services_.downloads();
    apply_look();
    read_profiles();
    read_home();
    home_session_ = session_line(home_.last_title_id);
    home_focus_ = 0;
    home_springs_[0].snap(1.0f);
    rail_active_[0].snap(1.0f);
    settings_.wrap = false;
    settings_.visible = 12;
    settings_.reset(12, 0);
    section_.snap(1.0f);
    detail_.snap(1.0f);
    cue(home_.launch_failed ? Cue::notify : first_start ? Cue::welcome : Cue::resume);
    opening_on_ = first_start && textures_.splash() != 0;
    start_scan();
}

Launcher::~Launcher()
{
    room_directory_stop_ = true;
    if (room_directory_request_.valid()) room_directory_request_.wait();
    room_edit_stop_ = true;
    if (room_edit_.valid()) room_edit_.wait();
    seed_edit_stop_ = true;
    if (seed_edit_.valid()) seed_edit_.wait();
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
    rail_focus_ = false;
    screen_time_ = 0.0f;
    transition_.start(th().screen_seconds);
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
    launch_.start(th().launch_seconds);
    press_ = 1.0f;
    cue(Cue::launch);
}

void Launcher::press(Key key)
{
    if (!selected_game_.empty())
        return; // a game is starting
    if (opening_on_ && opening_ < opening_reveal())
    {
        // Any button ends the opening's first part: the menu comes at once.
        opening_ = opening_reveal();
        return;
    }
    key = menu_key_for(key, prefs_.menu_bind); // the core menu buttons the user has chosen
    if (modal_ == Modal::game)
        return press_game(key);
    if (modal_ == Modal::mods)
        return press_mods(key);
    if (modal_ == Modal::game_options)
        return press_game_options(key);
    if (modal_ == Modal::mapping)
        return press_mapping(key);
    if (modal_ == Modal::menu_mapping)
        return press_menu_mapping(key);
    if (modal_ == Modal::hud)
        return press_hud(key);
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
    if (modal_ == Modal::sync_setup)
        return press_sync_setup(key);
    if (modal_ == Modal::pairing)
        return press_pairing(key);
    if (modal_ != Modal::none)
        return press_dialog(key);
    // The rail, and the sections it holds: L1 and R1 go from one to the next.
    if (rail_focus_)
        return press_rail(key);
    if ((key == Key::l1 || key == Key::r1) && at_section())
    {
        go_section((section() + (key == Key::r1 ? 1 : kSections - 1)) % kSections);
        return;
    }
    switch (screen_)
    {
    case Screen::home:
        return press_home(key);
    case Screen::library:
        return press_library(key);
    case Screen::multiplayer:
        return press_multiplayer(key);
    case Screen::settings:
        return press_settings(key);
    case Screen::files:
        return press_files(key);
    case Screen::language:
        return press_language(key);
    case Screen::about:
        if (key == Key::circle)
            go_section(0);
        else if (key == Key::left)
            focus_rail();
        return;
    }
}

void Launcher::update(float dt)
{
    time_ += dt;
    intro_ += dt;
    if (opening_on_)
    {
        opening_ += dt;
        // The menu's parts arrive as the launch picture leaves, a moment after the mark set off.
        if (opening_ < opening_reveal() + (look().reduce_motion ? 0.0f : 0.14f))
            intro_ = 0.0f;
        if (opening_ >= opening_reveal() + opening_handover())
        {
            opening_on_ = false;
            textures_.drop_splash();
        }
    }
    screen_time_ += dt;
    message_age_ += dt;
    backdrop_.update(dt);
    textures_.pump(dt);
    finish_scan(false);
    poll_sources(dt);
    poll_save_sync(dt);
    poll_pairing(dt);
    refresh_sync_setup(dt);
    update_multiplayer();
    update_game_options();
    update_controllers(dt);
    transition_.update(dt);
    press_ = std::max(0.0f, press_ - dt / 0.18f);

    modal_open_.target = modal_ != Modal::none ? 1.0f : 0.0f;
    modal_open_.update(dt, 20.0f);
    if (modal_ == Modal::none && modal_open_.value < 0.01f)
        modal_shown_ = Modal::none;
    dim_.target = screen_ == Screen::home ? 0.0f : 1.0f;
    dim_.update(dt, 8.0f);

    const Theme &t = th();
    for (std::size_t i = 0; i < home_springs_.size(); ++i)
    {
        home_springs_[i].target = static_cast<int>(i) == home_focus_ && !rail_focus_ ? 1.0f : 0.0f;
        home_springs_[i].update(dt, t.focus_spring);
    }
    for (std::size_t i = 0; i < settings_springs_.size(); ++i)
    {
        settings_springs_[i].target = static_cast<int>(i) == settings_.selected && !rail_focus_ ? 1.0f : 0.0f;
        settings_springs_[i].update(dt, t.focus_spring);
    }
    rail_open_.target = rail_focus_ ? 1.0f : 0.0f;
    rail_open_.update(dt, 18.0f);
    for (int i = 0; i < kSections; ++i)
    {
        const std::size_t index = static_cast<std::size_t>(i);
        rail_springs_[index].target = rail_focus_ && rail_index_ == i ? 1.0f : 0.0f;
        rail_springs_[index].update(dt, t.focus_spring);
        rail_active_[index].target = section() == i ? 1.0f : 0.0f;
        rail_active_[index].update(dt, 14.0f);
    }
    library_.update(dt);
    grid_rows_.update(dt);
    grid_x_.update(dt, t.cursor_spring);
    grid_y_.update(dt, t.cursor_spring);
    pane_.target = modal_ == Modal::game_options || modal_ == Modal::mods ||
                           (modal_ == Modal::mapping && mapping_for_game_) ? 1.0f : 0.0f;
    pane_.update(dt, 18.0f);
    settings_.update(dt);
    files_.update(dt);
    language_.update(dt);
    video_rows_.update(dt);
    performance_rows_.update(dt);
    controls_rows_.update(dt);
    game_rows_.update(dt);
    mod_rows_.update(dt);
    option_rows_.update(dt);
    mapping_rows_.update(dt);
    menu_mapping_rows_.update(dt);
    hud_rows_.update(dt);
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
    if (!selected_read_ && screen_ == Screen::library && detail_.value > 0.5f)
        read_selected_details();
    section_.target = 1.0f;
    section_.update(dt, 14.0f);
    if (modal_ != Modal::none)
        option_cursor_.target = dialog_row_top(modal_, option_);
    option_cursor_.update(dt, t.cursor_spring);
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
        rail_focus_ = false;
        modal_ = modal_shown_ = Modal::update;
        message_.clear();
        cue(Cue::notify);
    }
}

void Launcher::draw_screen(Canvas &c, Screen screen)
{
    drawing_shown_ = screen == screen_;
    switch (screen)
    {
    case Screen::home:
        return draw_home(c);
    case Screen::library:
        return draw_library(c);
    case Screen::multiplayer:
        return draw_multiplayer(c);
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

void Launcher::draw_update_notice(Canvas &c)
{
    const float shown = tween::clamp01(update_notice_in_.value);
    if (shown <= 0.01f || update_version_.empty())
        return;
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    // Top right, over whatever the menu shows; it comes in from the right edge.
    const Rect card{1344.0f, 44.0f, 480.0f, 108.0f};
    list.push_opacity(shown);
    list.push_transform(1.0f, 0.0f, 0.0f, (1.0f - shown) * 72.0f * motion(), 0.0f);
    glass(c, card, t.radius_card, t.panel.with_alpha(0.97f), t.accent.with_alpha(0.55f), 1.4f);
    // A mark at the left: a disc of the accent with an arrow up.
    const float cx = card.x + 48.0f;
    const float cy = card.y + 50.0f;
    list.circle(cx, cy, 22.0f, t.accent);
    list.line(cx, cy + 9.0f, cx, cy - 9.0f, 2.8f, t.on_accent);
    list.line(cx - 8.0f, cy - 2.0f, cx, cy - 10.0f, 2.8f, t.on_accent);
    list.line(cx + 8.0f, cy - 2.0f, cx, cy - 10.0f, 2.8f, t.on_accent);
    {
        const Heavy heavy(c, 0.6f);
        text_shrink(c, tr("Update available"), card.x + 88.0f, baseline(card.y + 18.0f, 34.0f, t.size_text),
                    t.size_text, t.title, card.w - 112.0f);
    }
    text_shrink(c, fill(tr("Version {0} is on homebrew.page"), {update_version_}), card.x + 88.0f,
                baseline(card.y + 54.0f, 30.0f, t.size_small), t.size_small, t.copy, card.w - 112.0f);
    // The time it has left.
    const float left = tween::clamp01(update_notice_left_ / kUpdateNoticeSeconds);
    list.rounded_rect({card.x + 22.0f, card.y + card.h - 12.0f, (card.w - 44.0f) * left, 3.0f}, 1.5f,
                      t.accent.with_alpha(0.8f));
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::draw_launch(Canvas &c)
{
    const Theme &t = th();
    const float p = launch_.progress();
    // The menu dims, the game's cover steps forward with its name, then everything goes dark.
    const float black = tween::cubic_in_out((p - 0.50f) / 0.50f);
    const float veil = std::max(0.88f * tween::cubic_out(p / 0.22f), black);
    c.list.rounded_rect(kScreen, 0.0f, t.scrim.with_alpha(veil));

    const float leave = 1.0f - tween::cubic_in_out((p - 0.62f) / 0.38f);
    // The cover flies from where it was pressed to the middle, a little past its size before it
    // rests; with reduced motion it is simply there.
    const Rect rest{790.0f, 250.0f, 340.0f, 340.0f};
    const float fly = look().reduce_motion ? 1.0f : tween::back_out(p / 0.50f);
    const Rect art{tween::lerp(launch_from_.x, rest.x, fly), tween::lerp(launch_from_.y, rest.y, fly),
                   tween::lerp(launch_from_.w, rest.w, fly), tween::lerp(launch_from_.h, rest.h, fly)};
    const float corner = t.radius_card + (t.radius_panel - t.radius_card) * tween::clamp01(fly);
    c.list.push_opacity(leave * (look().reduce_motion ? tween::clamp01(p / 0.18f) : 1.0f));
    c.list.shadow({art.x - 30.0f, art.y - 10.0f, art.w + 60.0f, art.h + 60.0f}, 60.0f, 120.0f,
                  t.highlight.with_alpha(0.16f * tween::clamp01(fly)));
    cover(c, launch_cover_, art, corner, 1.6f);
    c.list.pop_opacity();
    // Its name comes in under it once it is nearly there.
    const float named = tween::cubic_out((p - 0.22f) / 0.30f);
    c.list.push_opacity(named * leave);
    c.list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - named) * 18.0f * motion());
    kicker(c, tr("STARTING"), 960.0f, 664.0f, t.accent, 1200.0f, Align::center);
    {
        const Heavy heavy(c);
        text_fit(c, launch_title_, 960.0f, 730.0f, t.size_display, t.title, 1400.0f, Align::center);
    }
    c.list.pop_transform();
    c.list.pop_opacity();
}

// The opening's two parts, in seconds: the launch picture with the mark over it, then the picture
// leaving while the mark goes to its place on the rail. With reduced motion the picture is held a
// moment and fades.
float Launcher::opening_reveal() const
{
    return look().reduce_motion ? 0.30f : 1.05f;
}

float Launcher::opening_handover() const
{
    return look().reduce_motion ? 0.45f : 0.70f;
}

void Launcher::draw_opening(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};
    const float reveal = opening_reveal();
    const float handover = opening_handover();
    const float leave = tween::cubic_in_out((opening_ - reveal) / handover);
    // The console's own launch picture, as the console showed it. It is the menu's artwork before
    // the menu dims it, so it moves to where that artwork lies and the two are one as it leaves.
    const float nearer = tween::cubic_in_out(opening_ / (reveal + handover)) * motion();
    const Rect art = backdrop_.art();
    const Rect picture{tween::lerp(kScreen.x, art.x, nearer), tween::lerp(kScreen.y, art.y, nearer),
                       tween::lerp(kScreen.w, art.w, nearer), tween::lerp(kScreen.h, art.h, nearer)};
    list.push_opacity(1.0f - leave);
    list.image(textures_.splash(), picture, {0.0f, 0.0f, 1.0f, 1.0f}, white);
    list.pop_opacity();
    if (look().reduce_motion || textures_.brand() == 0)
        return;

    // The mark comes forward over the picture's light, then goes to its place on the rail.
    const float shown = tween::cubic_out((opening_ - 0.12f) / 0.40f);
    const float settle = tween::back_out(tween::clamp01((opening_ - 0.12f) / 0.55f));
    const float fly = tween::cubic_in_out((opening_ - reveal) / (handover * 0.68f));
    const float stay = 1.0f - fly;
    const float side = tween::lerp(232.0f * (0.84f + 0.16f * settle), 64.0f, fly);
    const float cx = tween::lerp(960.0f, (layout::kRail - 64.0f) * 0.5f + 32.0f, fly);
    const float cy = tween::lerp(500.0f, 72.0f, fly);
    const Rect mark{cx - side * 0.5f, cy - side * 0.5f, side, side};
    const float corner = tween::lerp(54.0f, 18.0f, fly);
    // A pool of dusk around it, so the mark and its name read on any picture.
    list.shadow({cx - 380.0f, cy - 270.0f, 760.0f, 680.0f}, 330.0f, 420.0f, t.base.with_alpha(0.46f * shown * stay));
    list.shadow({mark.x - 14.0f, mark.y - 14.0f, mark.w + 28.0f, mark.h + 28.0f}, corner + 14.0f, 80.0f,
                t.highlight.with_alpha(0.34f * shown * stay));
    list.shadow({mark.x + 4.0f, mark.y + side * 0.10f, mark.w - 8.0f, mark.h - 4.0f}, corner, side * 0.20f,
                Color{0.0f, 0.0f, 0.0f, 0.55f * shown});
    list.rounded_image(textures_.brand(), mark, {0.0f, 0.0f, 1.0f, 1.0f}, corner, white.with_alpha(shown));
    list.bordered_rect(mark, corner, white.with_alpha(0.0f), 1.5f, white.with_alpha(0.16f * shown * stay));
    // Two rings of light leave it, one after the other.
    for (const float begun : {0.40f, 0.62f})
    {
        const float out = tween::clamp01((opening_ - begun) / 0.62f);
        if (out <= 0.0f || out >= 1.0f)
            continue;
        const float grown = 46.0f * tween::cubic_out(out);
        list.bordered_rect({mark.x - grown, mark.y - grown, mark.w + 2.0f * grown, mark.h + 2.0f * grown},
                           corner + grown, white.with_alpha(0.0f), 2.0f,
                           t.accent.with_alpha(0.55f * std::sin(3.14159265f * out) * (1.0f - out) * stay));
    }

    // Its name under it, until the mark leaves.
    const float named = tween::cubic_out((opening_ - 0.38f) / 0.45f);
    const float gone = tween::cubic_out((opening_ - reveal) / 0.22f);
    list.push_opacity(named * (1.0f - gone));
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - named) * 16.0f - gone * 10.0f);
    {
        const Heavy heavy(c);
        text(c, "PROSPEROEDEN", 960.0f, 706.0f, 40.0f, t.title, Align::center, 12.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

void Launcher::draw(gfx::DrawList &list)
{
    Canvas c{list, fonts_, textures_, backdrop_, time_};
    backdrop_.draw(list, textures_, 0.0f);
    draw_scrims(c);

    const bool launching = !selected_game_.empty();
    const float zoom =
        launching ? 1.0f + 0.045f * tween::cubic_in_out(launch_.progress()) * motion() : 1.0f;
    list.push_transform(zoom, 960.0f, 540.0f, 0.0f, 0.0f);
    if (transition_.running)
    {
        // The old screen leaves toward the rail as the new one arrives from the other side. The
        // rail and the artwork stay where they are.
        const float p = transition_.progress();
        const float e = tween::cubic_in_out(p);
        const float direction = forward_ ? 1.0f : -1.0f;
        list.push_opacity(1.0f - tween::cubic_out(p / 0.55f));
        list.push_transform(1.0f, 0.0f, 0.0f, -direction * 64.0f * e * motion(), 0.0f);
        draw_screen(c, leaving_);
        list.pop_transform();
        list.pop_opacity();
        list.push_opacity(tween::cubic_out((p - 0.2f) / 0.8f));
        list.push_transform(1.0f, 0.0f, 0.0f, direction * 64.0f * (1.0f - e) * motion(), 0.0f);
        draw_screen(c, screen_);
        list.pop_transform();
        list.pop_opacity();
    }
    else
    {
        draw_screen(c, screen_);
    }

    // A game's settings are a page of their own over the Library, beside the rail; its button
    // mapping is a dialog over that page.
    const float opened = tween::clamp01(modal_open_.value);
    const bool mapping_of_game = modal_shown_ == Modal::mapping && mapping_for_game_;
    const bool page = modal_shown_ == Modal::game || modal_shown_ == Modal::mods ||
                      modal_shown_ == Modal::game_options || mapping_of_game;
    if (page)
        draw_game_page(c, opened);
    draw_rail(c);
    draw_clock(c);
    if (modal_shown_ != Modal::none && (!page || mapping_of_game))
    {
        list.rounded_rect(kScreen, 0.0f, th().scrim.with_alpha(0.72f * opened));
        if (modal_shown_ == Modal::mapping)
            draw_mapping(c, opened);
        else if (modal_shown_ == Modal::menu_mapping)
            draw_menu_mapping(c, opened);
        else if (modal_shown_ == Modal::hud)
            draw_hud(c, opened);
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

    if (opening_on_)
    {
        draw_opening(c);
        return;
    }
    // The launcher arrives out of the dark (after the opening it is there already).
    const float arrival = 1.0f - tween::cubic_out(intro_ / (first_start_ ? 0.7f : 0.45f));
    if (arrival > 0.0f)
        list.rounded_rect(kScreen, 0.0f, Color{0.0f, 0.0f, 0.0f, arrival});
}

} // namespace pe::ui
