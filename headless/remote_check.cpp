// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the download sources (remote/remote.h) and the save sync (remote/save_sync.h)
// where no real server is needed or a real one cannot be made to do it. Their basic workings are
// checked against real RomM servers and ftpsrv by tools/check-romm.py (romm_live_check.cpp); this
// has the rest: which game is which, the RomM backend's reading of RomM's answers, the save data's
// zips and RomM's times and versions, the check of a download's contents (stream_check.h) fed in
// every way a download feeds it, and, against tools/romm-mock-server.py and
// tools/ftp-mock-server.py, what a real RomM and ftpsrv do not do on demand: a server that hands out
// fewer games a page than asked for, games told apart by their ids at metadata providers and their
// title IDs, the same game under another file name on a second source, an update larger than its
// game, a server that cannot resume and a full drive. tools/check-remote.py runs it.
//
//   remote_check <server address> <empty folder> <FTP server's port>
#include "remote/backends.h"
#include "remote/ftp.h"
#include "remote/http.h"
#include "remote/remote.h"
#include "remote/romm/romm_client.h"
#include "remote/romm/romm_saves.h"
#include "remote/save_archive.h"
#include "remote/save_sync.h"
#include "remote/stream_check.h"

#include "miniz.h"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace Eden::Remote;

namespace {
int failures = 0;
void Expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string Pattern(int file_id, std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<char>((i * 7 + static_cast<std::size_t>(file_id)) & 0xFF);
    return data;
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
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

bool CopyCover(const std::string& encoded, const std::string& path) {
    Write(path, encoded);
    return encoded.size() > 8 && encoded.compare(1, 3, "PNG") == 0;
}

template <typename Done>
bool WaitFor(Done done, int seconds = 20) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

// A game's entry in the queue; an empty id when it has none.
Download Find(const std::vector<Download>& list, const std::string& source, const std::string& id) {
    for (const Download& download : list)
        if (download.source == source && download.id == id) return download;
    return {};
}
bool Queued(const std::string& source, const std::string& id) { return !Find(Downloads(), source, id).id.empty(); }

SourceStatus SourceOf(const std::string& key) {
    for (const SourceStatus& source : Current().sources)
        if (source.key == key) return source;
    return {};
}

// The games of a page of /api/roms as the RomM backend reads it; ok: whether it was one.
std::vector<SourceGame> Parse(const std::string& json, std::size_t* listed = nullptr, std::size_t* total = nullptr,
                              bool* ok = nullptr) {
    std::vector<SourceGame> games;
    const bool page = Romm::ParsePage(json, &games, listed, total);
    if (ok) *ok = page;
    return games;
}

// Which game is which (remote.h): title IDs, then provider ids, then names, then file names.
void Identity() {
    Expect(NormalName("Café: Let's Go, Nocturne!") == "cafeletsgonocturne", "accents and marks go");
    Expect(NormalName("THE STORY OF EMBER™ Knights of the Wild") == "thestoryofemberknightsofthewild",
           "case, spaces and symbols go");
    Expect(NormalName("Æon Straße Øresund") == "aeonstrasseoresund", "letters of two");
    Expect(NormalName("星のかけら") == "星のかけら" && NormalName("Маяк 3") == "Маяк3",
           "other scripts stay as they are, only spaces and marks go");
    const auto game = [](std::string name, std::string file, bool identified, std::string title_id,
                         std::map<std::string, std::string> ids) {
        Game g;
        g.name = std::move(name);
        g.file = std::move(file);
        g.identified = identified;
        g.title_id = std::move(title_id);
        g.ids = std::move(ids);
        return g;
    };
    const Game journey = game("Great Ember Journey", "Great.Ember.Journey.Dump.xci", true, "", {{"screenscraper", "123456"}});
    Expect(SameGame(journey, game("Great Ember Journey (EU)", "gej.xci", true, "", {{"screenscraper", "123456"}})),
           "the same provider id: the same game, whatever the names");
    Expect(!SameGame(journey, game("Great Ember Journey", "gej.xci", true, "", {{"screenscraper", "1"}})),
           "another id at the same provider: another game, although the names agree");
    Expect(SameGame(journey, game("GREAT EMBER JOURNEY", "other.xci", true, "", {{"igdb", "4321"}})),
           "no provider in common: the names decide");
    Expect(!SameGame(journey, game("Great Ember Journey", "other.xci", false, "", {})),
           "a name that is only guessed from a file name does not decide");
    Expect(SameGame(game("A", "Same File.nsp", false, "", {}), game("B", "same file.NSP", false, "", {})),
           "else the file names decide");
    Expect(!SameGame(game("Ember", "a.xci", true, "0100000000010000", {{"igdb", "1"}}),
                     game("Ember", "a.xci", true, "0100000000020000", {{"igdb", "1"}})),
           "the title IDs come first");
    const auto local = [](Game g, std::uint64_t title_id, const std::string& name, const std::string& file) {
        g.normal_name = NormalName(g.name); // as a source's list has it
        return SameAsLocal(g, title_id, NormalName(name), file);
    };
    Expect(local(game("Journey", "x.xci", true, "0100000000010000", {}), 0x0100000000010000ull, "Other", "y.xci"),
           "a game on the console by its title ID");
    Expect(local(journey, 0x0100000000010000ull, "Great Ember Journey™", "gej.xci"),
           "by its name, when the source does not know the title ID");
    Expect(!local(game("Ember", "x.xci", false, "", {}), 1, "Ember", "y.xci") &&
               local(game("Ember", "x.xci", false, "", {}), 1, "Other", "X.xci"),
           "by its file name, when the source's name is a guess");
    Expect(local(game("Galaxy 2", "Galaxy.2.nsp", true, "01B84DBAFD84A000", {}), 0x0100000000020000ull, "Galaxy 2",
                 "galaxy.2.NSP"),
           "the file it was downloaded as, although the title IDs disagree");
    Expect(local(game("Ember", "Ember/Ember.xci", false, "", {}), 1, "Other", "ember/EMBER.xci") &&
               local(game("Ember", "Ember/Ember.xci", false, "", {}), 1, "Other", "Ember.xci") &&
               !local(game("Ember", "Ember/Ember.xci", false, "", {}), 1, "Other", "Other/Ember.xci"),
           "a downloaded game by its path in roms/, or by its file name alone when the console's has no folder");
    Expect(SameGame(game("A", "One/Same.nsp", false, "", {}), game("B", "Two/same.NSP", false, "", {})),
           "the file names decide, whatever the game folders");
    Expect(TitleKey(journey) == "screenscraper:123456" && TitleKey(game("A", "F/F.nsp", false, "", {})) == "file:f.nsp" &&
               TitleKey(game("A", "F.nsp", false, "0100000000010000", {})) == "title:0100000000010000",
           "a title's key");
}

// The folder in roms/ a game is downloaded to.
void Folders() {
    Expect(GameFolder("Ember Journey [0100000000010000][v0].nsp", "7") == "Ember Journey" &&
               GameFolder("[Game] Ember Journey (USA).xci", "7") == "Ember Journey" &&
               GameFolder("Ember_Journey - [v0].nsp", "7") == "Ember_Journey",
           "the file's name without its extension and its tags");
    Expect(GameFolder("[0100000000010000].nsp", "7") == "[0100000000010000]" &&
               GameFolder(".Hidden (USA).nsp", "7") == "Hidden" && GameFolder("... .nsp", "a/b") == "a%2Fb",
           "a tag at its start stays; no leading dot; else the source's id");
    std::string long_name;
    while (long_name.size() < 300) long_name += "\xC3\xA9";
    const std::string folder = GameFolder(long_name + ".nsp", "7");
    Expect(folder.size() == 254 && folder == long_name.substr(0, 254), "at most 255 bytes, cut between characters");
}

// The RomM backend on its own.
void RommBackend() {
    Expect(Romm::NormalUrl(" nas.local:3000/ ") == "http://nas.local:3000", "address without a scheme");
    Expect(Romm::NormalUrl("HTTPS://romm.example.com//") == "https://romm.example.com", "https address");
    Expect(Romm::NormalUrl("ftp://nas") == "", "another scheme is refused");
    Expect(Romm::NormalUrl("") == "", "no address");

    // ROMs not on the server's disk are left out; a name falls back to the file's.
    std::size_t listed = 0;
    std::size_t total = 0;
    auto games = Parse(R"({"items":[{"id":4,"name":null,"fs_name_no_ext":"Named","files":[
                                   {"id":9,"file_name":"Named.xci","file_size_bytes":42,"category":"game"}]},
                               {"id":5,"name":"Gone","fs_name":"Gone.nsp","missing_from_fs":true,"files":[
                                   {"id":8,"file_name":"Gone.nsp","file_size_bytes":1,"category":"game"}]},
                               {"id":"x"}],"total":3})",
                       &listed, &total);
    Expect(listed == 3 && total == 3, "a page says its total");
    Expect(games.size() == 1 && games[0].id == "4" && games[0].name == "Named" && games[0].files.size() == 1 &&
               games[0].files[0].id == "9" && games[0].files[0].size == 42,
           "a ROM on the server's disk, its files by id");
    // What tells a game apart: its title ID, its provider ids, whether RomM identified it.
    games = Parse(R"({"items":[{"id":4,"name":"Journey","is_identified":true,"title_id":"0100000000010000",
                                 "ss_id":123456,"igdb_id":null,"moby_id":0,"launchbox_id":77,"libretro_id":"abc",
                                 "files":[{"id":9,"file_name":"O.xci","file_size_bytes":1,"category":"game"}]}],"total":1})");
    Expect(games.size() == 1 && games[0].identified && games[0].title_id == "0100000000010000" &&
               games[0].ids.size() == 3 && games[0].ids["screenscraper"] == "123456" && games[0].ids["launchbox"] == "77" &&
               games[0].ids["libretro"] == "abc",
           "its title ID and its ids at the providers it matched");
    // RomM's categories say what each file is; other files stay on the server.
    games = Parse(R"({"items":[{"id":7,"name":"Many","fs_name":"Many","files":[
                            {"id":1,"file_name":"Many [UPD].nsp","file_size_bytes":10,"category":"update"},
                            {"id":2,"file_name":"Many.nsp","file_size_bytes":900,"category":null,"is_top_level":true},
                            {"id":3,"file_name":"readme.txt","file_size_bytes":1,"category":"manual"},
                            {"id":4,"file_name":"Many [DLC].nsp","file_size_bytes":3,"category":"dlc"},
                            {"id":5,"file_name":"Many Mod.nsp","file_size_bytes":5,"category":"mod"},
                            {"id":6,"file_name":"Loose.nsp","file_size_bytes":5,"category":null,"is_top_level":false}],
                         "path_cover_small":"/assets/romm/resources/roms/1/7/cover/small.png?ts=1"}],
                         "total":9,"limit":1,"offset":0})",
                  &listed, &total);
    Expect(listed == 1 && total == 9, "a page says its total");
    Expect(games.size() == 1 && games[0].files.size() == 3 && games[0].files[0].kind == FileKind::update &&
               games[0].files[1].kind == FileKind::game && games[0].files[2].kind == FileKind::dlc,
           "the game, its update and DLC; manuals, mods and files without a category are left out");
    Expect(games.size() == 1 && games[0].cover == "/assets/romm/resources/roms/1/7/cover/small.png?ts=1",
           "the cover's place on the server");
    bool ok = true;
    Parse("not json", nullptr, nullptr, &ok);
    Expect(!ok, "an answer that is no JSON is no page");
    Parse("{}", nullptr, nullptr, &ok);
    Expect(!ok, "an object without items is no page");
    Parse(R"([{"id":8}])", nullptr, nullptr, &ok);
    Expect(!ok, "a plain list is no page");

    std::string error;
    Expect(MakeSource("romm", nlohmann::json::object(), &error) == nullptr && error.find("url") != std::string::npos,
           "a RomM source without an address is not usable");
    Expect(MakeSource("ftp", {{"url", "x"}}, &error) == nullptr && error.find("Unknown") != std::string::npos,
           "an unknown type is reported");
    Expect(FindBackend("romm") != nullptr && FindBackend("romm")->saves != nullptr,
           "RomM is a backend that keeps save data");
    Expect(MakeSaveStore("romm", {{"url", "nas:3000"}}, "/nowhere", &error) == nullptr && error.find("sign-in") != std::string::npos,
           "a RomM entry in save-sync.json without a sign-in keeps no save data");
    Expect(MakeSaveStore("", {}, "/nowhere", &error) == nullptr && MakeSaveStore("ftp", {}, "/nowhere", &error) == nullptr &&
               error.find("ftp") != std::string::npos,
           "no type, or an unknown one, keeps no save data");
    Expect(FindBackend("romm") != nullptr && FindBackend("romm")->source != nullptr && FindBackend("ftp") == nullptr,
           "RomM is a backend that has games to download");
    // The server's part of an entry, read for any config file: what goes wrong names that file.
    Expect(Romm::Client::Make(nlohmann::json::object(), "save-sync.json", &error) == nullptr &&
               error.find("save-sync.json") != std::string::npos,
           "a RomM entry without an address names its file");
    const auto client = Romm::Client::Make({{"url", "nas:3000/"}, {"username", "me"}, {"password", "secret"}},
                                           "save-sync.json", &error);
    Expect(client && client->url() == "http://nas:3000" && client->authorization() == "Basic bWU6c2VjcmV0" &&
               client->StatusError(401).find("save-sync.json") != std::string::npos,
           "a RomM entry's address and sign-in");
    // A file name comes from the source: a line break in it would be a command to the FTP server.
    FtpUpload upload;
    Expect(!upload.Open(FtpServer{}, "/data/x.nsp\r\nDELE /data/y", 0, &error) && error.find("line break") != std::string::npos,
           "a file name with a line break is not sent to the FTP server");
}

