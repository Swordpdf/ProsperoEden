// SPDX-License-Identifier: GPL-3.0-or-later
// Download sources; see remote.h. Nothing here knows a backend: they are made by MakeSource
// (backends.h) and used through Source (source.h).
#include "remote.h"

#include "backends.h"
#include "ftp.h"
#include "stream_check.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#include <nlohmann/json.hpp>

namespace Eden::Remote {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// A list is read again when the menu opens this long after the last time.
constexpr auto kRefreshAge = std::chrono::minutes(15);
// Room left free on the drive besides a download.
constexpr std::uint64_t kSpare = 256ull << 20;
// A download that goes on fetches its last bytes again: after a power cut, the end of a file may
// not hold what was written there.
constexpr std::uint64_t kRewind = 4u << 20;

struct Entry {
    std::uint64_t serial = 0; // tells entries apart for the threads
    std::string source;
    std::string id;
    State state = State::queued;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
};

struct SourceState {
    std::string key;
    std::string name;
    std::string signature;           // its entry in sources.json, to see a change
    std::shared_ptr<Source> source;  // nullptr when the entry is not usable
    std::string error;
    bool refresh_wanted = false;
    bool refreshing = false;
    bool online = false;
    bool listed_once = false;        // its list was read since the app opened
    Clock::time_point listed_at{};
    std::vector<Game> games;
};

// A cancelled game's files in .remote-downloads/, to be deleted.
struct Discard {
    std::string source;
    std::string id;
    std::vector<std::string> names;
};

struct Shared {
    std::mutex lock;
    std::condition_variable wake;
    Paths paths;
    CoverWriter writer = nullptr;
    FolderLister lister = nullptr;
    bool configured = false;
    std::string error; // about sources.json
    std::vector<std::shared_ptr<SourceState>> sources;
    std::deque<Entry> queue;
    // Deleted by the download thread without the lock (a file of gigabytes takes a while), and
    // before it begins another download: one of the same game finds nothing of the old one.
    std::vector<Discard> discards;
    FtpServer ftp; // where the downloads are written (sources.json's "ftp_port" and sign-in)
    // The covers on the console (CoverPath), known without asking the drive: the menu asks for
    // them often, and a drive busy with a download can keep it waiting.
    std::set<std::string> covers;
    std::uint64_t serials = 0;
    bool threads = false;
    bool queue_read = false;
    std::uint64_t generation = 1;
    bool transferring = false; // the download thread is in a transfer
    bool listing = false;      // the list thread is
    // Read by the transfers' stop questions, without the lock.
    std::atomic<bool> halt{true};
    std::atomic<std::uint64_t> cancel{0};
    std::atomic<std::uint64_t> current{0};
    std::atomic<std::uint64_t> current_done{0};
    std::atomic<std::uint64_t> current_rate{0}; // bytes a second, smoothed; 0 until a second went by
};

Shared& State_() {
    static Shared* shared = new Shared; // lives as long as the process: its threads use it
    return *shared;
}

void Log(const std::string& line) { std::fprintf(stderr, "[remote] %s\n", line.c_str()); }

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A file the console can use: an NSP or XCI with a plain name.
bool GameFileName(const std::string& name) {
    if (name.size() < 5 || name.size() > 240) return false;
    for (const unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
    const std::string extension = Lower(name.substr(name.size() - 4));
    return extension == ".nsp" || extension == ".xci";
}

// The last part of a path: a file's name without its folder.
std::string BaseName(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A name as one part of a path: letters, digits, '-' and '_' stay, the rest is %XX.
std::string Safe(const std::string& name) {
    static const char hex[] = "0123456789ABCDEF";
    std::string safe;
    for (const unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            safe += static_cast<char>(c);
        } else {
            safe += '%';
            safe += hex[c >> 4];
            safe += hex[c & 15];
        }
    }
    return safe;
}

std::int64_t FileSize(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return -1;
    return static_cast<std::int64_t>(info.st_size);
}

// A folder and the ones above it that are missing.
void MakeFolders(const std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
        (void)mkdir(path.substr(0, slash).c_str(), 0777);
    (void)mkdir(path.c_str(), 0777);
}

// Unknown on the console: statfs is only in libkernel_sys, which a title does not import, and
// the SDK's libc.a has it as a raw system call, which ends the process. A full drive still stops
// the download when a write fails, before anything reaches roms/.
bool FreeSpace([[maybe_unused]] const std::string& path, [[maybe_unused]] std::uint64_t* bytes) {
#if defined(__PROSPERO__)
    return false;
#else
    struct statfs info {};
    if (statfs(path.c_str(), &info) != 0) return false;
    *bytes = static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_bsize);
    return true;
#endif
}

std::string Size(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    else std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return text;
}

// Written beside, then moved into place: a reader never sees half a file.
bool WriteFile(const std::string& path, const std::string& data) {
    const std::string staged = path + ".new";
    std::FILE* file = std::fopen(staged.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (std::fclose(file) != 0 || !written || std::rename(staged.c_str(), path.c_str()) != 0) {
        (void)std::remove(staged.c_str());
        return false;
    }
    return true;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Text(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::uint64_t Unsigned(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) return static_cast<std::uint64_t>(std::max<std::int64_t>(found->get<std::int64_t>(), 0));
    return 0;
}

std::string CoverPath(const Shared& s, const std::string& source, const std::string& id) {
    return s.paths.covers + "/remote-" + Safe(source) + "-" + Safe(id) + ".tga";
}

std::shared_ptr<SourceState> FindSource(const Shared& s, const std::string& key) {
    for (const auto& state : s.sources)
        if (state->key == key) return state;
    return nullptr;
}

const Game* FindGame(const Shared& s, const std::string& source, const std::string& id) {
    if (const auto state = FindSource(s, source))
        for (const Game& game : state->games)
            if (game.id == id) return &game;
    return nullptr;
}

// A title ID as 16 capital hex digits; empty when it is none.
std::string NormalTitleId(const std::string& text) {
    std::string id;
    for (const unsigned char c : text)
        if (std::isxdigit(c)) id += static_cast<char>(std::toupper(c));
        else if (c != ' ' && c != '-' && c != '[' && c != ']') return {};
    return id.size() == 16 && id != std::string(16, '0') ? id : std::string{};
}

// A source's game as the console takes it: its game file (the larger one when it has an .nsp and
// an .xci) and its updates and DLC; files the console cannot use are left out, and a game without a
// game file (an update on its own) is no game.
bool MakeGame(const std::string& source, const SourceGame& from, Game* game) {
    game->source = source;
    game->id = from.id;
    game->name = from.name;
    game->cover_source = from.cover;
    game->title_id = NormalTitleId(from.title_id);
    game->ids.clear();
    for (const auto& [provider, id] : from.ids)
        if (!provider.empty() && !id.empty()) game->ids[Lower(provider)] = id;
    game->identified = from.identified;
    game->parts.clear();
    const SourceFile* main = nullptr;
    for (const SourceFile& file : from.files)
        if (file.kind == FileKind::game && GameFileName(file.name) && (main == nullptr || file.size > main->size))
            main = &file;
    if (main == nullptr || from.id.empty()) return false;
    game->file = GameFolder(main->name, from.id) + "/" + main->name;
    game->parts.push_back({main->id, main->name, main->size, false});
    for (const SourceFile& file : from.files) {
        if (file.kind == FileKind::game || !GameFileName(file.name)) continue;
        // Two files may not end up with one name.
        if (std::any_of(game->parts.begin(), game->parts.end(), [&](const Part& p) { return p.name == file.name; }))
            continue;
        game->parts.push_back({file.id, file.name, file.size, true});
    }
    game->size = 0;
    for (const Part& part : game->parts) game->size += part.size;
    if (game->name.empty()) game->name = main->name;
    game->normal_name = NormalName(game->name);
    return true;
}

// What a backend needs to fetch one of its games' files.
SourceGame AsSourceGame(const Game& game) {
    SourceGame source;
    source.id = game.id;
    source.name = game.name;
    source.cover = game.cover_source;
    for (const Part& part : game.parts)
        source.files.push_back({part.id, part.name, part.size, part.update ? FileKind::update : FileKind::game});
    return source;
}

// ---- the kept lists and queue ----

// A source's games as its kept list has them.
Json CatalogGames(const std::vector<Game>& list) {
    Json games = Json::array();
    for (const Game& game : list) {
        Json parts = Json::array();
        for (const Part& part : game.parts)
            parts.push_back({{"id", part.id}, {"name", part.name}, {"size", part.size}, {"update", part.update}});
        games.push_back({{"id", game.id}, {"name", game.name}, {"file", game.file}, {"size", game.size},
                         {"cover", game.cover_source}, {"parts", parts}, {"title_id", game.title_id},
                         {"ids", game.ids}, {"identified", game.identified}});
    }
    return games;
}

void SaveCatalog(const Shared& s, const SourceState& state) {
    const Json games = CatalogGames(state.games);
    const std::string folder = s.paths.config + "/" + state.key;
    MakeFolders(folder);
    if (!WriteFile(folder + "/catalog.json", Json{{"signature", state.signature}, {"games", games}}.dump()))
        Log("could not write the game list of " + state.name);
}

void LoadCatalog(Shared& s, SourceState& state) {
    state.games.clear();
    const Json catalog = Json::parse(ReadFile(s.paths.config + "/" + state.key + "/catalog.json"), nullptr, false);
    // A list from another entry (another server, other settings) is not this one's.
    if (!catalog.is_object() || Text(catalog, "signature") != state.signature) return;
    const auto games = catalog.find("games");
    if (games == catalog.end() || !games->is_array()) return;
    for (const Json& item : *games) {
        if (!item.is_object()) continue;
        Game game;
        game.source = state.key;
        game.id = Text(item, "id");
        game.name = Text(item, "name");
        // Its path in roms/ from its file's name, also for a list kept before game folders.
        if (const std::string file = BaseName(Text(item, "file")); GameFileName(file))
            game.file = GameFolder(file, game.id) + "/" + file;
        game.size = Unsigned(item, "size");
        game.cover_source = Text(item, "cover");
        game.title_id = NormalTitleId(Text(item, "title_id"));
        if (const auto ids = item.find("ids"); ids != item.end() && ids->is_object())
            for (const auto& [provider, id] : ids->items())
                if (id.is_string() && !id.get<std::string>().empty()) game.ids[provider] = id.get<std::string>();
        const auto identified = item.find("identified");
        game.identified = identified != item.end() && identified->is_boolean() && identified->get<bool>();
        game.normal_name = NormalName(game.name);
        if (const auto parts = item.find("parts"); parts != item.end() && parts->is_array())
            for (const Json& entry : *parts)
                if (entry.is_object()) {
                    const auto update = entry.find("update");
                    game.parts.push_back({Text(entry, "id"), Text(entry, "name"), Unsigned(entry, "size"),
                                          update != entry.end() && update->is_boolean() && update->get<bool>()});
                }
        const bool usable = !game.id.empty() && !game.file.empty() && !game.parts.empty() &&
                            std::all_of(game.parts.begin(), game.parts.end(),
                                        [](const Part& part) { return !part.id.empty() && GameFileName(part.name); });
        if (!usable) continue;
        if (const std::string cover = CoverPath(s, state.key, game.id); FileSize(cover) > 0) s.covers.insert(cover);
        state.games.push_back(std::move(game));
    }
}

void SaveQueue(Shared& s) {
    Json entries = Json::array();
    for (const Entry& entry : s.queue) entries.push_back({{"source", entry.source}, {"id", entry.id}});
    if (!WriteFile(s.paths.config + "/queue.json", entries.dump())) Log("could not write queue.json");
}

void LoadQueue(Shared& s) {
    const Json entries = Json::parse(ReadFile(s.paths.config + "/queue.json"), nullptr, false);
    if (!entries.is_array()) return;
    for (const Json& item : entries) {
        if (!item.is_object() || Text(item, "source").empty() || Text(item, "id").empty()) continue;
        Entry entry;
        entry.serial = ++s.serials;
        entry.source = Text(item, "source");
        entry.id = Text(item, "id");
        s.queue.push_back(entry);
    }
}

// ---- sources.json ----

// The sources of sources.json, keeping those whose entry did not change (with their lists).
void ReadSources(Shared& s) {
    std::vector<std::shared_ptr<SourceState>> sources;
    s.configured = false;
    s.error.clear();
    std::ifstream in(s.paths.config + "/sources.json");
    if (in) {
        const Json json = Json::parse(in, nullptr, false);
        const auto list = json.is_object() ? json.find("sources") : json.end();
        if (!json.is_object() || list == json.end() || !list->is_array()) {
            s.error = "sources.json is not valid: it needs a \"sources\" list";
        } else {
            // The console's FTP server, which writes the downloads (ftp.h).
            s.ftp = FtpServer{};
            if (const auto port = json.find("ftp_port"); port != json.end() && port->is_number_integer() &&
                                                         port->get<int>() > 0 && port->get<int>() < 65536)
                s.ftp.port = port->get<int>();
            if (const std::string user = Text(json, "ftp_user"); !user.empty()) s.ftp.user = user;
            if (const std::string password = Text(json, "ftp_password"); !password.empty()) s.ftp.password = password;
            for (const Json& entry : *list) {
                if (!entry.is_object()) continue;
                const std::string type = Lower(Text(entry, "type"));
                std::string name = Text(entry, "name");
                if (name.empty()) name = type.empty() ? "?" : type;
                // Names tell the sources apart in the menu: a second "Home" is "Home (2)".
                const std::string named = name;
                for (int n = 2; std::any_of(sources.begin(), sources.end(), [&](const auto& other) { return other->name == name; }); ++n)
                    name = named + " (" + std::to_string(n) + ")";
                // Its key: from its name, told apart from another one of the same name.
                std::string key = Lower(Safe(name));
                for (int n = 2; std::any_of(sources.begin(), sources.end(), [&](const auto& other) { return other->key == key; }); ++n)
                    key = Lower(Safe(name)) + "-" + std::to_string(n);
                auto state = std::make_shared<SourceState>();
                state->key = key;
                state->name = name;
                state->signature = std::to_string(std::hash<std::string>{}(entry.dump()));
                if (const auto before = FindSource(s, key); before && before->signature == state->signature) {
                    sources.push_back(before);
                    continue;
                }
                std::string error;
                state->source = MakeSource(type, entry, &error);
                state->error = error;
                LoadCatalog(s, *state);
                if (!state->source) Log("source " + name + ": " + error);
                sources.push_back(std::move(state));
            }
            s.configured = !sources.empty();
        }
    }
    if (sources.size() != s.sources.size() ||
        !std::equal(sources.begin(), sources.end(), s.sources.begin()))
        ++s.generation;
    s.sources = std::move(sources);
}

// ---- downloading ----

// A game's files while it downloads, and where they go.
std::string StagedFolder(const Paths& paths, const std::string& source, const std::string& id) {
    return paths.downloads + "/" + Safe(source) + "/" + Safe(id);
}
std::string StagedPath(const Paths& paths, const Game& game, const Part& part) {
    return StagedFolder(paths, game.source, game.id) + "/" + part.name;
}
// A game's folder in roms/: its files all go there.
std::string FolderPath(const Paths& paths, const Game& game) {
    return paths.roms + "/" + GameFolder(game.file, game.id);
}
std::string FinalPath(const Paths& paths, const Game& game, const Part& part) {
    return FolderPath(paths, game) + "/" + part.name;
}
// Where a download before game folders put a file: the game's in roms/, its updates and DLC in
// updates/. Such a game is still on the console; its files are not moved.
std::string EarlierPath(const Paths& paths, const Part& part) {
    return (part.update ? paths.updates : paths.roms) + "/" + part.name;
}
// A file of a game on the console, in its game folder or where an earlier download put it; empty
// when it is not there.
std::string PlacedPath(const Paths& paths, const Game& game, const Part& part) {
    for (std::string path : {FinalPath(paths, game, part), EarlierPath(paths, part)})
        if (FileSize(path) >= 0) return path;
    return {};
}

// The bytes of a download, to its file in .remote-downloads/ through the console's FTP server
// (ftp.h): the app does not write it itself. Its contents are checked as they go by (StreamCheck).

class FileReceiver final : public Receiver {
  public:
    FileReceiver(FtpUpload& upload, const FtpServer& server, std::string path, std::uint64_t base, std::uint64_t start,
                 StreamCheck& check)
        : upload_(upload), server_(server), path_(std::move(path)), base_(base), start_(start), check_(check) {}

    // The file's size as its source gave it (0: not known): a server that sends more is stopped,
    // rather than left to fill the drive.
    void limit(std::uint64_t size) { limit_ = size; }

    bool begin(bool from_start) override {
        // The whole file, although a part was asked for: it is written again from its start.
        if (from_start && start_ > 0) {
            start_ = 0;
            check_.Restart();
            if (!upload_.Open(server_, path_, 0, &error_)) return false;
        }
        return true;
    }
    bool take(const void* data, std::size_t size) override {
        if (limit_ > 0 && start_ + written_ + size > limit_) {
            error_ = "The server sent more than the file's size";
            return false;
        }
        check_.Feed(start_ + written_, data, size);
        if (!upload_.Send(data, size)) {
            // Its reason, a full drive above all, is on the control connection.
            const std::string reason = upload_.Reason();
            error_ = "The console's FTP server could not write the file" + (reason.empty() ? std::string{} : ": " + reason);
            return false;
        }
        written_ += size;
        State_().current_done.store(base_ + start_ + written_);
        const Clock::time_point now = Clock::now();
        // The speed: measured each second, smoothed so that it does not jump.
        const double seconds = std::chrono::duration<double>(now - sampled_).count();
        if (seconds >= 1.0) {
            const double measured = static_cast<double>(written_ - sampled_bytes_) / seconds;
            const double before = static_cast<double>(State_().current_rate.load());
            State_().current_rate.store(static_cast<std::uint64_t>(before > 0.0 ? before * 0.7 + measured * 0.3 : measured));
            sampled_ = now;
            sampled_bytes_ = written_;
        }
        return true;
    }
    bool stopped() override {
        const Shared& s = State_();
        return s.halt.load() || (s.cancel.load() != 0 && s.cancel.load() == s.current.load());
    }
    // Why the server did not take the file; empty when it did.
    const std::string& error() const { return error_; }

  private:
    FtpUpload& upload_;
    const FtpServer& server_;
    std::string path_;
    std::uint64_t base_;  // the game's bytes before this file
    std::uint64_t start_; // where in the file the transfer began
    StreamCheck& check_;
    std::uint64_t written_ = 0;
    std::uint64_t limit_ = 0;
    std::string error_;
    Clock::time_point sampled_ = Clock::now(); // when the speed was last measured
    std::uint64_t sampled_bytes_ = 0;          // written_ then
};

enum class Outcome { done, stopped, failed };

// The queue entry being downloaded shows the step it is in.
void ShowState(State state) {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    for (Entry& entry : s.queue)
        if (entry.serial == s.current.load()) entry.state = state;
}

// A download that goes on from where it was: what its file has before `start`, read from the drive
// for the check of its contents (the queue shows it as verifying meanwhile). False when it was
// asked to stop; a file that cannot be read is left unchecked (the check sees the gap).
bool FeedFromDrive(StreamCheck& check, const std::string& path, std::uint64_t base, std::uint64_t start) {
    Shared& s = State_();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return true;
    ShowState(State::verifying);
    std::vector<char> buffer(1u << 20);
    bool stopped = false;
    for (std::uint64_t done = 0; done < start;) {
        if (s.halt.load() || (s.cancel.load() != 0 && s.cancel.load() == s.current.load())) {
            stopped = true;
            break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), start - done));
        const std::size_t got = std::fread(buffer.data(), 1, want, file);
        if (got == 0) break;
        check.Feed(done, buffer.data(), got);
        done += got;
        s.current_done.store(base + done);
    }
    std::fclose(file);
    ShowState(State::downloading);
    return !stopped;
}

Outcome DownloadPart(Source& source, const Paths& paths, const FtpServer& ftp, const Game& game, const Part& part,
                     std::uint64_t base, std::string* error) {
    // The game folder is made once the download is complete (PlaceParts): one stopped or cancelled
    // leaves nothing in roms/.
    const std::string& folder = paths.roms;
    MakeFolders(folder);
    // A file of that name is on the console already: it stays as it is, whatever its size (one
    // the player copied there, or an earlier download), and nothing is downloaded over it.
    const std::string placed = PlacedPath(paths, game, part);
    const std::int64_t present = placed.empty() ? -1 : FileSize(placed);
    if (present >= 0) {
        if (part.size > 0 && static_cast<std::uint64_t>(present) != part.size)
            Log("download of " + game.name + ": " + part.name + " is on the console with another size; it is kept");
        return Outcome::done;
    }
    MakeFolders(StagedFolder(paths, game.source, game.id));
    const std::string staged = StagedPath(paths, game, part);
    std::int64_t existing = FileSize(staged);
    if (existing < 0 || (part.size > 0 && static_cast<std::uint64_t>(existing) > part.size)) {
        (void)std::remove(staged.c_str());
        existing = 0;
    }
    const std::uint64_t start =
        static_cast<std::uint64_t>(existing) > kRewind ? static_cast<std::uint64_t>(existing) - kRewind : 0;
    std::uint64_t space = 0;
    if (part.size > 0 && FreeSpace(folder, &space) && space < part.size - start + kSpare) {
        *error = "Not enough free space: " + Size(part.size - start + kSpare) + " needed, " + Size(space) + " free";
        return Outcome::failed;
    }
    // Going on: written over from `start` (the rewound bytes come again), else a new file.
    StreamCheck check(part.name);
    if (start > 0 && !FeedFromDrive(check, staged, base, start)) return Outcome::stopped;
    FtpUpload upload;
    if (!upload.Open(ftp, staged, start, error)) return Outcome::failed;
    State_().current_done.store(base + start);
    FileReceiver receiver(upload, ftp, staged, base, start, check);
    receiver.limit(part.size);
    const bool fetched = source.fetch(AsSourceGame(game), {part.id, part.name, part.size, part.update ? FileKind::update : FileKind::game},
                                      start, receiver, error);
    // Stopped or failed: what the server has of the file stays, to go on from.
    if (receiver.stopped()) return Outcome::stopped;
    if (!receiver.error().empty()) {
        *error = receiver.error();
        return Outcome::failed;
    }
    if (!fetched) return Outcome::failed;
    if (!upload.Finish(error)) return Outcome::failed;
    const std::int64_t have = FileSize(staged);
    if (part.size > 0 && have != static_cast<std::int64_t>(part.size)) {
        *error = "The download is incomplete (" + Size(static_cast<std::uint64_t>(std::max<std::int64_t>(have, 0))) +
                 " of " + Size(part.size) + ")";
        return Outcome::failed;
    }
    // A damaged file is deleted, so that trying again downloads it again.
    switch (check.Result()) {
    case Verified::damaged:
        (void)std::remove(staged.c_str());
        *error = "The downloaded file " + part.name + " is damaged; trying again downloads it again";
        return Outcome::failed;
    case Verified::intact:
        Log("download of " + game.name + ": " + part.name + " checked (" + std::to_string(check.Contents()) + " contents)");
        break;
    default:
        Log("download of " + game.name + ": " + part.name + " could not be checked");
    }
    return Outcome::done;
}

// A game whose files are all downloaded: they go to its game folder, its updates and DLC first, so
// the game appears in the Library with all of it. A file that was there already (also where an
// earlier download put it) stays as it was.
bool PlaceParts(const Paths& paths, const Game& game, std::string* error) {
    std::vector<Part> parts = game.parts;
    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.update && !b.update; });
    const std::string folder = FolderPath(paths, game);
    for (const Part& part : parts) {
        const std::string staged = StagedPath(paths, game, part);
        if (FileSize(staged) < 0) continue;
        // rename() would replace a file that appeared there since the download began.
        if (!PlacedPath(paths, game, part).empty()) {
            Log("download of " + game.name + ": " + part.name + " is on the console already; it is kept");
            (void)std::remove(staged.c_str());
            continue;
        }
        MakeFolders(folder);
        if (std::rename(staged.c_str(), FinalPath(paths, game, part).c_str()) != 0) {
            *error = "Cannot move the download into " + folder;
            return false;
        }
    }
    return true;
}

