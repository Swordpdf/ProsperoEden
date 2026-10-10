// ProsperoEden - What the launcher screens ask of the application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/image.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <optional>
#include "../../../multiplayer.h"

namespace pe::ui
{

// One pressed button (a first press, or a repeat of a held direction or shoulder button).
enum class Key : std::uint8_t
{
    cross,
    circle,
    square,
    triangle,
    options,
    l1,
    r1,
    up,
    down,
    left,
    right,
};

// A newer release of the app, listed on homebrew.page.
struct UpdateOffer
{
    std::string version;      // its name: "v1.000.060"
    std::uint64_t size = 0;   // its download in bytes; 0 when not known
    bool installable = false; // the app can install it itself (otherwise it is only announced)
    std::string notes;        // what the developer wrote on the release (plain text); may be empty
    bool notes_truncated = false; // the catalog cut the notes; the rest is on the app's page
};
// Where installing it is.
enum class UpdatePhase : std::uint8_t
{
    idle,
    starting,
    downloading,
    unpacking,
    ready,
    applying,
    cancelled,
    failed,
};
struct UpdateStatus
{
    UpdatePhase phase = UpdatePhase::idle;
    std::uint64_t done = 0;
    std::uint64_t total = 0; // 0 while not known
    std::string error;       // why it failed (English, technical)
};

// A game file in the games folder.
struct Game
{
    std::string name;
    std::string format; // "NSP" or "XCI"
    std::string size;   // "1.2 GB"
    std::string file;   // its name in the games folder
    std::string cover;  // image path; empty without cover art
    std::uint64_t title_id = 0;
    std::string addons;        // "Update 1.2.0, 2 DLC"; empty without either
    std::string addons_short;  // the same where there is little room: "v1.2.0, 2 DLC"
    std::string language;      // the language the game will use
    std::string language_note; // set when that is not the chosen one
    // Its mods, and how many of them are switched on. The launcher counts them (Services::mods)
    // when it takes the list and whenever they change. With the game's Mods switch off
    // (Services::mods_enabled) none is on.
    int mods = 0;
    int mods_on = 0;
    bool mods_enabled = true;
    // The download sources that have the game (their names, in the order they are set up in): it
    // can be downloaded from any of them, again once deleted. Empty for a game only on the console.
    std::vector<std::string> sources;
    // Which game it is on the sources (the same on all of them; Download::key); empty without.
    std::string key;
    // Not on the console yet: on its sources only. Its file is the name it gets in roms/.
    bool remote = false;
};

// A download source: a place on the network with Switch games (headless/remote/remote.h).
struct SourceInfo
{
    std::string name;
    std::string address;
    bool refreshing = false; // its game list is being read
    bool online = false;     // the last look at it worked
    std::string error;       // why the last look failed, or what is wrong with its entry (English)
    int games = 0;
};
struct Sources
{
    bool configured = false; // sources.json names at least one source
    std::string setup_file;  // where sources.json goes
    std::string error;       // what is wrong with sources.json itself (English)
    std::vector<SourceInfo> list;
    // Changes when a source's games, their covers or the downloaded files changed: the Library
    // reads its list again.
    std::uint64_t generation = 0;
    int ftp_port = 2121; // the console's FTP server, which writes the downloads; it has to run
};

// The sync of a game's save data with the profile's save store (save-sync.json): before the game
// starts, and once the menu is back after it ended.
enum class SaveSyncStage : std::uint8_t
{
    idle,
    working,
    conflict, // both changed since they were last the same: the player chooses (choose_save_data)
    done,
    failed,
};
enum class SaveSyncOutcome : std::uint8_t
{
    same,       // nothing to do
    uploaded,   // the console's save data went to the server
    downloaded, // the server's replaced the console's (the console's is in the backups)
    kept,       // a conflict left as it is
    no_game,    // the server does not have the game, so it cannot keep its save data
};
struct SaveSync
{
    SaveSyncStage stage = SaveSyncStage::idle;
    bool before = true;      // before the game starts; false: after it ended
    std::string game;        // its name
    std::string profile;     // the profile's name
    std::string server;      // where it syncs to: "alex on http://nas:3000"
    SaveSyncOutcome outcome = SaveSyncOutcome::same; // done: what happened
    std::string error;       // failed: why (English, technical)
    // Failed because the server is older than the save sync takes (shown until the player confirms
    // it): its version and the oldest one taken.
    bool too_old = false;
    std::string server_version;
    std::string needed_version;
    // A conflict: when each was last changed (seconds since 1970, 0: not known), and the device
    // that changed the server's.
    std::int64_t console_time = 0;
    std::int64_t server_time = 0;
    std::string server_device;
};
// What the player chose in a conflict.
enum class SaveChoice : std::uint8_t
{
    console, // the console's goes to the server
    server,  // the server's replaces the console's
    neither, // both stay as they are
};

// Settings > Save sync: each profile and where it keeps its save data (save-sync.json).
struct SaveSyncProfile
{
    std::string name;
    bool current = false; // the one playing
    bool linked = false;  // its entry names a server
    std::string server;   // where: "player @ http://nas:3000" (who, when known)
    std::string note;     // what is wrong with its entry (English); empty when nothing
};
struct SaveSyncSetup
{
    std::vector<SaveSyncProfile> profiles;
    std::string error; // save-sync.json cannot be read (English): nothing syncs
    bool automatic = true;
    std::string file;  // where save-sync.json is, for editing it over FTP
};
// A server a profile can be paired with: a download source whose kind of server can pair.
struct PairServer
{
    std::string name;
    std::string address;
};
// A pairing: the code shown, approved on the server signed in as the profile's user.
enum class PairingStage : std::uint8_t
{
    idle,
    asking,  // the server is asked for a code
    waiting, // the code shows: approve it on the server
    done,    // approved: the profile syncs with that user
    failed,  // see error; denied and expired say so
};
struct PairingStatus
{
    PairingStage stage = PairingStage::idle;
    std::string profile;  // its name
    std::string server;   // the server's name
    std::string code;     // to tell the request on the server's page
    std::string address;  // the page that approves it, the code in it (the QR code)
    int seconds_left = 0; // until the code expires
    std::string user;     // done: who it signed in as
    bool denied = false;  // failed: the player said no on the server
    bool expired = false; // failed: nobody approved it in time
    std::string error;    // failed otherwise (English)
};

// A game in the download queue.
enum class DownloadState : std::uint8_t
{
    queued,
    downloading,
    verifying, // one that goes on from where it was: what its files have is read first, for the check
               // of their contents (done goes up to where it goes on)
    failed,
};
struct Download
{
    std::string key;     // which game it is (Game::key)
    std::string file;    // its file in roms/ once downloaded (from this source)
    std::string name;
    std::string source;  // the name of the source it comes from
    std::string cover;
    DownloadState state = DownloadState::queued;
    std::uint64_t done = 0;
    std::uint64_t total = 0; // 0 when not known
    std::uint64_t rate = 0;  // bytes a second while it downloads; 0 when not known yet
    std::string error;       // why it failed (English, technical)
};

struct Recent
{
    std::string file;
    std::string title;
    std::string cover;
};

// The home screen's content.
struct Home
{
    bool setup_ready = false;
    std::string status;    // a setup or launch problem; empty when there is none
    bool launch_failed = false; // the status is about the game that just failed to start
    std::string last_file; // the last game played; empty before the first one
    bool last_exists = false;
    std::string last_title;
    std::string last_caption;
    bool last_caption_warning = false; // the caption says what is wrong with the game
    std::string last_cover;
    // What the last game comes with, when it can be started: its title ID, its update and DLC
    // (as Game::addons) and the language it will use. Its mods are counted by the launcher.
    std::uint64_t last_title_id = 0;
    std::string last_addons;
    std::string last_language;
    int last_mods = 0;
    int last_mods_on = 0;
    std::vector<Recent> recents; // at most four
    std::string system_status;
};

// Button mapping: which DualSense button presses each of the game's buttons, on every controller.
// The game's buttons: A B X Y L R ZL ZR + - and the two stick presses; the DualSense buttons:
// Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create Touchpad. A mapping never names
// a DualSense button twice (headless/button_mapping.h).
constexpr int kGameButtons = 12;
constexpr int kPadButtons = 13;
using ButtonMapping = std::array<int, kGameButtons>;
constexpr ButtonMapping kDefaultMapping = {1, 0, 3, 2, 4, 5, 6, 7, 10, 12, 8, 9};
// The game button takes the DualSense button; the one that had it gets this one's old one.
inline ButtonMapping assign_button(ButtonMapping mapping, int game, int pad)
{
    for (int other = 0; other < kGameButtons; ++other)
        if (other != game && mapping[static_cast<std::size_t>(other)] == pad)
            mapping[static_cast<std::size_t>(other)] = mapping[static_cast<std::size_t>(game)];
    mapping[static_cast<std::size_t>(game)] = pad;
    return mapping;
}

// One of the people who play on this console.
struct Profile
{
    std::string name;
    bool playing = false; // games start as this one
};

struct Preferences
{
    bool hud = true;
    int volume = 100; // game volume, 0-100
    bool mute = false;
    bool detailed_logging = false;
    bool immediate_logs = false; // every log line written as it comes; from the next start
    int renderer = 1; // 0 OpenGL, 1 Vulkan
    int resolution = 2;
    int filter = 0;
    int refresh = 0; // the output while a game runs: 0 60 Hz, 1 120 Hz
    int output = 0;  // the size of the picture, menu and games: 0 1080p, 1 1440p, 2 2160p
    // Lossless Scaling frame generation (Vulkan only): between a game's own frames, show
    // frames generated from them, up to the output's rate.
    bool frame_gen = false;
    int frame_gen_target = 0;      // index into kFrameGenTargetKeys: 0 auto, then 60/90/120/144/240
    int frame_gen_multiplier = 0;  // index into kFrameGenMultiplierKeys: 2x, 3x, 4x
    bool vibration = true;
    int language = 0;
    int menu_volume = 70; // launcher sounds, 0-100
    // Accessibility: how the launcher itself is shown (theme.hpp, Look).
    bool large_text = false;
    bool high_contrast = false;
    bool reduce_motion = false;
    // Performance: speed against accuracy, for every game.
    bool block_list = false;    // compile the code of earlier sessions ahead
    bool async_shaders = false; // draw before a new shader is ready
    bool fast_gpu = false;      // the emulator's lowest GPU accuracy
    bool unsafe_cpu = false;    // inexact floating-point shortcuts
    bool unsafe_dma = false;    // unsafe DMA accuracy
    bool reactive_flushing = true;  // off is faster; some effects break
    bool skip_invalidation = false; // fewer invalidations of what the GPU caches hold
    ButtonMapping mapping = kDefaultMapping; // Settings > Controls > Button mapping
};

// What one game does differently from Settings (Library > Game settings). Each value is -1 while
// the game follows Settings; switches are 0 off, 1 on.
struct GameSettings
{
    int renderer = -1;   // 0 OpenGL, 1 Vulkan
    int resolution = -1; // index into Services::resolution_labels
    int filter = -1;     // index into Services::filter_labels
    int refresh = -1;    // 0 60 Hz, 1 120 Hz
    int hud = -1;        // FPS overlay
    int frame_gen = -1;  // Lossless Scaling frame generation (Vulkan only)
    int frame_gen_target = -1;      // index into kFrameGenTargetKeys
    int frame_gen_multiplier = -1;  // index into kFrameGenMultiplierKeys
    int volume = -1;     // game volume, 0-100
    int mute = -1;
    int vibration = -1;
    int language = -1;   // index into Services::language_labels
    int controller = -1; // 0 Pro Controller, 1 handheld, 2 two Joy-Cons, 3 left Joy-Con, 4 right Joy-Con
    bool own_mapping = false; // the game has a button mapping of its own
    ButtonMapping mapping = kDefaultMapping;
    // The Performance switches, in the order of Preferences: block list, async shaders, fast GPU,
    // unsafe CPU, unsafe DMA, reactive flushing, skip invalidation.
    std::array<int, 7> performance{-1, -1, -1, -1, -1, -1, -1};
};

// One cheat of a mod that lists several: each is chosen on its own.
struct Cheat
{
    std::string name;     // as its file names it
    bool enabled = false; // chosen: runs when its mod is on
};

// A mod of one game, from the game files folder's mods/<title ID>/.
struct Mod
{
    std::string name;    // its folder's name
    std::string kind;    // what it is made of: "Patch", "Files", "Cheats"
    bool enabled = true; // used when the game starts
    std::vector<Cheat> cheats; // its cheats when it lists several; empty for a single one
};

// Where a save to import was found, in the game files folder.
enum class SaveSource : std::uint8_t
{
    none,
    folder,  // save-import/<title ID>/, copied by hand
    ryujinx, // ryujinx/, a Ryujinx data folder
};

// What a folder holds, for Settings > Game files. Counts are -1 without the subfolder.
struct FolderInfo
{
    bool keys = false;
    int firmware = -1;
    int games = -1;
};

class Services
{
  public:
    virtual ~Services() = default;

