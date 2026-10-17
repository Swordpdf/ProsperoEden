// SPDX-License-Identifier: GPL-3.0-or-later
// Check of the save sync (remote/save_sync.h) and the RomM backends (remote/romm/) against a real
// RomM server with fake games, as tools/check-romm.py sets one up in Docker: the game list, a
// first upload, a large save data with many files, a second console, a device deleted in RomM,
// the versions RomM keeps, another device's save data (an Argosy zip and a JKSV zip), another
// emulator's and another slot's save data, a slot emptied in RomM, both sides changed, a token
// without the scopes and a server that does not answer. Another device is played through RomM's
// API, as Argosy would. A RomM older than the save sync takes (Romm::kMinimumVersion) is only
// checked to be refused at each sync.
//
// The download sources are checked too: the server as a source (token, user and password, a
// wrong token), a game of a few hundred MB downloaded through ftpsrv (the console's FTP server,
// built for Linux) and compared with the server's file byte for byte,
// a download stopped part way and gone on with, its contents checked as they come (an NSP's and an
// XCI's spoilt on the drive found damaged), a game with its update and DLC, and a cancel.
//
//   romm_live_check <server address> <token> <token with roms.read only> <empty folder>
//                   <the server's library folder> <ftpsrv's port>
#include "remote/backends.h"
#include "remote/http.h"
#include "remote/remote.h"
#include "remote/romm/romm_client.h"
#include "remote/romm/romm_saves.h"
#include "remote/save_archive.h"
#include "remote/save_sync.h"

#include "miniz.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace Eden::Remote;
using Json = nlohmann::json;

namespace {
int failures = 0;
void Expect(bool ok, const char* what) {
    std::fprintf(stderr, "%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

const char* kTitle = "0100000000010000";
const char* kName = "Alpha Quest";
const char* kFile = "Alpha Quest [0100000000010000][v0].nsp";

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

std::vector<std::string> ListFolder(const std::string& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) names.push_back(entry.path().filename().string());
    return names;
}

// The same for the save sync (SaveArchive::Lister): false when the folder cannot be read.
bool ReadFolder(const std::string& folder, std::vector<std::string>* names) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) names->push_back(entry.path().filename().string());
    return !error;
}

std::map<std::string, std::string> Contents(const fs::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(folder, error))
        if (entry.is_regular_file()) files[fs::relative(entry.path(), folder).string()] = Read(entry.path());
    return files;
}

// RomM names an upload after its second: another device's next upload waits for the next one.
void NextSecond() { std::this_thread::sleep_for(std::chrono::milliseconds(1100)); }

struct Server {
    std::string url;
    std::string token;
    std::unique_ptr<Romm::Client> client;
    int rom = 0;

    Json Ask(const char* method, const std::string& path, const Json& body = {}) const {
        Romm::Client::Answer answer;
        std::string error;
        if (!client->Send(method, path, body.is_null() ? std::string{} : body.dump(), &answer, &error)) return {};
        return Json::parse(answer.body, nullptr, false);
    }
    // The save data of the game on the server: slot, emulator, content hash, by id.
    std::vector<Json> Saves() const {
        const Json saves = Ask(nullptr, "/api/saves?rom_id=" + std::to_string(rom));
        return saves.is_array() ? std::vector<Json>(saves.begin(), saves.end()) : std::vector<Json>{};
    }
    int Count(const std::string& slot, const std::string& emulator) const {
        int count = 0;
        for (const Json& save : Saves())
            count += save.value("slot", "") == slot && save.value("emulator", "") == emulator;
        return count;
    }
};

// A console: its save folder of the game and what its save sync keeps.
struct Console {
    fs::path root;
    SyncPlaces Places() const {
        SyncPlaces places;
        places.save = (root / "nand" / kTitle).string();
        places.store = (root / "config" / "P1").string();
        places.work = (root / "config" / "P1" / "work").string();
        places.backups = (root / "backup").string();
        places.lister = ReadFolder;
        return places;
    }
    fs::path Save() const { return root / "nand" / kTitle; }
    SyncResult Sync(const std::string& url, const std::string& token, SyncChoice choice = SyncChoice::neither,
                    bool* asked = nullptr) const {
        const Json settings = {{"type", "romm"}, {"url", url}, {"token", token}};
        const SyncResult result = SyncSaveData("romm", settings, {0x0100000000010000ull, kName, kFile}, Places(),
                                               [&](const SavePlan&, const LocalSave&) {
                                                   if (asked) *asked = true;
                                                   return choice;
                                               },
                                               [] { return false; });
        if (result.outcome == SyncOutcome::failed) std::fprintf(stderr, "      the sync failed: %s\n", result.message.c_str());
        return result;
    }
};