// What a real RomM and ftpsrv do not do on demand (see the top of the file).
void MockSpecials(const std::string& url, const fs::path& root, const std::string& ftp_port) {
    const fs::path config = root / "config" / "remote";
    const fs::path roms = root / "games" / "roms";
    const fs::path updates = root / "games" / "updates";
    const fs::path downloads = root / "games" / ".remote-downloads";
    fs::create_directories(config);
    fs::create_directories(roms);
    const Paths paths{config.string(), (root / "covers").string(), roms.string(), updates.string(), downloads.string()};
    const auto start = [&] { Eden::Remote::Start(paths, CopyCover, ListFolder); };
    const std::string token = "\"token\":\"rmm_test\"";
    const auto romm = [&](const std::string& name, const std::string& address) {
        return "{\"type\":\"romm\",\"name\":\"" + name + "\",\"url\":\"" + address + "\"," + token + "}";
    };
    std::string other = url;
    other.replace(other.find("127.0.0.1"), 9, "localhost");

    // Two sources (the mock under two addresses; as localhost it names Alpha Quest's file
    // otherwise). It hands out three games a page, whatever was asked for.
    Write(config / "sources.json", "{\"ftp_port\":" + ftp_port + ",\"sources\":[" + romm("Home", url) + "," +
                                       romm("Office", other) + "]}");
    start();
    Expect(WaitFor([] {
               return SourceOf("home").online && !SourceOf("home").refreshing && SourceOf("office").online &&
                      !SourceOf("office").refreshing;
           }),
           "both lists are read");
    Expect(Games().size() == 10, "five of seven ROMs on each source are games (not the .nsz, not the update on its own), "
                                 "read in pages of three");

    // Which game is which: by provider ids, title IDs, else file names; a second copy on one source
    // is a title of its own, and a game of another file name on a second source the same title.
    const std::vector<Title> titles = Titles();
    const auto title_of = [&](const std::string& id) {
        for (const Title& title : titles)
            for (const Game& game : title.games)
                if (game.source == "home" && game.id == id) return title;
        return Title{};
    };
    Expect(titles.size() == 5, "the same games on two sources are five titles");
    Expect(title_of("16").key == "screenscraper:1000#2", "a second copy on one source is a title of its own");
    Expect(title_of("10").key == "screenscraper:1000" && title_of("10").games.size() == 2 &&
               title_of("10").games[1].file == "Alpha Quest/Alpha Quest (Office).nsp",
           "a game of another file name on the second source is the same title");
    Expect(title_of("11").key == "title:0100000000011000" && title_of("13").key.starts_with("file:"),
           "titles by title ID, and by file name without anything else");
    Expect(Enqueue("office", "10", false) && !Enqueue("home", "10", false), "a game of another name is not queued twice");
    Expect(WaitFor([] { return !Queued("office", "10"); }) &&
               Read(roms / "Alpha Quest" / "Alpha Quest (Office).nsp") == Pattern(100, 10000000),
           "it is downloaded to its game folder under that source's file name");

    // A full drive: the download fails with what the FTP server said, and what it wrote stays in
    // .remote-downloads/, to go on from once there is room.
    Game epsilon;
    Expect(Eden::Remote::Find("home", "14", &epsilon) && epsilon.file == "Epsilon/Epsilon.nsp" && epsilon.parts.size() == 3 &&
               !epsilon.parts[0].update && epsilon.parts[1].update && epsilon.parts[2].update,
           "the game file first, an update larger than it after it; the mod is left out");
    fs::create_directories(downloads / "home" / "14");
    Write(downloads / "home" / "14" / "ftp-full", "");
    Expect(Enqueue("home", "14", false), "a game is queued for a full drive");
    Expect(WaitFor([] { return Find(Downloads(), "home", "14").state == State::failed; }), "its download fails");
    Expect(Find(Downloads(), "home", "14").error.find("No space left on device") != std::string::npos,
           "and says the drive is full, as the FTP server said");
    const fs::path written = downloads / "home" / "14" / "Epsilon [UPD][v131072].nsp";
    Expect(fs::exists(written) && fs::file_size(written) == 1000 && !fs::exists(roms / "Epsilon"),
           "what was written stays in .remote-downloads/, and no game folder is made yet");
    fs::remove(downloads / "home" / "14" / "ftp-full");

    // Room again: it goes on, and the update larger than its game goes with it as an update (the
    // categories decide, not the sizes); its mod stays on the server.
    start();
    Expect(Enqueue("home", "14", false) && WaitFor([] { return !Queued("home", "14"); }), "it goes on once there is room");
    Expect(Read(roms / "Epsilon" / "Epsilon.nsp") == Pattern(140, 50000) &&
               Read(roms / "Epsilon" / "Epsilon [UPD][v131072].nsp") == Pattern(141, 150000) &&
               Read(roms / "Epsilon" / "Epsilon [DLC].nsp") == Pattern(142, 20000) &&
               !fs::exists(roms / "Epsilon" / "Epsilon Mod.nsp") && !fs::exists(updates),
           "the game, its update and its DLC to its game folder, not its mod, and nothing to updates/");

    // A server that sends a game file whole although part of it is there: it starts again.
    fs::create_directories(downloads / "home" / "11");
    Write(downloads / "home" / "11" / "Beta Racer.xci", std::string(5000000, 'x'));
    Expect(Enqueue("home", "11", false) && WaitFor([] { return !Queued("home", "11"); }), "a game from such a server");
    Expect(Read(roms / "Beta Racer" / "Beta Racer.xci") == Pattern(110, 6000000) &&
               Read(roms / "Beta Racer" / "Beta Racer [UPD][v65536].nsp") == Pattern(111, 100000),
           "a download that could not resume starts again, and every byte is right");

    // A game downloaded before game folders: its file in roms/, its update in updates/. It is on
    // the console, a download of it keeps those files and makes no game folder, and its files are
    // found there, to be deleted.
    fs::remove_all(roms / "Beta Racer");
    fs::create_directories(updates);
    Write(roms / "Beta Racer.xci", Pattern(110, 6000000));
    Write(updates / "Beta Racer [UPD][v65536].nsp", Pattern(111, 100000));
    Game racer;
    Expect(Eden::Remote::Find("home", "11", &racer) && SameAsLocal(racer, 0, "", "Beta Racer.xci"),
           "a game downloaded before game folders is on the console");
    std::vector<std::string> earlier = Files("home", "11");
    std::sort(earlier.begin(), earlier.end());
    Expect(earlier == std::vector<std::string>{(roms / "Beta Racer.xci").string(),
                                               (updates / "Beta Racer [UPD][v65536].nsp").string()},
           "its files are where that download put them");
    Expect(Enqueue("home", "11", false) && WaitFor([] { return !Queued("home", "11"); }) &&
               !fs::exists(roms / "Beta Racer") && Read(roms / "Beta Racer.xci") == Pattern(110, 6000000),
           "a download of it keeps them, and makes no game folder");
    for (const std::string& path : earlier) fs::remove(path);
    Expect(Files("home", "11").empty(), "once they are deleted, none of it is on the console");
    Eden::Remote::Stop();
}