    // ---- home ----
    virtual Home home() = 0;
    virtual std::string clock() = 0;   // "14:05"
    // The players (bit 0 is player 1) whose controller is connected right now.
    virtual unsigned controllers()
    {
        return 1u;
    }
    virtual std::string version() = 0; // "v1.000.040"
    // A newer release than this one is listed (asked once per launch): handed over once, when the
    // answer has come.
    virtual bool take_update(UpdateOffer *)
    {
        return false;
    }
    // Installing it (the offer must be installable): begin downloading and unpacking it beside the
    // app (false: it could not begin), where that is, stop it (nothing is changed before
    // apply_update), and once it is ready the go-ahead (true: the app must close now; its files
    // are replaced once it has). finish_update after a cancel or a failure.
    virtual bool start_update()
    {
        return false;
    }
    virtual UpdateStatus update_status()
    {
        return {};
    }
    virtual void cancel_update()
    {
    }
    virtual bool apply_update()
    {
        return false;
    }
    virtual void finish_update()
    {
    }

    // ---- library ----
    virtual std::vector<Game> games() = 0; // reads every game file: slow
    // The value the launcher hands back to start a game.
    virtual std::string game_path(const std::string &file) = 0;
    // Whether a game's file is still in the game files folder (one look at the file, no reading).
    virtual bool game_exists(const std::string &)
    {
        return true;
    }
    virtual bool docked(std::uint64_t title_id) = 0;
    virtual bool set_docked(std::uint64_t title_id, bool docked) = 0;
    virtual GameSettings game_settings(std::uint64_t title_id) = 0;
    virtual bool set_game_settings(std::uint64_t title_id, const GameSettings &settings) = 0;

