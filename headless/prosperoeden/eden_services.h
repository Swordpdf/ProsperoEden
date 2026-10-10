// SPDX-License-Identifier: GPL-3.0-or-later
// What the launcher screens ask of the app, answered from the console: the games folder,
// the settings file, Eden's metadata reader.
#pragma once

#include "pe/ui/services.hpp"
#include "remote/remote.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class EdenServices final : public pe::ui::Services {
public:
    // launch_error: why the game chosen last time did not start (empty: nothing to report).
    explicit EdenServices(std::string launch_error, Eden::Multiplayer::RoomClient& rooms);
    // Downloads from the download sources stop while a game runs (the menu is gone then).
    ~EdenServices() override;

    pe::ui::Home home() override;
    std::string clock() override;
    unsigned controllers() override;
    std::string version() override;

    std::vector<pe::ui::Game> games() override;
    std::string game_path(const std::string& file) override;
    bool game_exists(const std::string& file) override;
    bool take_update(pe::ui::UpdateOffer* offer) override;
    bool start_update() override;
    pe::ui::UpdateStatus update_status() override;
    void cancel_update() override;
    bool apply_update() override;
    void finish_update() override;
    bool docked(std::uint64_t title_id) override;
    bool set_docked(std::uint64_t title_id, bool docked) override;
    pe::ui::GameSettings game_settings(std::uint64_t title_id) override;
    bool set_game_settings(std::uint64_t title_id, const pe::ui::GameSettings& settings) override;

    pe::ui::Preferences preferences() override;
    bool set_preferences(const pe::ui::Preferences& preferences) override;
    const std::vector<std::string>& resolution_labels() override;
    const std::vector<std::string>& resolution_keys() override;
    const std::vector<std::string>& filter_labels() override;
    int frame_gen_state() override;
    const std::vector<std::string>& language_labels() override;
    std::string language_region(int language) override;
    std::string setup_details() override;

    bool folders(const std::string& directory, std::vector<std::string>* names) override;
    pe::ui::FolderInfo folder_info(const std::string& directory) override;
    std::string files_folder() override;
    std::string saved_files_folder() override;
    std::string default_files_folder() override;
    bool set_files_folder(const std::string& directory) override;
    int filesystem_access() override;

    std::vector<pe::ui::Profile> profiles() override;
    bool choose_profile(int index) override;
    int add_profile() override;
    bool rename_profile(int index, int step) override;
    bool remove_profile(int index) override;

    bool save_transfer_available() override;
    pe::ui::SaveSource save_import_source(std::uint64_t title_id) override;
    bool save_import(std::uint64_t title_id, std::string* message) override;
    bool save_export(std::uint64_t title_id, std::string* message) override;

    std::vector<pe::ui::Mod> mods(std::uint64_t title_id) override;
    bool set_mod_enabled(std::uint64_t title_id, const std::string& name, bool enabled) override;
    bool set_cheat_enabled(std::uint64_t title_id, const std::string& mod, const std::string& cheat,
                           bool enabled) override;
    bool mods_enabled(std::uint64_t title_id) override;
    bool set_mods_enabled(std::uint64_t title_id, bool enabled) override;
    std::string mods_folder(std::uint64_t title_id) override;
    bool make_mods_folder(std::uint64_t title_id) override;

    pe::ui::Sources sources() override;
    void refresh_sources() override;
    bool download(const pe::ui::Game& game, int source, bool first) override;
    bool cancel_download(const std::string& file) override;
    std::vector<pe::ui::Download> downloads() override;
    bool delete_game(const pe::ui::Game& game, std::string* message) override;

    bool save_sync_wanted(const std::string& file) override;
    void start_save_sync(const std::string& file, bool before) override;
    pe::ui::SaveSync save_sync() override;
    void choose_save_data(pe::ui::SaveChoice choice) override;
    void end_save_sync() override;
    void stop_save_sync() override;
    void will_play(const std::string& file) override;

    pe::ui::SaveSyncSetup save_sync_setup() override;
    std::vector<pe::ui::PairServer> pair_servers() override;
    bool start_pairing(int profile, int server) override;
    pe::ui::PairingStatus pairing() override;
    void cancel_pairing() override;
    bool unlink_profile(int profile) override;

    bool load_image(const std::string& path, pe::gfx::Image* image) override;

    Eden::Multiplayer::Snapshot room_status() override;
    Eden::Multiplayer::Connection room_connection() override;
    bool connect_room(const Eden::Multiplayer::Connection&) override;
    void leave_room() override;
    std::optional<std::string> room_text(int, const std::string&, const std::atomic<bool>&) override;
    bool can_launch_game() override;
private:
    Eden::Multiplayer::RoomClient& rooms_;
    int user_ = -1; // the PS5 user in front when the menu opened (negative: unknown)
    std::string launch_error_;
    std::string setup_; // what is missing from keys and firmware; empty when ready
    std::mutex bridge_; // Eden's metadata reader keeps state between calls: one caller at a time
    // The download sources' games as titles (Remote::Titles), read again when their lists changed:
    // the menu asks for its downloads four times a second.
    std::vector<Eden::Remote::Title> Titles();
    // The key of the title a source's game is in, without copying the titles.
    std::string TitleKeyOf(const std::string& source, const std::string& id);
    void RefreshTitles(); // with titles_lock_ held
    std::mutex titles_lock_;
    std::vector<Eden::Remote::Title> titles_;
    std::uint64_t titles_generation_ = 0;

    // The save sync (remote/save_sync.h): one at a time, on a thread of its own. Before a game
    // starts it syncs that game; after one ended, every game still waiting for that (pending.json).
    struct SyncJob {
        std::string profile; // its ID
        std::string file;    // the game's file in roms/
    };
    void StartSync(std::vector<SyncJob> jobs, bool before);
    std::thread sync_thread_;
    std::mutex sync_lock_;
    std::condition_variable sync_changed_;
    pe::ui::SaveSync sync_;
    std::optional<pe::ui::SaveChoice> sync_choice_;
    std::atomic<bool> sync_stop_{false};

    // A pairing (Settings > Save sync), on a thread of its own: it asks the server for a code and
    // then whether it was approved, until it was, or the code expired, or it is cancelled.
    std::thread pair_thread_;
    std::mutex pair_lock_;
    pe::ui::PairingStatus pair_;
    std::chrono::steady_clock::time_point pair_until_;
    std::atomic<bool> pair_stop_{false};
};