// ---- the check of a download's contents ----

void Put(std::string& to, std::size_t at, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) to[at + static_cast<std::size_t>(i)] = static_cast<char>(value >> (8 * i) & 0xFF);
}

// A partition as NSPs (PFS0, entries of 0x18 bytes) and XCIs (HFS0, 0x40) have them: its header,
// then its files.
std::string Partition(const char* magic, std::size_t entry_size, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string names;
    for (const auto& file : files) names += file.first + '\0';
    names.resize((names.size() + 0x1F) & ~std::size_t{0x1F}, '\0');
    std::string out(0x10 + files.size() * entry_size, '\0');
    out.replace(0, 4, magic);
    Put(out, 4, files.size(), 4);
    Put(out, 8, names.size(), 4);
    std::uint64_t offset = 0;
    std::size_t name_at = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t entry = 0x10 + i * entry_size;
        Put(out, entry, offset, 8);
        Put(out, entry + 8, files[i].second.size(), 8);
        Put(out, entry + 16, name_at, 4);
        offset += files[i].second.size();
        name_at += files[i].first.size() + 1;
    }
    out += names;
    for (const auto& file : files) out += file.second;
    return out;
}

// An NCA, named after its SHA-256 as a game's are; the contents list's name is not.
std::pair<std::string, std::string> Nca(int id, std::size_t size) {
    const std::string data = Pattern(id, size);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);
    char name[33];
    for (int i = 0; i < 16; ++i) std::snprintf(name + 2 * i, 3, "%02x", digest[i]);
    return {std::string(name) + ".nca", data};
}
std::pair<std::string, std::string> Cnmt() { return {"0123456789abcdef0123456789abcdef.cnmt.nca", Pattern(9, 3000)}; }