    // ---- settings ----
    virtual Preferences preferences() = 0;
    virtual bool set_preferences(const Preferences &preferences) = 0;
    virtual const std::vector<std::string> &resolution_labels() = 0; // "1x (native)"
    virtual const std::vector<std::string> &resolution_keys() = 0;   // "1x"
    virtual const std::vector<std::string> &filter_labels() = 0;
    // Frame generation: 0 this build has none (its settings are not shown), 1 the user's
    // Lossless.dll is missing (they are shown greyed out), 2 ready.
    virtual int frame_gen_state()
    {
        return 0;
    }
    virtual const std::vector<std::string> &language_labels() = 0;
    virtual std::string language_region(int language) = 0;
    virtual std::string setup_details() = 0;

    // ---- game files ----
    // The subfolder names of a folder; false when it cannot be opened.
    virtual bool folders(const std::string &directory, std::vector<std::string> *names) = 0;
    virtual FolderInfo folder_info(const std::string &directory) = 0;
    virtual std::string files_folder() = 0;       // in use by this process
    virtual std::string saved_files_folder() = 0; // used from the next start; empty when none
    virtual std::string default_files_folder() = 0;
    virtual bool set_files_folder(const std::string &directory) = 0;
    virtual int filesystem_access() = 0; // 0: the whole filesystem

