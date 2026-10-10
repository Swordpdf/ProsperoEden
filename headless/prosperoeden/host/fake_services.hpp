// ProsperoEden - Sample data for the launcher preview on a PC (no console, no game files).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/core/strings.hpp"
#include "pe/ui/services.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pe::host
{

// A made-up library: invented titles with drawn covers, settings kept in memory.
class FakeServices final : public ui::Services
{
  public:
    // covers_directory receives one drawn cover per sample game.
    explicit FakeServices(const std::string &covers_directory);

    // What the preview varies between pictures.
    bool setup_ready = true;
    std::string launch_error;
    std::string crash_report; // the previous run's crash report, when it left one
    bool has_history = true;
    bool import_available = true;
    ui::SaveSource import_source = ui::SaveSource::ryujinx;
    unsigned connected_controllers = 0b0011;
    Eden::Multiplayer::Snapshot room;
    Eden::Multiplayer::Snapshot room_status() override { return room; }
    Eden::Multiplayer::Connection room_connection() override { return {"127.0.0.1", 24872, "PlayerPS5", ""}; }
    bool connect_room(const Eden::Multiplayer::Connection&) override {
        room = {Eden::Multiplayer::Phase::Connecting, {}, {}, {}};
        return true;
    }
    void leave_room() override { room = {}; }
    bool can_launch_game() override {
        return room.phase == Eden::Multiplayer::Phase::Idle || room.phase == Eden::Multiplayer::Phase::Connected;
    }

    ui::Home home() override;
    std::string clock() override
    {
        return "21:47";
    }
    unsigned controllers() override
    {
        return connected_controllers;
    }
    std::string version() override
    {
        return "v1.000.040";
    }
    // The people who play on the preview's console.
    std::vector<ui::Profile> people{{"Eden", true}};
    std::vector<ui::Profile> profiles() override
    {
        return people;
    }
    bool choose_profile(int index) override
    {
        if (index < 0 || index >= static_cast<int>(people.size()))
            return false;
        for (std::size_t i = 0; i < people.size(); ++i)
            people[i].playing = static_cast<int>(i) == index;
        return true;
    }
    int add_profile() override
    {
        if (people.size() >= 8)
            return -1;
        static constexpr const char *kNames[] = {"Marina", "Player 2", "Player 3", "Player 4",
                                                 "Player 5", "Player 6", "Player 7", "Player 8"};
        people.push_back({kNames[people.size() - 1], false});
        return static_cast<int>(people.size()) - 1;
    }
    bool rename_profile(int index, int step) override
    {
        if (index < 0 || index >= static_cast<int>(people.size()))
            return false;
        static constexpr const char *kNames[] = {"Eden", "Marina", "Player 1", "Player 2"};
        int at = 0;
        for (int i = 0; i < 4; ++i)
            if (people[static_cast<std::size_t>(index)].name == kNames[i])
                at = i;
        people[static_cast<std::size_t>(index)].name = kNames[((at + step) % 4 + 4) % 4];
        return true;
    }
    bool remove_profile(int index) override
    {
        if (people.size() < 2 || index < 0 || index >= static_cast<int>(people.size()) ||
            people[static_cast<std::size_t>(index)].playing)
            return false;
        people.erase(people.begin() + index);
        return true;
    }
    // A newer release for the preview to offer: handed over once. Installing it goes through its
    // phases a step per look (the launcher looks once a frame); update_fails ends it in a failure.
    std::string update_version;
    bool update_installable = true;
    // The sample release's notes, as the catalog gives them; empty: a release without notes.
    std::string update_notes =
        "Warning: This is a pre-release. Your jailbreak environment must provide a local ELF loader on TCP port "
        "9021.\n\nThis version makes the menu faster to open and adds a few things players asked for.\n\n"
        "New features\n- Release notes in the update dialog, so you can see what changes before you update.\n"
        "- A clock in the game menu.\n- Faster scrolling in long game lists, with a letter jump on L2 and R2.\n\n"
        "Bug fixes and improvements\n- The menu opens about a second sooner on a cold start.\n"
        "- Covers no longer flicker when a list scrolls quickly.\n- A crash when a controller was disconnected "
        "during the loading screen is fixed.\n- Saves are written to a temporary file first, so a power loss "
        "while saving no longer damages them.\n- Better text fitting in Greek, Hungarian and Finnish.\n\n"
        "Note: Your settings, saves and profiles stay where they are; nothing needs to be moved.\n\n"
        "Upgrading\nCopy the PPSA99008 folder from the ZIP over the one you have, or install it from the "
        "update dialog. See the README for every step: "
        "https://github.com/blackbearreloaded/ProsperoEden/blob/main/README.md#updating\n\n"
        "Thanks\nThank you to everyone who tested this release and reported what they found.";
    bool update_notes_truncated = false;
    bool update_fails = false;
    int update_steps = -1; // looks since start_update; -1: not begun
    bool update_cancelled = false;
    bool take_update(ui::UpdateOffer *offer) override
    {
        if (update_version.empty())
            return false;
        offer->version = update_version;
        offer->size = 38215192;
        offer->installable = update_installable;
        offer->notes = update_notes;
        offer->notes_truncated = update_notes_truncated;
        update_version.clear();
        return true;
    }
    bool start_update() override
    {
        update_steps = 0;
        update_cancelled = false;
        return true;
    }
    ui::UpdateStatus update_status() override
    {
        ui::UpdateStatus status;
        if (update_steps < 0)
            return status;
        const int step = update_steps++;
        constexpr std::uint64_t kSize = 38215192;
        if (update_cancelled)
            status.phase = step < 20 ? ui::UpdatePhase::starting : ui::UpdatePhase::cancelled;
        else if (step < 40)
            status.phase = ui::UpdatePhase::starting;
        else if (step < 400)
        {
            status.phase = ui::UpdatePhase::downloading;
            status.total = kSize;
            status.done = kSize * static_cast<std::uint64_t>(step - 40) / 360;
        }
        else if (update_fails)
        {
            status.phase = ui::UpdatePhase::failed;
            status.error = "Download failed: the connection was lost";
        }
        else if (step < 520)
            status.phase = ui::UpdatePhase::unpacking;
        else
            status.phase = ui::UpdatePhase::ready;
        return status;
    }
    void cancel_update() override
    {
        update_cancelled = true;
        update_steps = 0;
    }
    bool apply_update() override
    {
        return update_steps >= 0;
    }
    void finish_update() override
    {
        update_steps = -1;
    }
    // Game files the preview takes away from the folder, as a player would by deleting them.
    std::vector<std::string> removed;
    bool game_exists(const std::string &file) override
    {
        return std::find(removed.begin(), removed.end(), file) == removed.end();
    }
    std::vector<ui::Game> games() override
    {
        std::vector<ui::Game> present;
        for (const ui::Game &game : games_)
            if (game_exists(game.file))
                present.push_back(game);
        if (has_server)
            for (ui::Game game : remote_)
            {
                // With Office away, only Home's games are there.
                if (server_fails)
                    game.sources.erase(std::remove(game.sources.begin(), game.sources.end(), "Office"),
                                       game.sources.end());
                if (!game.sources.empty())
                    present.push_back(game);
            }
        std::sort(present.begin(), present.end(),
                  [](const ui::Game &a, const ui::Game &b) { return a.name < b.name; });
        return present;
    }
    std::string game_path(const std::string &file) override
    {
        return "/games/roms/" + file;
    }
    bool docked(std::uint64_t title_id) override;
    bool set_docked(std::uint64_t title_id, bool docked) override;
    ui::GameSettings game_settings(std::uint64_t) override
    {
        return game_settings_;
    }
    bool set_game_settings(std::uint64_t, const ui::GameSettings &settings) override
    {
        game_settings_ = settings;
        return true;
    }
    ui::Preferences preferences() override
    {
        return preferences_;
    }
    bool set_preferences(const ui::Preferences &preferences) override
    {
        preferences_ = preferences;
        return true;
    }
    const std::vector<std::string> &resolution_labels() override;
    const std::vector<std::string> &resolution_keys() override;
    const std::vector<std::string> &filter_labels() override;
    const std::vector<std::string> &language_labels() override;
    std::string language_region(int language) override;
    std::string setup_details() override;
    bool folders(const std::string &directory, std::vector<std::string> *names) override;
    ui::FolderInfo folder_info(const std::string &directory) override;
    std::string files_folder() override
    {
        return "/mnt/ext1/eden";
    }
    std::string saved_files_folder() override
    {
        return saved_folder_;
    }
    std::string default_files_folder() override
    {
        return "/data/prosperoeden";
    }
    bool set_files_folder(const std::string &directory) override
    {
        saved_folder_ = directory;
        return true;
    }
    int filesystem_access() override
    {
        return 0;
    }
    bool save_transfer_available() override
    {
        return import_available;
    }
    ui::SaveSource save_import_source(std::uint64_t) override
    {
        return import_source;
    }
    bool save_import(std::uint64_t, std::string *message) override
    {
        *message = tr("Imported. The save it replaced was backed up.");
        return true;
    }
    bool save_export(std::uint64_t, std::string *message) override
    {
        *message = fill(tr("Exported to {0}."), {"save-export/0100A00B00003000-20261001-213000"});
        return true;
    }
    // Sample mods, for two of the games: three, the second switched off, the third with cheats
    // chosen one by one (its two frame rates replace each other); none when has_mods is cleared.
    bool has_mods = true;
    std::vector<ui::Mod> mods(std::uint64_t) override;
    bool set_mod_enabled(std::uint64_t, const std::string &name, bool enabled) override;
    bool set_cheat_enabled(std::uint64_t, const std::string &mod, const std::string &cheat,
                           bool enabled) override;
    // One Mods switch for every sample game.
    bool mods_enabled(std::uint64_t) override
    {
        return mods_enabled_;
    }
    bool set_mods_enabled(std::uint64_t, bool enabled) override
    {
        mods_enabled_ = enabled;
        return true;
    }
    std::string mods_folder(std::uint64_t) override
    {
        return "mods/0100A00B00003000/";
    }
    bool make_mods_folder(std::uint64_t) override
    {
        return true;
    }
    bool load_image(const std::string &path, gfx::Image *image) override
    {
        return gfx::load_tga(path, image);
    }

    // Two download sources, "Home" with three games and "Office" with two (one of them Home's
    // too); a download moves a step each time the queue is looked at (the launcher looks four
    // times a second) and the game then joins the library. server_fails: Office cannot be
    // reached; download_fails: a download stops part way.
    bool has_server = false;
    bool server_fails = false;
    bool download_fails = false;
    ui::Sources sources() override;
    void refresh_sources() override
    {
        ++generation_;
    }
    bool download(const ui::Game &game, int source, bool first) override;
    bool cancel_download(const std::string &key) override;
    std::vector<ui::Download> downloads() override;
    // The queue as it is, without moving it on (for the preview to look at).
    const std::vector<ui::Download> &downloads_now() const
    {
        return queue_;
    }
    bool delete_game(const ui::Game &game, std::string *message) override;

    // The profile syncs its save data (save_sync_on); a sync takes a few looks of the launcher.
    // save_sync_conflict: both sides changed; save_sync_fails: the server cannot be reached.
    // played(): a game just ended, its save data goes up once the menu is back.
    bool save_sync_on = false;
    bool save_sync_conflict = false;
    bool save_sync_fails = false;
    bool save_sync_too_old = false; // the server runs RomM 4.9.2
    bool save_sync_wanted(const std::string &) override
    {
        return save_sync_on;
    }
    void start_save_sync(const std::string &file, bool before) override;
    ui::SaveSync save_sync() override;
    void choose_save_data(ui::SaveChoice choice) override;
    void end_save_sync() override
    {
        if (sync_.stage == ui::SaveSyncStage::done || sync_.stage == ui::SaveSyncStage::failed)
            sync_ = {};
    }
    void stop_save_sync() override
    {
        sync_ = {};
    }
    void played(const std::string &name);

    // Settings > Save sync: two profiles (the second paired with Home), two servers to pair with;
    // a pairing is approved after a few looks.
    ui::SaveSyncSetup save_sync_setup() override;
    std::vector<ui::PairServer> pair_servers() override
    {
        return {{"Home", "http://192.168.1.20:3000"}, {"Office", "https://games.example.org"}};
    }
    bool start_pairing(int profile, int server) override;
    ui::PairingStatus pairing() override;
    void cancel_pairing() override
    {
        pair_ = {};
    }
    bool unlink_profile(int profile) override;

  private:
    ui::PairingStatus pair_;
    int pair_looks_ = 0;
    int pair_profile_ = 0;
    std::vector<std::string> linked_{"", "kids @ http://192.168.1.20:3000"};
    ui::SaveSync sync_;
    int sync_looks_ = 0;
    std::vector<ui::Game> remote_; // the sources' games not on the console
    std::vector<ui::Download> queue_;
    // Where a download that goes on was (verifying reads up to there first), by its key.
    std::vector<std::pair<std::string, std::uint64_t>> going_on_;
    std::uint64_t generation_ = 1;
    std::vector<ui::Game> games_;
    std::vector<std::uint64_t> handheld_;
    ui::GameSettings game_settings_;
    ui::Preferences preferences_;
    std::string saved_folder_;
    std::vector<std::uint64_t> modded_; // the games that have the sample mods
    std::vector<std::string> mods_off_{"Sharper textures"};
    std::vector<std::string> cheats_on_{"60 FPS"};
    bool mods_enabled_ = true;
};

} // namespace pe::host