// Another device of the same user (a phone with Argosy): it uploads through RomM's API.
struct Phone {
    const Server* server = nullptr;
    std::string device;
    fs::path work;

    // A zip of save data: Argosy's (its folder at the top) or JKSV's (its files at the top).
    std::string Zip(const std::string& text, bool jksv) const {
        const std::string path = (work / "phone.zip").string();
        mz_zip_archive archive{};
        const std::string name = jksv ? "progress.bin" : std::string{kTitle} + "/progress.bin";
        bool made = mz_zip_writer_init_file(&archive, path.c_str(), 0) &&
                    mz_zip_writer_add_mem(&archive, name.c_str(), text.data(), text.size(), MZ_DEFAULT_LEVEL);
        if (jksv) made = made && mz_zip_writer_add_mem(&archive, ".nx_save_meta.bin", "meta", 4, MZ_DEFAULT_LEVEL);
        made = mz_zip_writer_finalize_archive(&archive) && made;
        return mz_zip_writer_end(&archive) && made ? path : std::string{};
    }
    bool Upload(const std::string& text, bool jksv = false, const std::string& emulator = "eden",
                const std::string& slot = "autosave") const {
        char escaped[256];
        if (remote_http_escape(slot.c_str(), escaped, sizeof(escaped)) != 0) return false;
        const std::string zip = Zip(text, jksv);
        Romm::Client::Answer answer;
        std::string error;
        const bool sent = !zip.empty() &&
                          server->client->Upload("POST",
                                                 "/api/saves?rom_id=" + std::to_string(server->rom) + "&emulator=" +
                                                     emulator + "&slot=" + escaped + "&device_id=" + device +
                                                     "&overwrite=true",
                                                 "saveFile", zip, std::string{kTitle} + ".zip", &answer, &error);
        if (sent && (answer.status == 200 || answer.status == 201)) return true;
        std::fprintf(stderr, "      the phone's upload: %d %s%s\n", answer.status, error.c_str(), answer.body.substr(0, 300).c_str());
        return false;
    }
};

void GameList(const Server& server) {
    std::string error;
    std::vector<SourceGame> games;
    const auto source = MakeSource("romm", {{"url", server.url}, {"token", server.token}}, &error);
    Expect(source && source->list(&games, &error, [] { return false; }) && games.size() == 6,
           "the fake games are listed");
    for (const SourceGame& game : games) {
        if (game.files.size() != 3) continue; // Beta Racer, with its update and DLC
        int kinds[3] = {};
        for (const SourceFile& file : game.files) ++kinds[static_cast<int>(file.kind)];
        Expect(kinds[0] == 1 && kinds[1] == 1 && kinds[2] == 1, "a game's file, its update and its DLC by RomM's categories");
    }
}

bool WaitFor(const std::function<bool()>& done, int seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}

SourceStatus SourceOf(const std::string& key) {
    for (const SourceStatus& source : Current().sources)
        if (source.key == key) return source;
    return {};
}

Download Queued(const std::string& source, const std::string& id) {
    for (const Download& download : Downloads())
        if (download.source == source && download.id == id) return download;
    return {};
}

// A source's game by its file name (its path in roms/ without its game folder); empty when it has
// none.
Game GameOf(const std::string& file) {
    for (const Game& game : Games())
        if (fs::path(game.file).filename() == file) return game;
    return {};
}

bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code error;
    if (fs::file_size(a, error) != fs::file_size(b, error) || error) return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    std::vector<char> one(1 << 20), two(1 << 20);
    while (x && y) {
        x.read(one.data(), one.size());
        y.read(two.data(), two.size());
        if (x.gcount() != y.gcount() || !std::equal(one.begin(), one.begin() + x.gcount(), two.begin())) return false;
    }
    return true;
}

// The cover as the source sends it, kept as it came (the console turns it into a TGA).
bool KeepCover(const std::string& encoded, const std::string& path) {
    Write(path, encoded);
    return !encoded.empty();
}

std::string Address(const std::string& url, const std::string& host) {
    std::string other = url;
    other.replace(other.find("127.0.0.1"), 9, host);
    return other;
}