// A game's folder in .remote-downloads/ with the files named, then the folders above it once
// they are empty.
void RemoveStaged(const Paths& paths, const std::string& source, const std::string& id,
                  const std::vector<std::string>& names) {
    const std::string folder = StagedFolder(paths, source, id);
    for (const std::string& name : names) (void)std::remove((folder + "/" + name).c_str());
    (void)rmdir(folder.c_str());
    (void)rmdir((paths.downloads + "/" + Safe(source)).c_str());
    (void)rmdir(paths.downloads.c_str());
}

// What a cancelled download leaves: its folder in .remote-downloads/. Nothing of it is in roms/
// yet (PlaceParts), so nothing there is touched. The download thread deletes it.
void DiscardParts(Shared& s, const std::string& source, const std::string& id, const Game* game) {
    Discard discard{source, id, {}};
    if (game != nullptr)
        for (const Part& part : game->parts) discard.names.push_back(part.name);
    s.discards.push_back(std::move(discard));
    s.wake.notify_all();
}

// What of a file is on the console: all of it, or what its download has so far.
std::uint64_t PresentOf(const Paths& paths, const Game& game, const Part& part) {
    const std::string placed = PlacedPath(paths, game, part);
    const std::int64_t whole = placed.empty() ? -1 : FileSize(placed);
    const std::int64_t started = FileSize(StagedPath(paths, game, part));
    return static_cast<std::uint64_t>(whole >= 0 ? whole : std::max<std::int64_t>(started, 0));
}