std::string Nsp(const std::vector<std::pair<std::string, std::string>>& files) { return Partition("PFS0", 0x18, files); }

// An XCI: its header (the root partition's place at 0x130), the root partition with the secure one
// in it, which has the NCAs.
std::string Xci(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string header(0x200, '\0');
    header.replace(0x100, 4, "HEAD");
    Put(header, 0x130, 0x200, 8);
    return header + Partition("HFS0", 0x40,
                              {{"update", Partition("HFS0", 0x40, {})},
                               {"normal", Partition("HFS0", 0x40, {})},
                               {"secure", Partition("HFS0", 0x40, files)}});
}

// Fed as a download feeds it: in pieces of `piece` bytes from `from` (a download that goes on).
Verified Fed(const std::string& name, const std::string& file, std::size_t piece, std::size_t from = 0,
             std::size_t* contents = nullptr) {
    StreamCheck check(name);
    for (std::size_t at = from; at < file.size(); at += piece)
        check.Feed(at, file.data() + at, std::min(piece, file.size() - at));
    if (contents != nullptr) *contents = check.Contents();
    return check.Result();
}

void StreamChecks() {
    const std::string nsp = Nsp({Nca(1, 3000000), Cnmt(), Nca(2, 70000)});
    std::size_t contents = 0;
    Expect(Fed("Game.nsp", nsp, nsp.size(), 0, &contents) == Verified::intact && contents == 2,
           "an NSP at once: its NCAs as named (the contents list is not checked)");
    Expect(Fed("Game.NSP", nsp, 7) == Verified::intact && Fed("Game.nsp", nsp, 65536) == Verified::intact,
           "in pieces of any size");
    std::string spoilt = nsp;
    spoilt[nsp.size() - 1000] ^= 1;
    Expect(Fed("Game.nsp", spoilt, 4096) == Verified::damaged, "a byte of an NCA other than it should be: damaged");

    const std::string xci = Xci({Nca(3, 2500000), Cnmt(), Nca(4, 1000)});
    Expect(Fed("Game.xci", xci, 4096, 0, &contents) == Verified::intact && contents == 2, "an XCI's secure partition");
    spoilt = xci;
    spoilt[xci.size() - 2000000] ^= 0x40;
    Expect(Fed("Game.xci", spoilt, 1 << 20) == Verified::damaged, "an XCI with a byte spoilt");

    // A download that goes on: what the file had, read from the drive, then the rest from a little
    // before where it was (its last bytes come again).
    {
        StreamCheck check("Game.nsp");
        check.Feed(0, nsp.data(), 2000000);
        check.Feed(1500000, nsp.data() + 1500000, nsp.size() - 1500000);
        Expect(check.Next() == nsp.size() && check.Result() == Verified::intact, "fed again over what it had: intact");
    }
    // The server sends the whole file after all: checked from the start again.
    {
        StreamCheck check("Game.nsp");
        check.Feed(0, spoilt.data(), 1000000);
        check.Restart();
        check.Feed(0, nsp.data(), nsp.size());
        Expect(check.Result() == Verified::intact, "from the start again: what came before does not count");
    }
    // Bytes it never got: unchecked, not damaged.
    {
        StreamCheck check("Game.nsp");
        check.Feed(0, nsp.data(), 1000);
        check.Feed(2000, nsp.data() + 2000, nsp.size() - 2000);
        Expect(check.Result() == Verified::unknown, "a gap leaves it unchecked");
    }
    Expect(Fed("Game.nsp", nsp, 4096, 100) == Verified::unknown, "a file not fed from its start: unchecked");
    Expect(Fed("Game.nsp", nsp.substr(0, nsp.size() - 5), 4096) == Verified::unknown, "a file that ends early: unchecked");
    Expect(Fed("Game.nsz", nsp, 4096) == Verified::unknown && Fed("Game.nsp", Pattern(5, 100000), 4096) == Verified::unknown &&
               Fed("Game.xci", Pattern(5, 100000), 4096) == Verified::unknown && Fed("Game.nsp", "", 1) == Verified::unknown,
           "no NSP or XCI: unchecked");
    Expect(Fed("Game.nsp", Nsp({Cnmt()}), 4096) == Verified::unknown, "an NSP without NCAs named after their contents: unchecked");
    // A header that says more entries than there can be is no header.
    std::string huge = nsp;
    Put(huge, 4, 0xFFFFFFFF, 4);
    Expect(Fed("Game.nsp", huge, 4096) == Verified::unknown, "a header of absurd size: unchecked");
    // An NCA of a size or at a place no file has (the sums would wrap round): unchecked, no crash.
    for (const std::uint64_t value : {~std::uint64_t{0}, ~std::uint64_t{0} - 0x100}) {
        std::string wrapped = nsp;
        Put(wrapped, 0x10 + 8, value, 8); // the first entry's size
        std::string far = nsp;
        Put(far, 0x10, value, 8); // the first entry's place
        Expect(Fed("Game.nsp", wrapped, 4096) == Verified::unknown && Fed("Game.nsp", far, 4096) == Verified::unknown,
               "an entry past any file's end: unchecked");
    }
}