    // ---- save transfer: a game's save in from, or out to, a folder (not in every build) ----
    // ---- profiles: who is playing ----
    // Each profile keeps its own save data and its own recently played games. Empty: this build
    // has none to choose from.
    virtual std::vector<Profile> profiles()
    {
        return {};
    }
    // The profile games start with from now on.
    virtual bool choose_profile(int)
    {
        return false;
    }
    // A new profile, named by itself; its place in the list, or -1 (eight is the most).
    virtual int add_profile()
    {
        return -1;
    }
    // The next (step 1) or the one before (step -1) of the names a profile can take: the PS5
    // users signed in, then "Player 1" to "Player 8".
    virtual bool rename_profile(int, int)
    {
        return false;
    }
    // Takes a profile off the list; its save data stays on the console. Not the one playing.
    virtual bool remove_profile(int)
    {
        return false;
    }

    virtual bool save_transfer_available()
    {
        return false;
    }
    // What there is to import for the game.
    virtual SaveSource save_import_source(std::uint64_t)
    {
        return SaveSource::none;
    }
    // Each returns whether it was done, with what to tell the player in message.
    virtual bool save_import(std::uint64_t, std::string *)
    {
        return false;
    }
    virtual bool save_export(std::uint64_t, std::string *)
    {
        return false;
    }