void SourceDownloads(const Server& server, const fs::path& library, const std::string& ftp_port, const fs::path& work) {
    const fs::path config = work / "config" / "remote";
    const fs::path roms = work / "games" / "roms";
    const fs::path updates = work / "games" / "updates";
    const fs::path downloads = work / "games" / ".remote-downloads";
    fs::create_directories(config);
    fs::create_directories(roms);
    const Paths paths{config.string(), (work / "covers").string(), roms.string(), updates.string(), downloads.string()};
    const fs::path switch_roms = library / "roms" / "switch";
    const auto start = [&] { Start(paths, KeepCover, ListFolder); };
    const auto romm = [&](const std::string& name, const std::string& address, const std::string& sign_in) {
        return "{\"type\":\"romm\"," + (name.empty() ? std::string{} : "\"name\":\"" + name + "\",") + "\"url\":\"" +
               address + "\"," + sign_in + "}";
    };
    const std::string token = "\"token\":\"" + server.token + "\"";
    std::string ftp = ftp_port;
    // sources.json with these sources, and the menu opened: true once their lists are in (or failed).
    const auto setup = [&](const std::string& sources) {
        Write(config / "sources.json", "{\"ftp_port\":" + ftp + ",\"sources\":[" + sources + "]}");
        start();
        return WaitFor([] {
            for (const SourceStatus& source : Current().sources)
                if (source.refreshing || (!source.online && source.error.empty())) return false;
            return true;
        }, 60);
    };

    // No sources.json, and one that is not valid.
    start();
    Expect(!Current().configured && Games().empty(), "no source without sources.json");
    Write(config / "sources.json", "[]");
    start();
    Expect(!Current().configured && !Current().error.empty(), "a sources.json without a list says so");

    // The server as a download source: a wrong token is refused and said so; a token, and a user's
    // name and password, read its games. A .nsz, a mod, a manual and an update on its own are not
    // listed as games.
    Expect(setup(romm("Test", server.url, "\"token\":\"rmm_wrong\"")) && !SourceOf("test").online &&
               SourceOf("test").error.find("did not accept") != std::string::npos,
           "a wrong token is refused, and the menu says so");
    Expect(setup(romm("Test", server.url, "\"username\":\"player\",\"password\":\"player-check\"")) &&
               SourceOf("test").online && Games().size() == 4,
           "with the player's name and password, the four games are listed (no .nsz, no update on its own)");
    Expect(setup(romm("Test", server.url, token)) && SourceOf("test").online && Games().size() == 4 &&
               SourceOf("test").address == server.url && SourceOf("test").name == "Test",
           "with a token, too: the source's name and address");
    Expect(fs::exists(config / "test" / "catalog.json"), "the list is kept for the next start");
    const std::uint64_t generation = Current().generation;
    Refresh();
    Expect(WaitFor([] { return !SourceOf("test").refreshing; }, 60) && Current().generation == generation,
           "an unchanged list read again does not have the Library read again");
    Expect(WaitFor([] { return !GameOf("Alpha Quest [0100000000010000][v0].nsp").cover.empty(); }, 60) &&
               fs::file_size(GameOf("Alpha Quest [0100000000010000][v0].nsp").cover) > 0,
           "a game's cover set in RomM is fetched");
    const Game racer = GameOf("Beta Racer.xci");
    Expect(!racer.id.empty() && racer.parts.size() == 3 && !racer.parts[0].update && racer.parts[1].update &&
               racer.parts[2].update,
           "a game's file first, its update and DLC after it; its mod and manual stay on the server");

    // Without an FTP server on the console a download fails, and says why.
    const Game gamma = GameOf("Gamma.nsp");
    ftp = "1";
    setup(romm("Test", server.url, token));
    Expect(Enqueue("test", gamma.id, false) &&
               WaitFor([&] { return Queued("test", gamma.id).state == State::failed; }, 60) &&
               Queued("test", gamma.id).error.find("FTP server") != std::string::npos,
           "without an FTP server a download fails, and says it needs one");
    Expect(Cancel("test", gamma.id), "it is cancelled");
    ftp = ftp_port;
    setup(romm("Test", server.url, token));

    // A game of a few hundred MB: stopped part way, gone on with where it was, and every byte right.
    const Game big = GameOf("Delta Big.nsp");
    Expect(!big.id.empty() && big.size == fs::file_size(switch_roms / "Delta Big.nsp"), "the large game, with its size");
    Expect(Enqueue("test", big.id, true), "it is queued");
    Expect(WaitFor([&] { return Queued("test", big.id).done > big.size / 4; }, 120), "it downloads");
    Stop();
    const fs::path partial = downloads / "test" / big.id / "Delta Big.nsp";
    std::error_code error;
    const auto stopped_at = fs::file_size(partial, error);
    Expect(!error && stopped_at > 0 && stopped_at < big.size && !fs::exists(roms / "Delta Big") &&
               Queued("test", big.id).state == State::queued,
           "stopped part way (a game starts): it stays queued, what came stays in .remote-downloads/");
    // Its last bytes spoilt, as a power cut may leave them: they are fetched again.
    {
        std::fstream file(partial, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(stopped_at - 100000));
        file << std::string(100000, 'x');
    }
    // Going on, what the file has is read from the drive first, for the check of its contents
    // (the queue shows it as verifying, up to where it was), then it downloads from where it was,
    // less the last 4 MB (fetched again in case of a power cut), not from the start. Watched closely:
    // a drive reads it in moments.
    bool read_first = false;
    std::uint64_t first = 0;
    start();
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
         std::chrono::steady_clock::now() < until; std::this_thread::sleep_for(std::chrono::microseconds(200))) {
        const Download download = Queued("test", big.id);
        if (download.state == State::verifying && download.done > 0 && download.done < stopped_at) read_first = true;
        if ((download.state == State::downloading && download.done > 0) || download.id.empty()) {
            first = download.done;
            break;
        }
    }
    Expect(read_first, "what came before is read from the drive first, and the queue shows it checking");
    Expect(first + (5u << 20) >= stopped_at, "it goes on from where it was (RomM sends the rest: Range)");
    Expect(WaitFor([&] { return Queued("test", big.id).id.empty(); }, 300) && Queued("test", big.id).error.empty(),
           "it goes on and is done");
    Expect(SameFile(roms / "Delta Big" / "Delta Big.nsp", switch_roms / "Delta Big.nsp"),
           "every byte as the server has it, the spoilt end too, in the game's folder in roms/");
    Expect(!fs::exists(downloads / "test"), "nothing is left in .remote-downloads/");

    // A file whose contents are not what they are named after (here what a download that stopped
    // left, spoilt on the drive before where it goes on): deleted, the download fails and says so;
    // trying again downloads it again. An NSP's and an XCI's.
    const auto spoilt = [&](const Game& game, const fs::path& server_file) {
        const std::string name = server_file.filename().string();
        std::string part = Read(server_file).substr(0, 5u << 20);
        part[512u << 10] = static_cast<char>(part[512u << 10] ^ 1); // before where it goes on (5 MB less 4)
        Write(downloads / "test" / game.id / name, part);
        return Enqueue("test", game.id, false) &&
               WaitFor([&] { return Queued("test", game.id).state == State::failed; }, 120) &&
               Queued("test", game.id).error.find("damaged") != std::string::npos &&
               !fs::exists(downloads / "test" / game.id / name) && !fs::exists(roms / GameFolder(name, game.id));
    };
    Expect(spoilt(gamma, switch_roms / "Gamma.nsp"), "an NSP found damaged is deleted, and the download fails saying so");
    Expect(spoilt(racer, switch_roms / "Beta Racer" / "Beta Racer.xci"), "an XCI found damaged, too");
    Expect(Enqueue("test", racer.id, false) && WaitFor([&] { return Queued("test", racer.id).id.empty(); }, 120) &&
               SameFile(roms / "Beta Racer" / "Beta Racer.xci", switch_roms / "Beta Racer" / "Beta Racer.xci"),
           "tried again, it is downloaded again, and comes out right");
    fs::remove_all(roms / "Beta Racer");
    Changed();
    Expect(WaitFor([&] { return !GameOf("Beta Racer.xci").id.empty(); }, 20), "deleted again, for what follows");

    // Begun from a longer file than the server has now: it starts again (416), and comes out right.
    Write(downloads / "test" / gamma.id / "Gamma.nsp", std::string(fs::file_size(switch_roms / "Gamma.nsp") + 5000, 'y'));
    Expect(Enqueue("test", gamma.id, false) && WaitFor([&] { return Queued("test", gamma.id).id.empty(); }, 120) &&
               SameFile(roms / "Gamma" / "Gamma.nsp", switch_roms / "Gamma.nsp"),
           "a download begun longer than the server's file starts again (after the damaged one: tried again)");

    // A game with its update and DLC: all of them to the game's folder in roms/.
    Expect(Enqueue("test", racer.id, false) && WaitFor([&] { return Queued("test", racer.id).id.empty(); }, 120),
           "a game with its update and DLC is downloaded");
    const fs::path racer_folder = roms / "Beta Racer";
    Expect(SameFile(racer_folder / "Beta Racer.xci", switch_roms / "Beta Racer" / "Beta Racer.xci") &&
               SameFile(racer_folder / "Beta Racer [UPD][v65536].nsp",
                        switch_roms / "Beta Racer" / "update" / "Beta Racer [UPD][v65536].nsp") &&
               SameFile(racer_folder / "Beta Racer [DLC].nsp", switch_roms / "Beta Racer" / "dlc" / "Beta Racer [DLC].nsp"),
           "the game, its update and its DLC in the game's folder in roms/");
    Expect(!fs::exists(racer_folder / "Beta Racer Mod.nsp") && !fs::exists(updates), "its mod is not downloaded, and nothing goes to updates/");

    // A cancel: the download stops, what came of it goes; an update that was in the game's folder
    // already (the player's own) stays.
    fs::remove_all(roms / "Delta Big");
    Changed();
    Expect(WaitFor([&] { return !GameOf("Delta Big.nsp").id.empty(); }, 20), "a deleted game is on the source only again");
    Expect(Enqueue("test", big.id, false) && WaitFor([&] { return Queued("test", big.id).done > 0; }, 60),
           "it downloads again");
    Expect(Cancel("test", big.id) && WaitFor([&] { return !fs::exists(downloads / "test" / big.id); }, 30) &&
               !fs::exists(roms / "Delta Big"),
           "a cancel stops it, and what came goes");
    fs::remove(racer_folder / "Beta Racer.xci");
    Write(racer_folder / "Beta Racer [UPD][v65536].nsp", "the player's own");
    Changed();
    Stop();
    Write(downloads / "test" / racer.id / "Beta Racer.xci", std::string(1000, 'x'));
    Expect(Enqueue("test", racer.id, false) && Cancel("test", racer.id), "a queued game is cancelled");
    Expect(WaitFor([&] { return !fs::exists(downloads / "test" / racer.id); }, 30) &&
               Read(racer_folder / "Beta Racer [UPD][v65536].nsp") == "the player's own",
           "a cancel deletes what was downloaded, and nothing in roms/");

    // What .remote-downloads/ has for a game that is not queued goes when the menu opens; a queued
    // game's download stays.
    Stop();
    Expect(Enqueue("test", gamma.id, false), "a game is queued while the menu is away");
    Write(downloads / "test" / gamma.id / "kept", "x");
    Write(downloads / "test" / "99" / "Gone.nsp", "x");
    Write(downloads / "gone" / "1" / "x", "x");
    Write(downloads / "stray", "x");
    start();
    Stop();
    Expect(fs::exists(downloads / "test" / gamma.id / "kept") && !fs::exists(downloads / "test" / "99") &&
               !fs::exists(downloads / "gone") && !fs::exists(downloads / "stray"),
           "leftovers of games not in the queue are removed");
    Expect(Cancel("test", gamma.id), "the queued one is cancelled");

    // Two sources (the same server under two addresses), one of an unknown type: each lists its
    // games, the same game is one title, queued from one source only.
    const std::string other = Address(server.url, "localhost");
    Expect(setup(romm("Home", server.url, token) + "," + romm("Office", other, token) + ",{\"type\":\"ftp\",\"name\":\"Old\"}"),
           "two sources and one of an unknown type are read");
    Expect(Current().sources.size() == 3 && SourceOf("home").online && SourceOf("office").online && Games().size() == 8 &&
               Titles().size() == 4,
           "each source has its games, the same games are one title each");
    Expect(!SourceOf("old").error.empty() && SourceOf("old").address.empty(), "a source of an unknown type says so");
    Game office_gamma;
    for (const Game& game : Games())
        if (game.source == "office" && game.file == "Gamma/Gamma.nsp") office_gamma = game;
    fs::remove_all(roms / "Gamma");
    Changed();
    Expect(Enqueue("office", office_gamma.id, false) && !Enqueue("home", gamma.id, false),
           "a game is queued from one source, not from the other one too");
    Expect(WaitFor([&] { return Queued("office", office_gamma.id).id.empty(); }, 120) &&
               SameFile(roms / "Gamma" / "Gamma.nsp", switch_roms / "Gamma.nsp"),
           "it is downloaded from the second source");

    // Sources without a name are told apart.
    setup(romm("", server.url, token) + "," + romm("", other, token));
    Expect(Current().sources.size() == 2 && Current().sources[0].name == "romm" && Current().sources[1].name == "romm (2)",
           "two sources without a name have names of their own");

    // A source taken out of sources.json: what was queued from it says so.
    setup(romm("Home", server.url, token) + "," + romm("Office", other, token));
    fs::remove_all(roms / "Gamma");
    Changed();
    Stop();
    Expect(Enqueue("office", office_gamma.id, false), "queued from the second source");
    setup(romm("Home", server.url, token));
    Expect(WaitFor([&] { return Queued("office", office_gamma.id).state == State::failed; }, 60) &&
               Cancel("office", office_gamma.id),
           "a game whose source is gone fails, and is cancelled");

    // A new address for the same source: the queue goes on with it.
    Stop();
    Expect(Enqueue("home", gamma.id, false), "queued before the address changes");
    Expect(Read(config / "queue.json") == "[{\"id\":\"" + gamma.id + "\",\"source\":\"home\"}]",
           "the queue is kept for the next start");
    setup(romm("Home", other, token));
    Expect(WaitFor([&] { return Queued("home", gamma.id).id.empty(); }, 120) &&
               SameFile(roms / "Gamma" / "Gamma.nsp", switch_roms / "Gamma.nsp"),
           "the queue goes on with the new address");
    Stop();
}