std::uint64_t Present(const Paths& paths, const Game& game) {
    std::uint64_t bytes = 0;
    for (const Part& part : game.parts) bytes += PresentOf(paths, game, part);
    return bytes;
}

// Unfinished downloads of games that are no longer in the queue (cancelled while one was still
// writing, a queue file that was lost, a source taken out of sources.json): nothing of them is kept.
void RemoveLeftovers(const Shared& s) {
    if (s.lister == nullptr) return;
    const auto names = [&](const std::string& folder) {
        std::vector<std::string> list;
        for (std::string& name : s.lister(folder))
            if (name != "." && name != "..") list.push_back(std::move(name));
        return list;
    };
    const auto remove = [&](const std::string& path) {
        // A folder (with the files in it) or a file.
        for (const std::string& name : names(path)) (void)std::remove((path + "/" + name).c_str());
        if (rmdir(path.c_str()) != 0) (void)std::remove(path.c_str());
    };
    for (const std::string& source : names(s.paths.downloads)) {
        const std::string folder = s.paths.downloads + "/" + source;
        for (const std::string& game : names(folder))
            if (std::none_of(s.queue.begin(), s.queue.end(),
                             [&](const Entry& e) { return Safe(e.source) == source && Safe(e.id) == game; }))
                remove(folder + "/" + game);
        if (rmdir(folder.c_str()) != 0 && FileSize(folder) >= 0) remove(folder);
    }
    (void)rmdir(s.paths.downloads.c_str());
}