    // ---- mods: patches, replacement files and cheats the player added for a game ----
    virtual std::vector<Mod> mods(std::uint64_t)
    {
        return {};
    }
    virtual bool set_mod_enabled(std::uint64_t, const std::string &, bool)
    {
        return false;
    }
    // One cheat of a mod that lists several. Choosing one of a group (two frame rates) takes the
    // other out: the list is read again afterwards.
    virtual bool set_cheat_enabled(std::uint64_t, const std::string &, const std::string &, bool)
    {
        return false;
    }
    // One switch for all of a game's mods (the Library's Mods switch), on unless turned off. The
    // mods keep their own switches behind it.
    virtual bool mods_enabled(std::uint64_t)
    {
        return true;
    }
    virtual bool set_mods_enabled(std::uint64_t, bool)
    {
        return false;
    }
    // Where a game's mods go, as the player would write it: "mods/0100.../".
    virtual std::string mods_folder(std::uint64_t)
    {
        return {};
    }
    // Makes that folder; false when it cannot be made.
    virtual bool make_mods_folder(std::uint64_t)
    {
        return false;
    }

    // ---- download sources: games on the network, downloaded to the games folder when wanted ----
    virtual Sources sources()
    {
        return {};
    }
    // Reads every source's game list again.
    virtual void refresh_sources()
    {
    }
    // Puts a game in the download queue, from one of its sources (an index into Game::sources):
    // first (to play it as soon as it is there) or last; a failed one is tried again. False when
    // the source no longer has it.
    virtual bool download(const Game &, int, bool)
    {
        return false;
    }
    // Takes a game (Game::key) out of the queue and deletes what it had downloaded.
    virtual bool cancel_download(const std::string &)
    {
        return false;
    }
    // The queue, in order.
    virtual std::vector<Download> downloads()
    {
        return {};
    }
    // Deletes a game that a source has from the console (Game::sources): its file and all of its
    // updates and DLC in updates/. Its save data and settings stay. Says what was done in message.
    virtual bool delete_game(const Game &, std::string *)
    {
        return false;
    }

    // ---- save sync: a game's save data in step with the profile's save store ----
    // Whether the profile playing syncs the game's save data before it starts and after it ended.
    virtual bool save_sync_wanted(const std::string &)
    {
        return false;
    }
    // Syncs a game's save data (its file in roms/), on a thread of its own; save_sync() tells how
    // it goes. before: the game is about to start (else: it ended, the menu is back).
    virtual void start_save_sync(const std::string &, bool)
    {
    }
    virtual SaveSync save_sync()
    {
        return {};
    }
    // The player's choice in a conflict.
    virtual void choose_save_data(SaveChoice)
    {
    }
    // What the last sync came to was shown: save_sync() is idle again.
    virtual void end_save_sync()
    {
    }
    // The player does not wait for the sync before a game: it stops, changing nothing more.
    virtual void stop_save_sync()
    {
    }
    // A game starts: once the menu is back, its save data is synced after it (start_save_sync,
    // not before), and on later starts until that worked.
    virtual void will_play(const std::string &)
    {
    }

    // ---- Settings > Save sync: profiles linked with a server ----
    virtual SaveSyncSetup save_sync_setup()
    {
        return {};
    }
    // The servers a profile can be paired with, in the order of sources.json.
    virtual std::vector<PairServer> pair_servers()
    {
        return {};
    }
    // Pairs a profile (an index into save_sync_setup().profiles) with a server (into
    // pair_servers()), on a thread of its own; pairing() tells how it goes. False when it cannot
    // start (one runs already).
    virtual bool start_pairing(int, int)
    {
        return false;
    }
    virtual PairingStatus pairing()
    {
        return {};
    }
    // Stops a pairing that waits, or forgets the end of one: pairing() is idle again.
    virtual void cancel_pairing()
    {
    }
    // Takes a profile's server out of save-sync.json: its save data stays on the console and on
    // the server.
    virtual bool unlink_profile(int)
    {
        return false;
    }

    virtual Eden::Multiplayer::Snapshot room_status() { return {}; }
    virtual Eden::Multiplayer::Connection room_connection() { return {}; }
    virtual bool connect_room(const Eden::Multiplayer::Connection&) { return false; }
    virtual void leave_room() {}
    virtual std::optional<std::string> room_text(int, const std::string&, const std::atomic<bool>&)
    { return std::nullopt; }
    virtual bool can_launch_game() { return true; }

    // ---- images ----
    virtual bool load_image(const std::string &path, gfx::Image *image) = 0;
};

} // namespace pe::ui