void SaveSync(Server& server, const std::string& bare, const fs::path& work) {
    const Console a{work / "a"};
    const Console b{work / "b"};
    const auto sync = [&](const Console& console, SyncChoice choice = SyncChoice::neither, bool* asked = nullptr) {
        return console.Sync(server.url, server.token, choice, asked);
    };

    // The first console's save data goes up; unchanged, nothing goes.
    Write(a.Save() / "progress.bin", "a 1");
    Write(a.Save() / "sub" / "x.bin", "x");
    SyncResult result = sync(a);
    Expect(result.outcome == SyncOutcome::uploaded && server.Count("autosave", "eden") == 1, "the first upload");
    Expect(sync(a).outcome == SyncOutcome::same, "unchanged, nothing to do");

    // Large save data with many files, and a second console that gets it.
    std::string big(150u << 20, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 2654435761u) >> 13);
    Write(a.Save() / "big.bin", big);
    for (int i = 0; i < 400; ++i) Write(a.Save() / "many" / ("f" + std::to_string(i) + ".txt"), std::to_string(i));
    Expect(sync(a).outcome == SyncOutcome::uploaded, "150 MB and 400 files go up");
    Expect(sync(b).outcome == SyncOutcome::downloaded && Contents(b.Save()) == Contents(a.Save()),
           "a second console gets them as they are");
    fs::remove(a.Save() / "big.bin");
    fs::remove_all(a.Save() / "many");
    Write(a.Save() / "progress.bin", "a 2");
    NextSecond();
    Expect(sync(a).outcome == SyncOutcome::uploaded && sync(b).outcome == SyncOutcome::downloaded &&
               Contents(b.Save()) == Contents(a.Save()),
           "and follows the first one's next save data");

    // The console's device deleted in RomM: it is registered again, and its save data still goes up.
    const Json device = Json::parse(Read(a.root / "config" / "P1" / "romm-device.json"), nullptr, false);
    server.Ask("DELETE", "/api/devices/" + device.value("device_id", ""));
    Write(a.Save() / "progress.bin", "a 3");
    NextSecond();
    Expect(sync(a).outcome == SyncOutcome::uploaded &&
               Json::parse(Read(a.root / "config" / "P1" / "romm-device.json"), nullptr, false).value("device_id", "") !=
                   device.value("device_id", ""),
           "a device deleted in RomM is registered again, without a conflict");

    // RomM keeps ten versions of the slot.
    for (int i = 4; i < 16; ++i) {
        Write(a.Save() / "progress.bin", "a " + std::to_string(i));
        NextSecond();
        if (sync(a).outcome != SyncOutcome::uploaded) Expect(false, "each new version goes up");
    }
    Expect(server.Count("autosave", "eden") <= 10, "RomM keeps ten versions of the slot");

    // Another device's save data: an Argosy zip, then a JKSV one; both consoles follow.
    Phone phone{&server, {}, work};
    const Json made = server.Ask("POST", "/api/devices",
                                 {{"name", "Test Phone"}, {"platform", "android"}, {"hostname", "check-phone"}});
    phone.device = made.is_object() ? made.value("device_id", "") : std::string{};
    if (phone.device.empty()) std::fprintf(stderr, "      the phone was not registered: %s\n", made.dump().c_str());
    sync(b);
    NextSecond();
    Expect(phone.Upload("phone 1") && sync(a).outcome == SyncOutcome::downloaded &&
               Read(a.Save() / "progress.bin") == "phone 1" && !fs::exists(a.Save() / "sub"),
           "another device's save data (Argosy's zip) comes down");
    NextSecond();
    Expect(phone.Upload("phone jksv", true) && sync(a).outcome == SyncOutcome::downloaded &&
               Contents(a.Save()) == std::map<std::string, std::string>{{"progress.bin", "phone jksv"}},
           "a JKSV zip comes down without its .nx_save_meta.bin");
    Expect(sync(a).outcome == SyncOutcome::same, "and then the console is in step with it");
    Expect(sync(b).outcome == SyncOutcome::downloaded && sync(b).outcome == SyncOutcome::same,
           "the second console follows it and is in step");

    // Another emulator's save data in the slot, and the game's other slots, are left alone.
    NextSecond();
    Expect(phone.Upload("yuzu's", false, "yuzu") && phone.Upload("before the boss", false, "eden", "before boss"),
           "the phone uploads another emulator's and another slot's save data");
    Expect(sync(a).outcome == SyncOutcome::same && Read(a.Save() / "progress.bin") == "phone jksv",
           "they do not come down");
    Write(a.Save() / "progress.bin", "a after yuzu");
    NextSecond();
    Expect(sync(a).outcome == SyncOutcome::uploaded && sync(a).outcome == SyncOutcome::same,
           "the console's goes up beside them without asking");

    // Both changed: the player chooses. Played before the phone's came in, then after.
    sync(b);
    Write(a.Save() / "progress.bin", "a offline");
    fs::last_write_time(a.Save() / "progress.bin", fs::file_time_type::clock::now() - std::chrono::hours(24));
    NextSecond();
    phone.Upload("phone 2");
    bool asked = false;
    result = sync(a, SyncChoice::neither, &asked);
    Expect(asked && result.outcome == SyncOutcome::kept && Read(a.Save() / "progress.bin") == "a offline",
           "played offline before another device's came in: asked, and left as it is");
    asked = false;
    result = sync(a, SyncChoice::console, &asked);
    Expect(asked && result.outcome == SyncOutcome::uploaded, "the console's chosen: it goes up");
    Expect(sync(b).outcome == SyncOutcome::downloaded && Read(b.Save() / "progress.bin") == "a offline",
           "and the second console, unchanged, follows it without asking");
    NextSecond();
    phone.Upload("phone 3");
    Write(a.Save() / "progress.bin", "a after the phone");
    asked = false;
    result = sync(a, SyncChoice::server, &asked);
    Expect(asked && result.outcome == SyncOutcome::downloaded && Read(a.Save() / "progress.bin") == "phone 3",
           "played after another device's came in: asked; the server's chosen: it comes down");
    const auto backups = ListFolder((a.root / "backup" / kTitle).string());
    Expect(!backups.empty() && backups.size() <= 3, "replaced save data is in the backups, three at most");

    // The slot emptied in RomM: the console that has the save data uploads it again.
    Json ids = Json::array();
    for (const Json& save : server.Saves())
        if (save.value("slot", "") == "autosave" && save.value("emulator", "") == "eden") ids.push_back(save["id"]);
    server.Ask("POST", "/api/saves/delete", {{"saves", ids}});
    Expect(server.Count("autosave", "eden") == 0 && sync(a).outcome == SyncOutcome::uploaded &&
               server.Count("autosave", "eden") == 1,
           "a slot emptied in RomM is filled again by the console");

    // A token without the scopes, a game the server lacks, a server that does not answer.
    result = a.Sync(server.url, bare);
    Expect(result.outcome == SyncOutcome::failed && result.message.find("scope") != std::string::npos,
           "a token without the scopes names one");
    result = SyncSaveData("romm", {{"url", server.url}, {"token", server.token}},
                          {0x0100000000099000ull, "Nowhere", "Nowhere.nsp"}, a.Places(), nullptr, [] { return false; });
    Expect(result.outcome == SyncOutcome::no_game, "a game the server does not have");
    result = a.Sync("http://127.0.0.1:9", server.token);
    Expect(result.outcome == SyncOutcome::failed && !result.message.empty(), "a server that does not answer");
}
// Pairing (pairing.h): a profile signed in by a code approved on the server, without typing.
void Pairings(const Server& server, const fs::path& work) {
    const Pairing* pairing = FindBackend("romm")->pairing;
    Expect(pairing != nullptr, "RomM's save stores can be paired");
    const fs::path folder = work / "pair" / "P1";
    const Json settings = {{"type", "romm"}, {"url", server.url}, {"platform", "switch"}};
    std::string error;
    // The player approves on the server as themself (here: through its API, as its page does).
    std::string ignored;
    const auto player = Romm::Client::Make({{"url", server.url}, {"username", "player"}, {"password", "player-check"}},
                                           "the check", &ignored);
    const auto answer = [&](const char* what, const std::string& code) {
        Romm::Client::Answer reply;
        std::string failed;
        const Json scopes = Json::array({"platforms.read", "roms.read", "assets.read", "assets.write", "devices.read",
                                         "devices.write", "me.read"});
        const Json body = std::string{what} == "approve" ? Json{{"user_code", code}, {"approved_scopes", scopes}} :
                                                           Json{{"user_code", code}};
        return player->Send("POST", std::string{"/api/auth/device/"} + what, body.dump(), &reply, &failed) &&
               (reply.status == 200 || reply.status == 201 || reply.status == 204);
    };
    const auto devices = [&] {
        const Json list = server.Ask(nullptr, "/api/devices");
        return list.is_array() ? list.size() : std::size_t{0};
    };

    // Cancelled before it asked: nothing is sent, it says it stopped.
    PairingStart unasked;
    Expect(!pairing->start(settings, folder.string(), &unasked, &error, [] { return true; }) && unasked.device_code.empty(),
           "a pairing cancelled stops what it sends");
    PairingStart start;
    Expect(pairing->start(settings, folder.string(), &start, &error, {}) && !start.device_code.empty() &&
               start.user_code.size() >= 6 && start.address.starts_with(server.url + "/pair/device?user_code=") &&
               start.expires_in > 0 && start.interval > 0,
           "a pairing gives a code and the address to approve it at");
    PairingResult result = pairing->poll(settings, folder.string(), start, {});
    Expect(result.state == PairingState::pending && result.interval > 0, "not approved yet: it waits");
    const std::size_t before = devices();
    Expect(answer("approve", start.user_code), "the player approves it on the server");
    std::this_thread::sleep_for(std::chrono::seconds(start.interval));
    result = pairing->poll(settings, folder.string(), start, {});
    Expect(result.state == PairingState::approved && result.user == "player" && result.entry.value("type", "") == "romm" &&
               result.entry.value("url", "") == server.url && result.entry.value("token", "").starts_with("rmm_") &&
               result.entry.value("platform", "") == "switch",
           "approved: a token of the player's own for the profile's entry, the source's platform with it");
    Expect(devices() == before + 1, "RomM made a device of the console for it");

    // The save sync with that token is the device RomM made: no second one.
    Json entry = settings;
    entry["token"] = result.entry["token"];
    Write(folder.parent_path() / "nand" / kTitle / "progress.bin", "paired");
    SyncPlaces places;
    places.save = (folder.parent_path() / "nand" / kTitle).string();
    places.store = folder.string();
    places.work = (folder / "work").string();
    places.backups = (folder.parent_path() / "backup").string();
    places.lister = ReadFolder;
    const SyncResult synced = SyncSaveData("romm", entry, {0x0100000000010000ull, kName, kFile}, places, nullptr,
                                           [] { return false; });
    Expect((synced.outcome == SyncOutcome::uploaded || synced.outcome == SyncOutcome::kept) && devices() == before + 1,
           "the paired profile syncs as that device");

    // Denied on the server: the console says so.
    PairingStart denied;
    Expect(pairing->start(settings, (work / "pair" / "P2").string(), &denied, &error, {}) && answer("deny", denied.user_code),
           "another pairing is denied on the server");
    std::this_thread::sleep_for(std::chrono::seconds(denied.interval));
    Expect(pairing->poll(settings, (work / "pair" / "P2").string(), denied, {}).state == PairingState::denied,
           "and the console is told");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: romm_live_check <server address> <token> <roms.read token> <empty folder> "
                             "<library folder> <ftpsrv's port>\n");
        return 2;
    }
    std::string error;
    Server server;
    server.url = argv[1];
    server.token = argv[2];
    server.client = Romm::Client::Make({{"url", server.url}, {"token", server.token}}, "the check", &error);
    std::vector<SourceGame> games;
    if (!server.client || !server.client->Games(&games, [] { return false; }, &error)) {
        std::fprintf(stderr, "romm live check: %s\n", error.c_str());
        return 1;
    }
    // The game by its file: RomM names a game it did not identify as it likes (4.9: by its file).
    for (const SourceGame& game : games)
        for (const SourceFile& file : game.files)
            if (file.name == kFile) server.rom = std::stoi(game.id);
    if (server.rom == 0) {
        std::fprintf(stderr, "romm live check: the server has no %s\n", kFile);
        return 1;
    }
    const Json heartbeat = server.Ask(nullptr, "/api/heartbeat");
    const std::string version = heartbeat.is_object() ? heartbeat["SYSTEM"].value("VERSION", "") : "";
    std::fprintf(stderr, "RomM %s\n", version.c_str());
    GameList(server);
    SourceDownloads(server, argv[5], argv[6], fs::path(argv[4]) / "sources");
    if (Romm::CanSync(version)) {
        SaveSync(server, argv[3], argv[4]);
        Pairings(server, argv[4]);
    } else {
        // Pairing needs the same RomM.
        PairingStart start;
        std::string refused;
        Expect(!FindBackend("romm")->pairing->start({{"url", server.url}}, (fs::path(argv[4]) / "pair" / "P1").string(),
                                                    &start, &refused, {}) &&
                   refused.find(Romm::kMinimumVersion) != std::string::npos,
               "pairing with a RomM older than kMinimumVersion says it needs that one");
        // Older than the save sync takes: every sync says so, with both versions, and changes nothing.
        const Console console{fs::path(argv[4]) / "old"};
        Write(console.Save() / "progress.bin", "old");
        for (int sync = 0; sync < 2; ++sync) {
            const SyncResult result = console.Sync(server.url, server.token);
            Expect(result.outcome == SyncOutcome::failed && result.too_old && result.version == version &&
                       result.needed == Romm::kMinimumVersion,
                   "a RomM older than kMinimumVersion is not synced with, at each sync");
        }
        Expect(server.Count("autosave", "eden") == 0 && Read(console.Save() / "progress.bin") == "old",
               "and nothing changed on either side");
    }
    if (failures == 0) std::printf("romm live check (RomM %s): PASS\n", version.c_str());
    else std::printf("romm live check (RomM %s): %d FAILED\n", version.c_str(), failures);
    return failures == 0 ? 0 : 1;
}