// ---- the threads ----

void ListThread() {
    Shared& s = State_();
    std::unique_lock lock(s.lock);
    for (;;) {
        const auto wanted = [&] {
            for (const auto& state : s.sources)
                if (state->refresh_wanted && state->source) return state;
            return std::shared_ptr<SourceState>{};
        };
        s.wake.wait(lock, [&] { return !s.halt.load() && wanted() != nullptr; });
        const std::shared_ptr<SourceState> state = wanted();
        state->refresh_wanted = false;
        state->refreshing = true;
        s.listing = true;
        const std::shared_ptr<Source> source = state->source;
        const std::string key = state->key;
        lock.unlock();
        std::vector<SourceGame> listed;
        std::string error;
        const bool read = source->list(&listed, &error, [&s] { return s.halt.load(); });
        std::vector<Game> games;
        for (const SourceGame& from : listed) {
            Game game;
            if (MakeGame(key, from, &game)) games.push_back(std::move(game));
        }
        lock.lock();
        state->refreshing = false;
        // A source taken out of sources.json meanwhile is not this one any more.
        if (!read || FindSource(s, key) != state) {
            s.listing = false;
            if (FindSource(s, key) == state && !s.halt.load()) {
                state->online = false;
                state->error = error;
                Log(state->name + ": " + error);
            } else if (s.halt.load()) {
                state->refresh_wanted = true; // read when the menu is back
            }
            s.wake.notify_all();
            continue;
        }
        // A cover that changed on the source is fetched again.
        for (const Game& game : games)
            for (const Game& before : state->games)
                if (before.id == game.id && before.cover_source != game.cover_source) {
                    (void)std::remove(CoverPath(s, key, game.id).c_str());
                    s.covers.erase(CoverPath(s, key, game.id));
                }
        // The Library reads all of the console's games again on a new generation: only for a list
        // that is not the one it has.
        const bool changed = CatalogGames(games) != CatalogGames(state->games);
        state->games = std::move(games);
        state->online = true;
        state->error.clear();
        state->listed_once = true;
        state->listed_at = Clock::now();
        if (changed) {
            ++s.generation;
            SaveCatalog(s, *state);
        }
        s.wake.notify_all(); // queued games that waited for the list
        // Where each goes, while the lock is held (Start sets the paths again).
        std::vector<std::pair<Game, std::string>> covers;
        for (const Game& game : state->games)
            if (const std::string path = CoverPath(s, key, game.id); !game.cover_source.empty() && !s.covers.contains(path))
                covers.emplace_back(game, path);
        const CoverWriter writer = s.writer;
        lock.unlock();
        // The Library shows them once all are in: each new list reads every game of the console.
        std::vector<std::string> written;
        for (const auto& [game, path] : covers) {
            if (s.halt.load() || writer == nullptr) break;
            std::string picture;
            if (!source->cover(AsSourceGame(game), &picture, [&s] { return s.halt.load(); })) continue;
            if (writer(picture, path)) written.push_back(path);
            else Log("the cover of " + game.name + " cannot be read");
        }
        lock.lock();
        s.covers.insert(written.begin(), written.end());
        if (!written.empty()) ++s.generation;
        s.listing = false;
        s.wake.notify_all();
    }
}

