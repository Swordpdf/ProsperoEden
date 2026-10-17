// SPDX-License-Identifier: GPL-3.0-or-later
// Download sources: places on the network that have Switch games (source.h), listed in the
// Library beside the games on the console. A game is downloaded to the game files folder when it
// is played or put in the download queue; nothing is streamed: it runs from its file on the
// console, as one copied there by hand. The same game on several sources is one title that can be
// downloaded from any of them (SameGame tells), and a game already on the console is not offered.
//
// The sources are set up in config/remote/sources.json, which the player writes (over FTP, like
// the keys); each entry names its backend's "type" (backends.cpp), an optional "name" for the menu,
// and what the backend needs (see its header, remote/<type>/):
//
//   { "sources": [ { "type": "<type>", "name": "Home", ... } ] }
//
// What is kept, in config/remote/: queue.json (the download queue, one for all sources, so it goes
// on after the app was closed) and <source>/catalog.json (each source's game list, so the Library
// shows it at once and without a network); the covers go with the others (covers/remote-...tga).
//
// A download is written to the game files folder's .remote-downloads/<source>/<game>/, its
// contents checked as they come (stream_check.h; a damaged file is deleted), and, once all of the
// game's files are complete, moved to its game folder in roms/ (GameFolder), the game's file with
// its updates and DLC, so roms/ only ever holds whole files of whole games. Games downloaded before
// game folders keep their files where they are (the game's file in roms/, its updates and DLC in
// updates/), and are still found there. One that stopped (the app closed, a game started, the
// network went away) goes on from where it was, its last 4 MB fetched again in case of a power cut;
// .remote-downloads/ keeps nothing else (a cancel deletes a game's folder, the next start whatever
// is not queued). Everything runs on two threads of this file's own; the menu only reads their
// state.
#pragma once

#include "remote/source.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Eden::Remote {

struct Paths {
    std::string config;    // config/remote: sources.json, queue.json, <source>/catalog.json
    std::string covers;    // where the covers go
    std::string roms;      // the game files folder's roms/, where downloads go (in game folders),
    std::string updates;   // and updates/, where downloads went before game folders
    std::string downloads; // and its .remote-downloads/, for the files being downloaded
};

// Turns a cover as a source sends it (PNG or JPEG) into the TGA the launcher draws; false when it
// cannot be read.
using CoverWriter = bool (*)(const std::string& encoded, const std::string& tga_path);
// The names in a folder (empty when it cannot be read): the console's own way of reading one.
using FolderLister = std::vector<std::string> (*)(const std::string& folder);

// One file of a game.
struct Part {
    std::string id;         // the source's name for it
    std::string name;       // its file name, also on the console
    std::uint64_t size = 0; // 0 when not known
    bool update = false;    // an update or DLC, not the game's file
};

// A game on a source.
struct Game {
    std::string source;     // the key of its source (SourceStatus::key)
    std::string id;         // the source's name for it
    std::string name;
    std::string file;       // its path in roms/ once downloaded: "<GameFolder>/<game file name>"
    std::uint64_t size = 0; // all of its files
    std::string cover;      // its cover's TGA on the console; empty without one
    std::string cover_source; // the source's name for the cover
    std::vector<Part> parts;  // the game's file first
    // What tells it apart (source.h, SourceGame): its title ID (16 capital hex digits; empty when
    // unknown), its ids at metadata providers, and whether its name comes from such metadata.
    std::string title_id;
    std::map<std::string, std::string> ids;
    bool identified = false;
    std::string normal_name; // NormalName(name), kept for comparing
};

// The same game on several sources: one title, which can be downloaded from any of them.
struct Title {
    std::string key;         // which title it is, for the menu (TitleKey)
    std::vector<Game> games; // the game on each source that has it, in the order of sources.json
};

