// SPDX-License-Identifier: GPL-3.0-or-later
// A game's save data kept in step with a profile's save store (save_store.h), the same way for
// every backend: the console's save folder of the game is packed (save_archive.h), the store says
// whether its copy or the console's is newer, and the newer one goes to the other side. When both
// changed since they were last the same (played on two devices without a sync between), the
// player chooses; nothing is overwritten without that. Which side changed is told by the save
// data itself: the console keeps what each game's was when it was last in step, on both sides
// (<store>/synced.json), so a store that only compares times (RomM, when another device's
// version came in since) does not replace save data changed on the console.
// Save data replaced on the console is first moved to a backup folder (the last few of each game
// are kept).
//
// Blocking (the network), and on no thread of its own: the caller runs it where waiting is fine.
#pragma once

#include "remote/save_archive.h"
#include "remote/save_store.h"

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace Eden::Remote {

// A game on the console whose save data syncs.
struct SyncGame {
    std::uint64_t title_id = 0; // read from its file
    std::string name;           // as the Library shows it
    std::string file;           // its path in roms/
};

struct SyncPlaces {
    std::string save;    // the game's save folder (.../save/0000000000000000/<profile>/<title ID>)
    std::string store;   // the profile's folder for what its store keeps between starts
    std::string work;    // a folder for the zips while they are needed
    std::string backups; // where replaced save data goes (<backups>/<title ID>/<time>)
    SaveArchive::Lister lister = nullptr;
};

// The player's choice in a conflict.
enum class SyncChoice : std::uint8_t { console, server, neither };
// Asked in a conflict: what the store has (plan) and what the console has (local).
using SyncChooser = std::function<SyncChoice(const SavePlan& plan, const LocalSave& local)>;

enum class SyncOutcome : std::uint8_t {
    same,       // nothing to do
    uploaded,   // the console's save data went to the store
    downloaded, // the store's replaced the console's
    kept,       // a conflict the player left as it is
    no_game,    // the store has no such game: it cannot keep its save data
    failed,     // see message
};

struct SyncResult {
    SyncOutcome outcome = SyncOutcome::failed;
    std::string message; // what went wrong, in English; empty otherwise
    std::string user;    // who the store is signed in as, when it says
    // Failed because the server is older than its backend takes: its version and the oldest one.
    bool too_old = false;
    std::string version;
    std::string needed;
};

// Syncs a game's save data with the store of `type` made from `settings` (a profile's entry in
// save-sync.json). choose: asked in a conflict; nullptr leaves a conflict as it is.
SyncResult SyncSaveData(const std::string& type, const nlohmann::json& settings, const SyncGame& game,
                        const SyncPlaces& places, const SyncChooser& choose, const Stopped& stopped);

// The name of a game's save folder and its zip: its title ID as 16 capital hex digits.
std::string TitleFolder(std::uint64_t title_id);

} // namespace Eden::Remote