void DownloadThread() {
    Shared& s = State_();
    std::unique_lock lock(s.lock);
    for (;;) {
        // A game the kept list does not have (a new entry in sources.json) waits for the source's list.
        const auto next = [&] {
            return std::find_if(s.queue.begin(), s.queue.end(), [&](const Entry& entry) {
                if (entry.state != State::queued) return false;
                const auto state = FindSource(s, entry.source);
                return state == nullptr || state->source == nullptr || state->listed_once ||
                       FindGame(s, entry.source, entry.id) != nullptr;
            });
        };
        // A cancelled game's files go also while a game runs (a cancel says they are gone).
        s.wake.wait(lock, [&] { return !s.discards.empty() || (!s.halt.load() && next() != s.queue.end()); });
        if (!s.discards.empty()) {
            const std::vector<Discard> discards = std::move(s.discards);
            s.discards.clear();
            const Paths paths = s.paths;
            lock.unlock();
            for (const Discard& discard : discards) RemoveStaged(paths, discard.source, discard.id, discard.names);
            lock.lock();
            continue;
        }
        Entry& entry = *next();
        const auto state = FindSource(s, entry.source);
        const Game* found = FindGame(s, entry.source, entry.id);
        if (state == nullptr || state->source == nullptr || found == nullptr) {
            entry.state = State::failed;
            entry.error = state == nullptr ? "Its source is no longer in sources.json" :
                          state->source == nullptr ? state->error : "Its source no longer has this game";
            continue;
        }
        const std::uint64_t serial = entry.serial;
        const Game game = *found;
        const std::shared_ptr<Source> source = state->source;
        const Paths paths = s.paths;
        const FtpServer ftp = s.ftp;
        entry.state = State::downloading;
        entry.error.clear();
        entry.total = game.size;
        s.current.store(serial);
        s.current_done.store(Present(paths, game));
        s.current_rate.store(0);
        s.transferring = true;
        lock.unlock();

        // Each file to .remote-downloads/, then all of them to their places (PlaceParts).
        std::vector<Part> parts = game.parts;
        std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.update && !b.update; });
        Outcome outcome = Outcome::done;
        std::string error;
        try {
            for (const Part& part : parts) {
                // What the game's other files have on the console, for the progress.
                const std::uint64_t base = Present(paths, game) - PresentOf(paths, game, part);
                outcome = DownloadPart(*source, paths, ftp, game, part, base, &error);
                if (outcome != Outcome::done) break;
            }
            if (outcome == Outcome::done && !PlaceParts(paths, game, &error)) outcome = Outcome::failed;
        } catch (const std::exception& problem) {
            // Nothing a server sends ends the app: the download fails, and stays out of the way
            // until the player tries it again.
            outcome = Outcome::failed;
            error = std::string("The download stopped: ") + problem.what();
        }

        lock.lock();
        s.transferring = false;
        s.current.store(0);
        const bool cancelled = s.cancel.load() == serial;
        if (cancelled) s.cancel.store(0);
        const auto at = std::find_if(s.queue.begin(), s.queue.end(), [&](const Entry& e) { return e.serial == serial; });
        // Cancelled once it was all in place already: it is done, and on the console.
        if (cancelled && outcome != Outcome::done) {
            DiscardParts(s, game.source, game.id, &game);
            if (at != s.queue.end()) s.queue.erase(at);
            SaveQueue(s);
        } else if (outcome == Outcome::done) {
            RemoveStaged(paths, game.source, game.id, {});
            if (at != s.queue.end()) s.queue.erase(at);
            SaveQueue(s);
            ++s.generation;
        } else if (at != s.queue.end()) {
            at->done = Present(paths, game);
            if (outcome == Outcome::stopped) {
                at->state = State::queued; // goes on when the menu is back
            } else {
                at->state = State::failed;
                at->error = error;
                Log("download of " + game.name + " failed: " + error);
            }
        }
        s.wake.notify_all();
    }
}

} // namespace