// ---- which game is which ----
// Two sources' games are the same game when, asked in this order, the first thing both know is
// the same: their title ID; their id at a metadata provider both have one of; their names, when
// both come from metadata (NormalName); else their file names.
bool SameGame(const Game& a, const Game& b);
// A source's game is a game on the console: by the file it is downloaded as first (its path in roms/;
// the file name alone when either has no folder, as a game downloaded before game folders has not),
// then by their title IDs when the source knows its own, else by their names when it comes from
// metadata. normal_name: the console game's name as NormalName makes it (once, for all the
// sources' games it is compared with). file: the console game's path in roms/.
bool SameAsLocal(const Game& game, std::uint64_t title_id, const std::string& normal_name, const std::string& file);
// A source's game as the console takes it (its game file, its updates and DLC); false when it has
// no game file the console can use. source: the key of its source ("" for none).
bool AsGame(const std::string& source, const SourceGame& from, Game* game);
// The folder in roms/ a game is downloaded to, from its game file's name: the name without its
// extension and the tags dumps carry ("Name [0100...][v0]", "Name (USA)"), as the Library names a
// game by its file; else the name without its extension, else the source's id for the game, each
// made fit for a folder's name.
std::string GameFolder(const std::string& file, const std::string& id);
// A name as names are compared: lower case, without accents, marks and anything but letters and
// digits ("Café: Let's Go!" is "cafeletsgo").
std::string NormalName(const std::string& name);
// A title's key, from its first game: its title ID, else its first provider id, else its name or
// file name ("title:0100...", "screenscraper:123456", "name:...", "file:...").
std::string TitleKey(const Game& game);

// verifying: a download that goes on from where it was reads what its file has so far first, for
// the check of its contents (stream_check.h); done goes up to where the download goes on.
enum class State : std::uint8_t { queued, downloading, verifying, failed };
struct Download {
    std::string source;
    std::string id;
    State state = State::queued;
    std::uint64_t done = 0;  // bytes of the whole game on the console so far
    std::uint64_t total = 0; // the whole game; 0 when not known
    std::uint64_t rate = 0;  // bytes a second, smoothed, while it downloads; 0 when not known yet
    std::string error;       // why it failed (English)
};

struct SourceStatus {
    std::string key;      // stable, from its name: its folder in config/remote and in the queue
    std::string name;     // as the menu shows it
    std::string address;  // where it is; empty when its entry is not usable
    bool refreshing = false;
    bool online = false;  // the last look at it worked
    std::string error;    // why the last look failed, or what is wrong with its entry
    std::size_t games = 0;
};

struct Status {
    bool configured = false; // sources.json names at least one source
    std::string error;       // what is wrong with sources.json itself
    std::vector<SourceStatus> sources;
    // Changes whenever a game list, a cover or the files in roms/ changed: the Library reads its
    // list again.
    std::uint64_t generation = 0;
    int ftp_port = 2121; // the console's FTP server, which writes the downloads (ftp.h)
};

// Reads sources.json, the kept lists and the queue (again: an edited sources.json applies when the
// menu opens), and lets the threads run: a source's list is read when it was not yet, or long ago,
// and the queue goes on. Called whenever the menu opens.
void Start(const Paths& paths, CoverWriter writer, FolderLister lister);
// Stops the threads before a game starts: a download stops (its file stays, for later), nothing is
// read from the network while the game runs. Waits a moment for the transfer to end.
void Stop();
// Reads every source's game list again.
void Refresh();
// Files in roms/ changed (a game was deleted): the Library reads its list again.
void Changed();

Status Current();
// Every source's games, in the order of sources.json.
std::vector<Game> Games();
// The same, as titles: each game once, with every source that has it.
std::vector<Title> Titles();
// One game of a source; false when it has no such game.
bool Find(const std::string& source, const std::string& id, Game* game);
std::vector<Download> Downloads();

// Puts a game in the download queue: first (it is played as soon as it is there) or last. A game
// whose download failed is tried again. False when the source has no such game, or when the same
// game (SameGame) is already queued from another source.
bool Enqueue(const std::string& source, const std::string& id, bool first);
// Takes a game out of the queue; what it had downloaded is deleted.
bool Cancel(const std::string& source, const std::string& id);
// The files of a game that are on the console, as full paths: in its game folder, and where a
// download before game folders put them.
std::vector<std::string> Files(const std::string& source, const std::string& id);

} // namespace Eden::Remote