// ---- save data's zips ----

// A folder of save data: name -> contents.
void Fill(const fs::path& folder, const std::map<std::string, std::string>& files) {
    fs::remove_all(folder);
    for (const auto& [name, data] : files) {
        fs::create_directories((folder / name).parent_path());
        Write(folder / name, data);
    }
}

std::map<std::string, std::string> Contents(const fs::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(folder, error))
        if (entry.is_regular_file()) files[fs::relative(entry.path(), folder).string()] = Read(entry.path());
    return files;
}

void SaveArchives(const fs::path& root) {
    std::string error, hash;
    // RomM's content hash: of the files, by their names, whatever the zip is like.
    Expect(SaveArchive::Md5("abc", 3) == "900150983cd24fb0d6963f7d28e17f72", "MD5");
    Fill(root / "a", {{"main.sav", "one"}, {"sub/x.bin", "two"}});
    SaveArchive::Folder files;
    Expect(SaveArchive::List((root / "a").string(), "0100000000011000", ReadFolder, &files, &error) &&
               files.files.size() == 2 && files.files[0].first == "0100000000011000/main.sav" &&
               files.folders == std::vector<std::string>{"0100000000011000/sub/"} && files.bytes == 6,
           "a save folder's files by their names in the zip");
    const std::string lines = "0100000000011000/main.sav:" + SaveArchive::Md5("one", 3) +
                              "\n0100000000011000/sub/x.bin:" + SaveArchive::Md5("two", 3);
    Expect(SaveArchive::Hash(files, &hash, &error) && hash == SaveArchive::Md5(lines.data(), lines.size()),
           "the content hash is RomM's");
    std::string zipped_hash;
    Expect(SaveArchive::Pack(files, (root / "a.zip").string(), &error) &&
               SaveArchive::HashZip((root / "a.zip").string(), &zipped_hash, &error) && zipped_hash == hash,
           "a packed folder has the folder's hash");
    Expect(SaveArchive::Unpack((root / "a.zip").string(), (root / "b").string(), "0100000000011000", &error) &&
               Contents(root / "b") == Contents(root / "a") && fs::is_directory(root / "b" / "sub"),
           "unpacked, the folder's contents without its top folder");
    Expect(!SaveArchive::Unpack((root / "a.zip").string(), (root / "b").string(), "0100000000011000", &error),
           "not over a folder that is there");
    SaveArchive::Folder none;
    Expect(SaveArchive::List((root / "missing").string(), "X", ReadFolder, &none, &error) && none.files.empty(),
           "no folder: no save data");
    // A folder in it that cannot be read: no save data is taken from the rest.
    SaveArchive::Folder partial;
    Expect(!SaveArchive::List((root / "a").string(), "0100000000011000",
                              [](const std::string& folder, std::vector<std::string>* names) {
                                  return !folder.ends_with("sub") && ReadFolder(folder, names);
                              },
                              &partial, &error) &&
               error.find("sub") != std::string::npos,
           "a folder that cannot be read whole fails the listing");

    // JKSV's zips have the files at their top and a file of its own; one with a way out is refused.
    const auto zip = [&](const std::string& path, const std::vector<std::pair<std::string, std::string>>& entries) {
        mz_zip_archive archive{};
        bool ok = mz_zip_writer_init_file(&archive, path.c_str(), 0);
        for (const auto& [name, data] : entries)
            ok = ok && mz_zip_writer_add_mem(&archive, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL);
        ok = ok && mz_zip_writer_finalize_archive(&archive);
        return mz_zip_writer_end(&archive) && ok;
    };
    Expect(zip((root / "jksv.zip").string(), {{"main.sav", "1"}, {".nx_save_meta.bin", "m"}, {"sub/x", "2"}}) &&
               SaveArchive::Unpack((root / "jksv.zip").string(), (root / "c").string(), "0100000000011000", &error) &&
               Contents(root / "c") == std::map<std::string, std::string>{{"main.sav", "1"}, {"sub/x", "2"}},
           "a JKSV zip, without its .nx_save_meta.bin");
    Expect(zip((root / "evil.zip").string(), {{"0100/main.sav", "1"}, {"0100/../../evil", "2"}}) &&
               !SaveArchive::Unpack((root / "evil.zip").string(), (root / "d").string(), "0100", &error) &&
               error.find("outside") != std::string::npos && !fs::exists(root / "evil"),
           "a zip with a way out of its folder is refused");
    // Save data that is one folder of the game's own, without the title's folder around it: kept.
    Expect(zip((root / "own.zip").string(), {{"save/slot1.bin", "1"}, {"save/system.bin", "2"}}) &&
               SaveArchive::Unpack((root / "own.zip").string(), (root / "e").string(), "0100000000011000", &error) &&
               Contents(root / "e") == std::map<std::string, std::string>{{"save/slot1.bin", "1"}, {"save/system.bin", "2"}},
           "a zip of one folder that is not the title's keeps it");
    // Packed on a Mac: its own files beside the title's folder are left out, the folder taken off.
    Expect(zip((root / "mac.zip").string(), {{"0100000000011000/main.sav", "1"}, {"__MACOSX/0100000000011000/._main.sav", "x"},
                                             {".DS_Store", "x"}, {"0100000000011000/.DS_Store", "x"}}) &&
               SaveArchive::Unpack((root / "mac.zip").string(), (root / "f").string(), "0100000000011000", &error) &&
               Contents(root / "f") == std::map<std::string, std::string>{{"main.sav", "1"}},
           "a Mac's zip: its __MACOSX and .DS_Store left out");

    Expect(Romm::ParseTime("2026-10-08T12:34:56.123456+00:00") == 1791462896 &&
               Romm::ParseTime("2026-10-08T12:34:56Z") == 1791462896 &&
               Romm::ParseTime("2026-10-08T14:34:56+02:00") == 1791462896 && Romm::ParseTime("never") == 0 &&
               Romm::FormatTime(1791462896) == "2026-10-08T12:34:56Z",
           "RomM's times");
    // The oldest version the save sync takes, and around it.
    int major = 0, minor = 0, patch = 0;
    std::sscanf(Romm::kMinimumVersion, "%d.%d.%d", &major, &minor, &patch);
    const auto version = [](int a, int b, int c) { return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c); };
    Expect(Romm::CanSync(Romm::kMinimumVersion) && Romm::CanSync(version(major, minor, patch + 1)) &&
               Romm::CanSync(version(major, minor + 1, 0) + "-beta.1") && Romm::CanSync(version(major + 1, 0, 0)) &&
               Romm::CanSync("development") && !Romm::CanSync("3.10.0") &&
               (patch == 0 || !Romm::CanSync(version(major, minor, patch - 1))) &&
               (minor == 0 || !Romm::CanSync(version(major, minor - 1, 9))) && !Romm::CanSync(version(major - 1, 99, 0)),
           "the RomM versions the save sync takes: kMinimumVersion and newer");
}

} // namespace

int main(int argc, char** argv) {
    Identity();
    Folders();
    RommBackend();
    StreamChecks();
    if (argc >= 3) SaveArchives(fs::path(argv[2]) / "archives");
    if (argc >= 4) MockSpecials(argv[1], argv[2], argv[3]);
    if (failures == 0) std::printf("remote check: PASS\n");
    return failures == 0 ? 0 : 1;
}