std::string GameFolder(const std::string& file, const std::string& id) {
    const std::string name = BaseName(file);
    const auto dot = name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    // As the Library's CleanTitle (eden_services.cpp) names a game by its file.
    std::string title = stem;
    if (title.rfind("[Game] ", 0) == 0) title.erase(0, 7);
    if (const auto tag = title.find_first_of("[("); tag != std::string::npos && tag > 0) title.erase(tag);
    while (!title.empty() && (title.back() == ' ' || title.back() == '_' || title.back() == '-')) title.pop_back();
    // Fit for a folder's name: no separators or control characters, no leading '.' (a hidden
    // folder, which the Library skips), no spaces or dots at its ends (a drive formatted exFAT drops
    // those, and the name would not be found again), and at most 255 bytes, cut between characters.
    const auto fit = [](const std::string& text) {
        std::string folder;
        for (const unsigned char c : text)
            if (c >= 32 && c != 127 && c != '/' && c != '\\') folder += static_cast<char>(c);
        const auto loose = [](char c) { return c == ' ' || c == '.'; };
        folder.erase(0, std::find_if_not(folder.begin(), folder.end(), loose) - folder.begin());
        if (folder.size() > 255) {
            std::size_t cut = 255;
            while (cut > 0 && (static_cast<unsigned char>(folder[cut]) & 0xC0) == 0x80) --cut;
            folder.resize(cut);
        }
        while (!folder.empty() && loose(folder.back())) folder.pop_back();
        return folder;
    };
    for (const std::string& candidate : {title, stem, Safe(id)})
        if (std::string folder = fit(candidate); !folder.empty()) return folder;
    return "game";
}

std::string NormalName(const std::string& name) {
    // Latin-1's letters (U+00C0-U+00FF) and Latin Extended-A (U+0100-U+017F) as plain letters;
    // '*' is a letter of two (Æ, Œ, ĳ...), '-' no letter (×, ÷).
    static constexpr std::string_view kLatin1 = "AAAAAA*CEEEEIIIIDNOOOOO-OUUUUY*saaaaaa*ceeeeiiiidnooooo-ouuuuy*y";
    static constexpr std::string_view kExtended =
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi**JjKkkLlLlLlLlLlNnNnNnnNnOoOoOo**RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    static const std::map<std::uint32_t, std::string_view> kPairs = {
        {0xC6, "ae"}, {0xDE, "th"}, {0xDF, "ss"}, {0xE6, "ae"}, {0xFE, "th"},
        {0x132, "ij"}, {0x133, "ij"}, {0x152, "oe"}, {0x153, "oe"}};
    std::string normal;
    for (std::size_t at = 0; at < name.size();) {
        // One UTF-8 character.
        const unsigned char first = static_cast<unsigned char>(name[at]);
        std::uint32_t code = first;
        std::size_t length = 1;
        if (first >= 0xF0) { code = first & 0x07; length = 4; }
        else if (first >= 0xE0) { code = first & 0x0F; length = 3; }
        else if (first >= 0xC0) { code = first & 0x1F; length = 2; }
        if (at + length > name.size()) break;
        for (std::size_t i = 1; i < length; ++i) code = (code << 6) | (static_cast<unsigned char>(name[at + i]) & 0x3F);
        const std::string_view bytes{name.data() + at, length};
        at += length;
        if (code < 0x80) {
            if (std::isalnum(static_cast<int>(code))) normal += static_cast<char>(std::tolower(static_cast<int>(code)));
        } else if (const auto pair = kPairs.find(code); pair != kPairs.end()) {
            normal += pair->second;
        } else if (code >= 0xC0 && code <= 0xFF && kLatin1[code - 0xC0] != '-') {
            normal += static_cast<char>(std::tolower(static_cast<unsigned char>(kLatin1[code - 0xC0])));
        } else if (code >= 0x100 && code <= 0x17F) {
            normal += static_cast<char>(std::tolower(static_cast<unsigned char>(kExtended[code - 0x100])));
        } else if (code >= 0x370 && !(code >= 0x2000 && code <= 0x2BFF) && !(code >= 0x3000 && code <= 0x303F) &&
                   !(code >= 0xFE00 && code <= 0xFE6F) && !(code >= 0xFF00 && code <= 0xFF0F)) {
            // Other scripts (Greek, Cyrillic, Japanese...) stay as they are; punctuation, symbols
            // (™ ®), marks and spaces go.
            normal += bytes;
        }
    }
    return normal;
}

bool SameGame(const Game& a, const Game& b) {
    if (!a.title_id.empty() && !b.title_id.empty()) return a.title_id == b.title_id;
    // An id at a metadata provider both have one of: the first one decides.
    for (const auto& [provider, id] : a.ids)
        if (const auto other = b.ids.find(provider); other != b.ids.end()) return id == other->second;
    if (a.identified && b.identified) {
        const std::string name = a.normal_name.empty() ? NormalName(a.name) : a.normal_name;
        return !name.empty() && name == (b.normal_name.empty() ? NormalName(b.name) : b.normal_name);
    }
    // The file names alone: the game folders could differ although the files are the same.
    return Lower(BaseName(a.file)) == Lower(BaseName(b.file));
}

bool AsGame(const std::string& source, const SourceGame& from, Game* game) { return MakeGame(source, from, game); }

bool SameAsLocal(const Game& game, std::uint64_t title_id, const std::string& normal_name, const std::string& file) {
    // The file it is downloaded as: whatever the title IDs say (a server's can be another than the
    // one the console reads from the file), it is this game, and not offered again. A game
    // downloaded before game folders, or copied to roms/ without a folder, is its file name alone.
    const bool loose = game.file.find('/') == std::string::npos || file.find('/') == std::string::npos;
    if (loose ? Lower(BaseName(game.file)) == Lower(BaseName(file)) : Lower(game.file) == Lower(file)) return true;
    if (!game.title_id.empty() && title_id != 0) {
        char id[17];
        std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(title_id));
        return game.title_id == id;
    }
    if (game.identified) {
        const std::string& normal = game.normal_name;
        if (!normal.empty() && normal == normal_name) return true;
    }
    return false;
}

std::string TitleKey(const Game& game) {
    if (!game.title_id.empty()) return "title:" + game.title_id;
    if (!game.ids.empty()) return game.ids.begin()->first + ":" + game.ids.begin()->second;
    if (game.identified && !NormalName(game.name).empty()) return "name:" + NormalName(game.name);
    return "file:" + Lower(BaseName(game.file));
}

void Start(const Paths& paths, CoverWriter writer, FolderLister lister) {
    Shared& s = State_();
    std::unique_lock lock(s.lock);
    s.paths = paths;
    s.writer = writer;
    s.lister = lister;
    MakeFolders(paths.config);
    MakeFolders(paths.covers);
    ReadSources(s);
    if (!s.queue_read) {
        s.queue_read = true;
        LoadQueue(s);
        // What each one has on the console already, before its download goes on.
        for (Entry& entry : s.queue)
            if (const Game* game = FindGame(s, entry.source, entry.id)) {
                entry.total = game->size;
                entry.done = Present(s.paths, *game);
            }
    }
    RemoveLeftovers(s);
    for (Entry& entry : s.queue)
        // One whose download did not end before the menu was away; not the one still in a transfer
        // (Stop does not wait for ever).
        if ((entry.state == State::downloading || entry.state == State::verifying) &&
            !(s.transferring && entry.serial == s.current.load()))
            entry.state = State::queued;
    for (const auto& state : s.sources)
        if (state->source && (!state->listed_once || Clock::now() - state->listed_at > kRefreshAge))
            state->refresh_wanted = true;
    s.halt.store(false);
    if (!s.threads) {
        s.threads = true;
        std::thread(ListThread).detach();
        std::thread(DownloadThread).detach();
    }
    s.wake.notify_all();
}

void Stop() {
    Shared& s = State_();
    std::unique_lock lock(s.lock);
    s.halt.store(true);
    s.wake.notify_all();
    // A transfer asks whether to stop at least once a second.
    (void)s.wake.wait_for(lock, std::chrono::seconds(3), [&] { return !s.transferring && !s.listing; });
}

void Refresh() {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    for (const auto& state : s.sources)
        if (state->source) state->refresh_wanted = true;
    s.wake.notify_all();
}

void Changed() {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    ++s.generation;
}

Status Current() {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    Status status;
    status.configured = s.configured;
    status.error = s.error;
    status.generation = s.generation;
    status.ftp_port = s.ftp.port;
    for (const auto& state : s.sources) {
        SourceStatus source;
        source.key = state->key;
        source.name = state->name;
        source.address = state->source ? state->source->address() : std::string{};
        source.refreshing = state->refreshing || state->refresh_wanted;
        source.online = state->online;
        source.error = state->error;
        source.games = state->games.size();
        status.sources.push_back(std::move(source));
    }
    return status;
}

std::vector<Game> Games() {
    Shared& s = State_();
    std::vector<Game> games;
    {
        const std::lock_guard lock(s.lock);
        for (const auto& state : s.sources) games.insert(games.end(), state->games.begin(), state->games.end());
        for (Game& game : games)
            if (const std::string cover = CoverPath(s, game.source, game.id); s.covers.contains(cover)) game.cover = cover;
    }
    return games;
}

std::vector<Title> Titles() {
    std::vector<Title> titles;
    for (Game& game : Games()) {
        // With the title it is the same game as; a source's own games are never one title.
        Title* title = nullptr;
        for (Title& other : titles)
            if (std::none_of(other.games.begin(), other.games.end(), [&](const Game& g) { return g.source == game.source; }) &&
                SameGame(other.games.front(), game)) {
                title = &other;
                break;
            }
        if (title == nullptr) {
            // Its key, told apart from another title's (two copies of a game on one source).
            std::string key = TitleKey(game);
            for (int n = 2; std::any_of(titles.begin(), titles.end(), [&](const Title& t) { return t.key == key; }); ++n)
                key = TitleKey(game) + "#" + std::to_string(n);
            titles.push_back({key, {}});
            title = &titles.back();
        }
        title->games.push_back(std::move(game));
    }
    return titles;
}

bool Find(const std::string& source, const std::string& id, Game* game) {
    Shared& s = State_();
    {
        const std::lock_guard lock(s.lock);
        const Game* found = FindGame(s, source, id);
        if (found == nullptr) return false;
        if (game == nullptr) return true;
        *game = *found;
        if (const std::string cover = CoverPath(s, game->source, game->id); s.covers.contains(cover)) game->cover = cover;
    }
    return true;
}

std::vector<Download> Downloads() {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    std::vector<Download> list;
    for (const Entry& entry : s.queue) {
        Download download;
        download.source = entry.source;
        download.id = entry.id;
        download.state = entry.state;
        const bool running = entry.state == State::downloading || entry.state == State::verifying;
        download.done = running ? s.current_done.load() : entry.done;
        download.rate = entry.state == State::downloading ? s.current_rate.load() : 0;
        download.total = entry.total;
        if (download.total == 0)
            if (const Game* game = FindGame(s, entry.source, entry.id)) download.total = game->size;
        download.error = entry.error;
        list.push_back(std::move(download));
    }
    return list;
}

bool Enqueue(const std::string& source, const std::string& id, bool first) {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    const Game* game = FindGame(s, source, id);
    if (game == nullptr) return false;
    auto at = std::find_if(s.queue.begin(), s.queue.end(),
                           [&](const Entry& entry) { return entry.source == source && entry.id == id; });
    if (at == s.queue.end()) {
        // The same game from another source is already coming.
        for (const Entry& entry : s.queue)
            if (const Game* other = FindGame(s, entry.source, entry.id); other && SameGame(*other, *game)) return false;
        Entry entry;
        entry.serial = ++s.serials;
        entry.source = source;
        entry.id = id;
        entry.total = game->size;
        entry.done = Present(s.paths, *game);
        if (first) s.queue.push_front(entry);
        else s.queue.push_back(entry);
    } else {
        if (at->state == State::failed) {
            at->state = State::queued;
            at->error.clear();
        }
        // Cancelled while it downloads, and wanted again before the download stopped: it goes on.
        if (s.cancel.load() == at->serial) s.cancel.store(0);
        // Played now: it comes before the others (the one downloading goes on first).
        if (first && at->state == State::queued && at != s.queue.begin()) {
            Entry entry = *at;
            s.queue.erase(at);
            const auto after = std::find_if(s.queue.begin(), s.queue.end(),
                                            [](const Entry& e) {
                                                return e.state != State::downloading && e.state != State::verifying;
                                            });
            s.queue.insert(after, entry);
        }
    }
    SaveQueue(s);
    s.wake.notify_all();
    return true;
}

bool Cancel(const std::string& source, const std::string& id) {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    const auto at = std::find_if(s.queue.begin(), s.queue.end(),
                                 [&](const Entry& entry) { return entry.source == source && entry.id == id; });
    if (at == s.queue.end()) return false;
    if (at->state == State::downloading || at->state == State::verifying) {
        s.cancel.store(at->serial); // the download thread removes it and its files
        return true;
    }
    DiscardParts(s, source, id, FindGame(s, source, id));
    s.queue.erase(at);
    SaveQueue(s);
    return true;
}

std::vector<std::string> Files(const std::string& source, const std::string& id) {
    Shared& s = State_();
    const std::lock_guard lock(s.lock);
    std::vector<std::string> files;
    if (const Game* game = FindGame(s, source, id))
        for (const Part& part : game->parts)
            for (std::string path : {FinalPath(s.paths, *game, part), EarlierPath(s.paths, part)})
                if (FileSize(path) >= 0) files.push_back(std::move(path));
    return files;
}

} // namespace Eden::Remote
