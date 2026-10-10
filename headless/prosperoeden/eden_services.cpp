// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_services.h"

#include "assets_dir.h"
#include "crash_report.h"
#include "diagnostics.h"
#include "metadata_bridge.h"
#include "mods.h"
#include "profiles.h"
#include "update_notice.h"
#include "game_name.h"
#include "native_directory.h"
#include "pe/core/strings.hpp"
#include "radio_input.h"
#include "remote/backends.h"
#include "remote/remote.h"
#include "remote/save_sync.h"
#include "save_sync_config.h"
#include "version.h"
#include "../system_keyboard.h"
#include "common/string_util.h"

#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using pe::fill;
using pe::tr;

bool IsFile(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// A game's update and DLC: "Update 1.2.0, 2 DLC"; brief leaves the word out ("v1.2.0, 2 DLC") for
// places with little room.
std::string AddOnSummary(uint64_t title_id, bool brief = false) {
    char update[64]{};
    unsigned dlc = 0;
    eden_game_addons(title_id, update, sizeof(update), &dlc);
    std::string text;
    if (update[0]) text = brief ? (update[0] == 'v' ? std::string{update} : "v" + std::string{update}) :
                                  fill(tr("Update {0}"), {update});
    if (dlc) text += (text.empty() ? "" : ", ") + fill(tr("{0} DLC"), {std::to_string(dlc)});
    return text;
}

// The language a game will use for the chosen one (Settings > Language), and a note when the game
// does not offer the choice and falls back to another language.
struct GameLanguage {
    std::string label;
    std::string note;
};
GameLanguage LanguageFor(const std::string& path, uint64_t title_id, int choice) {
    const int chosen = Eden::kLanguageSettings[choice];
    const int used = eden_game_language(path.c_str(), Eden::AssetsPath("keys").c_str(), title_id, chosen);
    GameLanguage result{tr(Eden::kLanguageLabels[choice]), {}};
    if (used == chosen) return result;
    result.label = tr("Another language");
    for (std::size_t i = 0; i < std::size(Eden::kLanguageSettings); ++i)
        if (Eden::kLanguageSettings[i] == used) result.label = tr(Eden::kLanguageLabels[i]);
    result.note = fill(tr("{0} not available"), {tr(Eden::kLanguageLabels[choice])});
    return result;
}

// Why a game did not start, as headless/main.cpp reports it. The reasons that are whole sentences
// are shown in the player's language; the others carry codes and file names and stay as they are.
// (tools/launcher/strings.py checks that main.cpp still says these.)
constexpr const char* kLaunchErrors[] = {
    TR("Selected ROM is no longer available"),
    TR("PS5 controller initialization failed"),
    TR("Graphics backend initialization failed. Try another backend in Settings; see stderr.log and eden_log.txt for "
       "driver details."),
    TR("The game ran out of graphics memory. Lower the resolution in Settings, Video (or in the game's own settings) "
       "and start it again."),
};
std::string LaunchError(const std::string& reason) {
    std::string text = reason;
    for (const char* known : kLaunchErrors)
        if (reason == known) text = tr(known);
    // "Details:" follows it: a reason without its own full stop gets one.
    if (!text.empty() && text.back() != '.' && text.back() != '!' && text.back() != '?') text += '.';
    return text;
}

// Names of the subfolders (folders = true) or regular files in path, sorted without regard
// to case. Unlike ReadNativeDirectory, an odd entry is skipped rather than failing the
// folder: the Game files browser walks the whole console filesystem.
std::vector<std::string> ListEntries(const std::string& path, bool folders, bool& ok) {
    ok = false;
    std::vector<std::string> names;
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return names;
    std::vector<char> buffer(65536);
    for (;;) {
        const int count = sceKernelGetdents(fd, buffer.data(), static_cast<int>(buffer.size()));
        if (count == 0) { ok = true; break; }
        if (count < 0 || count > static_cast<int>(buffer.size())) break;
        for (std::size_t offset = 0; offset + offsetof(dirent, d_name) < static_cast<std::size_t>(count);) {
            uint16_t length;
            uint8_t type;
            std::memcpy(&length, buffer.data() + offset + offsetof(dirent, d_reclen), sizeof(length));
            std::memcpy(&type, buffer.data() + offset + offsetof(dirent, d_type), sizeof(type));
            if (length <= offsetof(dirent, d_name) || offset + length > static_cast<std::size_t>(count)) break;
            const char* name = buffer.data() + offset + offsetof(dirent, d_name);
            const std::string entry(name, strnlen(name, length - offsetof(dirent, d_name)));
            offset += length;
            if (entry.empty() || entry == "." || entry == "..") continue;
            bool is_folder = type == DT_DIR, is_file = type == DT_REG;
            if (type == DT_UNKNOWN || type == DT_LNK) {
                struct stat info {};
                const std::string full = path == "/" ? "/" + entry : path + "/" + entry;
                if (stat(full.c_str(), &info) != 0) continue;
                is_folder = S_ISDIR(info.st_mode);
                is_file = S_ISREG(info.st_mode);
            }
            if (folders ? is_folder : is_file) names.push_back(entry);
        }
    }
    close(fd);
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        return lower(a) < lower(b);
    });
    return names;
}

std::string JoinPath(const std::string& directory, const std::string& name) {
    return directory == "/" ? "/" + name : directory + "/" + name;
}

// Files in directory with one of the (lower-case) extensions; -1 when it cannot be read.
int CountFiles(const std::string& directory, std::initializer_list<const char*> extensions) {
    bool ok = false;
    int count = 0;
    for (const auto& name : ListEntries(directory, false, ok)) {
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* extension : extensions)
            if (lower.size() > std::strlen(extension) && lower.ends_with(extension)) { ++count; break; }
    }
    return ok ? count : -1;
}

unsigned int HashPath(const std::string& path) {
    unsigned int hash = 2166136261u;
    for (const unsigned char byte : path) hash = (hash ^ byte) * 16777619u;
    return hash;
}

// A title from the file's name: without its extension and the tags dumps carry
// ("Name [0100...][v0]", "Name (USA)").
std::string CleanTitle(const std::string& filename) {
    std::string title = std::filesystem::path(filename).stem().string();
    if (title.rfind("[Game] ", 0) == 0) title.erase(0, 7);
    if (const auto tag = title.find_first_of("[("); tag != std::string::npos && tag > 0) title.erase(tag);
    while (!title.empty() && (title.back() == ' ' || title.back() == '_' || title.back() == '-')) title.pop_back();
    return title.empty() ? std::filesystem::path(filename).stem().string() : title;
}

// Covers are keyed by the ROM's file name, not its full path, so they survive a new game
// files folder.
std::string CoverPath(const std::string& filename) {
    char name[16]{};
    std::snprintf(name, sizeof(name), "/%08x.tga", HashPath(filename));
    return Eden::CoversDir() + name;
}

// The game's own name is kept beside its cover once the ROM has been read, so the home screen
// can name its games without opening them.
std::string NamePath(const std::string& filename) {
    char name[16]{};
    std::snprintf(name, sizeof(name), "/%08x.name", HashPath(filename));
    return Eden::CoversDir() + name;
}

std::string SavedTitle(const std::string& filename) {
    char text[513]{};
    if (std::FILE* file = std::fopen(NamePath(filename).c_str(), "rb")) {
        const std::size_t size = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        text[size] = '\0';
    }
    return text;
}

void SaveTitle(const std::string& filename, const std::string& title) {
    if (title.empty() || SavedTitle(filename) == title) return;
    const std::string path = NamePath(filename);
    const std::string staged = path + ".new";
    std::FILE* file = std::fopen(staged.c_str(), "wb");
    if (!file) return;
    const bool written = std::fwrite(title.data(), 1, title.size(), file) == title.size();
    if (std::fclose(file) != 0 || !written || std::rename(staged.c_str(), path.c_str()) != 0)
        (void)std::remove(staged.c_str());
}

// The name kept for a game, when it is one that can be shown (game_name.h: an earlier version
// kept whatever the game's first name held), else the name its file gives.
std::string GameTitle(const std::string& filename) {
    const std::string saved = SavedTitle(filename);
    return Eden::UsableName(saved) ? Eden::PlainName(saved) : CleanTitle(filename);
}

// The cached cover of a ROM, extracted from it when missing; empty when it has none.
std::string EnsureCover(const std::string& filename, std::string* title = nullptr) {
    const std::string cover = CoverPath(filename);
    if (Eden::FileExists(cover) && !title) return cover;
    const std::string rom = Eden::AssetsPath("roms/" + filename);
    if (!Eden::FileExists(rom)) return {};
    (void)mkdir(Eden::CoversDir().c_str(), 0777);
    char extracted[513]{};
    const int metadata = eden_extract_game_metadata(rom.c_str(), Eden::AssetsPath("keys").c_str(), cover.c_str(),
                                                    extracted, sizeof(extracted));
    if (metadata & EDEN_METADATA_TITLE) {
        SaveTitle(filename, extracted);
        if (title) *title = extracted;
    }
    if (metadata & EDEN_METADATA_COVER) return cover;
    Eden::Report("cover", ("No cover extracted from " + filename).c_str());
    return Eden::FileExists(cover) ? cover : std::string{};
}

// The names of the download sources, by their keys.
std::map<std::string, std::string> SourceNames() {
    std::map<std::string, std::string> names;
    if (Eden::FilesystemAccess())
        for (const Eden::Remote::SourceStatus& source : Eden::Remote::Current().sources) names[source.key] = source.name;
    return names;
}

// "6.4 GB", "512.0 MB".
std::string SizeLabel(double bytes) {
    char size[32];
    if (bytes >= 1073741824.0) std::snprintf(size, sizeof(size), "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(size, sizeof(size), "%.1f MB", bytes / 1048576.0);
    return size;
}

// The folders and files in a folder, read the console's way (ListEntries).
std::vector<std::string> ListFolder(const std::string& folder) {
    bool ok = false;
    std::vector<std::string> names = ListEntries(folder, true, ok);
    for (std::string& name : ListEntries(folder, false, ok)) names.push_back(std::move(name));
    return names;
}

bool WriteCover(const std::string& encoded, const std::string& path) {
    return eden_write_cover_tga(reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(), path.c_str()) != 0;
}

int CountInstalledGames() {
    std::error_code error;
    const auto entries = Eden::ReadNativeDirectory(Eden::AssetsPath("roms"), error);
    if (error) return 0;
    int count = 0;
    for (const auto& entry : entries) {
        const std::string filename = entry.path().filename().string();
        if (Eden::ValidRomFilename(filename) && IsFile(Eden::AssetsPath("roms/" + filename))) ++count;
    }
    return count;
}

// The labels of a setting, in the player's language (tools/launcher/strings.py lists them).
template <std::size_t N>
std::vector<std::string> Labels(const char* const (&values)[N]) {
    std::vector<std::string> labels;
    for (const char* value : values) labels.emplace_back(tr(value));
    return labels;
}

// What is missing from the setup, as metadata_bridge.cpp words it, in the player's language. Each
// message is one of these texts, with a folder or a code where it says {0}.
std::string SetupMessage(const std::string& english) {
    static constexpr const char* kMessages[] = {
        TR("Missing or empty keys/prod.keys in {0}."),
        TR("prod.keys could not supply an NCA header key. Replace it with a valid key dump."),
        TR("Cannot read firmware/ in {0}. Install extracted firmware NCAs."),
        TR("A firmware NCA cannot be read. Reinstall the firmware dump."),
        TR("Firmware NCA validation failed (code {0}). Check that firmware and prod.keys are compatible."),
        TR("No firmware NCAs found in {0}."),
        TR("Firmware SystemVersion data is missing or unreadable. Install a complete firmware dump."),
        TR("Setup validation failed. Check that firmware and key files are readable and valid."),
    };
    for (const std::string_view pattern : kMessages) {
        const std::size_t hole = pattern.find("{0}");
        if (hole == std::string_view::npos) {
            if (english == pattern) return tr(english);
            continue;
        }
        const std::string_view before = pattern.substr(0, hole);
        const std::string_view after = pattern.substr(hole + 3);
        if (english.size() >= before.size() + after.size() && english.starts_with(before) &&
            english.ends_with(after))
            return fill(tr(std::string(pattern)),
                        {std::string_view{english}.substr(before.size(),
                                                           english.size() - before.size() - after.size())});
    }
    return english;
}

} // namespace

namespace {
// The PS5's users: who is in front, who is signed in, and their names.
struct LoginUsers {
    int id[4];
};
extern "C" int sceUserServiceInitialize(const void* parameters);
extern "C" int sceUserServiceGetForegroundUser(int* user);
extern "C" int sceUserServiceGetLoginUserIdList(LoginUsers* list);
extern "C" int sceUserServiceGetUserName(int user, char* name, std::size_t size);

// The names a profile can take: the signed-in PS5 users', then "Player 1" to "Player 8".
std::vector<std::string> ProfileNames() {
    std::vector<std::string> names;
    LoginUsers users{{-1, -1, -1, -1}};
    if (sceUserServiceGetLoginUserIdList(&users) >= 0) {
        for (const int user : users.id) {
            char name[17]{};
            if (user < 0 || sceUserServiceGetUserName(user, name, sizeof(name)) < 0 || name[0] == '\0') continue;
            if (std::find(names.begin(), names.end(), name) == names.end()) names.emplace_back(name);
        }
    }
    for (int number = 1; number <= static_cast<int>(Eden::Profiles::kMax); ++number) {
        const std::string name = "Player " + std::to_string(number);
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

// save-sync.json follows the profiles: an entry for each, in their order, with their names
// (save_sync_config.h). Only with filesystem access, where the remote config folder is.
void KeepSaveSyncFile(const std::vector<Eden::Profiles::Profile>& profiles) {
    if (!Eden::FilesystemAccess() || profiles.empty()) return;
    std::vector<Eden::SaveSync::Owner> owners;
    for (const auto& profile : profiles) owners.push_back({profile.Key(), profile.name});
    if (!Eden::SaveSync::Reconcile(owners))
        Eden::Report("save sync", "save-sync.json is not readable as JSON: it is left as it is");
}

// ---- save sync ----

std::string SyncFolder() { return Eden::ConfigFile("remote") + "/save-sync"; }
// The games whose save data is still to be synced after they were played, by profile.
std::string PendingFile() { return SyncFolder() + "/pending.json"; }

// A field of a JSON object as text; empty when it is not there or no text (save-sync.json and
// sources.json are edited by hand).
std::string TextOf(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
std::string TextOf(const Eden::SaveSync::Json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// A profile's entry in save-sync.json when it names a backend: what syncs its save data.
struct SyncSetup {
    std::string type;
    nlohmann::json settings;
    bool automatic = true;
};
bool SyncSetupOf(const std::string& profile, SyncSetup* setup) {
    if (!Eden::FilesystemAccess() || profile.empty()) return false;
    const Eden::SaveSync::Config config = Eden::SaveSync::Read();
    const Eden::SaveSync::Entry* entry = config.readable ? Eden::SaveSync::Find(config, profile) : nullptr;
    if (entry == nullptr || entry->type.empty()) return false;
    setup->type = entry->type;
    setup->settings = nlohmann::json::parse(entry->settings.dump(), nullptr, false);
    setup->automatic = config.automatic;
    return true;
}

// The names in a save folder (SaveArchive::Lister): false when it cannot be read whole.
bool ListSaveFolder(const std::string& folder, std::vector<std::string>* names) {
    bool ok = false;
    *names = ListEntries(folder, true, ok);
    if (!ok) return false;
    std::vector<std::string> files = ListEntries(folder, false, ok);
    names->insert(names->end(), files.begin(), files.end());
    return ok;
}

std::vector<std::pair<std::string, std::string>> ReadPending() {
    std::vector<std::pair<std::string, std::string>> pending;
    std::string text;
    if (!Eden::Settings::ReadFile(PendingFile(), text)) return pending;
    const nlohmann::json list = nlohmann::json::parse(text, nullptr, false);
    if (!list.is_array()) return pending;
    for (const nlohmann::json& item : list)
        if (!TextOf(item, "profile").empty() && !TextOf(item, "file").empty())
            pending.emplace_back(TextOf(item, "profile"), TextOf(item, "file"));
    return pending;
}

void WritePending(const std::vector<std::pair<std::string, std::string>>& pending) {
    if (pending.empty()) {
        (void)std::remove(PendingFile().c_str());
        return;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const auto& [profile, file] : pending) list.push_back({{"profile", profile}, {"file", file}});
    std::error_code ignored;
    std::filesystem::create_directories(SyncFolder(), ignored);
    (void)Eden::Settings::WriteFile(PendingFile(), list.dump(2) + "\n");
}

// The servers a profile can be paired with: the download sources whose kind of server can pair,
// with what their entry in save-sync.json starts from (the type, the address, the platform).
struct PairSource {
    std::string name;
    std::string address;
    nlohmann::json settings;
    const Eden::Remote::Pairing* pairing = nullptr;
};
std::vector<PairSource> PairSources() {
    std::vector<PairSource> list;
    std::string text;
    if (!Eden::FilesystemAccess() || !Eden::Settings::ReadFile(Eden::ConfigFile("remote") + "/sources.json", text))
        return list;
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("sources") || !document["sources"].is_array()) return list;
    for (const nlohmann::json& source : document["sources"]) {
        if (!source.is_object()) continue;
        // sources.json is written by hand: a field that is no text is not there.
        const std::string type = TextOf(source, "type");
        const std::string url = TextOf(source, "url");
        const Eden::Remote::Backend* backend = Eden::Remote::FindBackend(type);
        if (url.empty() || backend == nullptr || backend->saves == nullptr || backend->pairing == nullptr) continue;
        PairSource item;
        item.name = TextOf(source, "name").empty() ? type : TextOf(source, "name");
        item.address = url;
        item.settings = {{"type", type}, {"url", url}};
        if (source.contains("platform")) item.settings["platform"] = source["platform"];
        item.pairing = backend->pairing;
        list.push_back(std::move(item));
    }
    return list;
}

std::string CurrentProfile(int user) {
    const auto who = Eden::Profiles::Resolve(user);
    return who.profiles.empty() ? std::string{} : who.profiles[static_cast<std::size_t>(who.current)].Key();
}

std::string ProfileName(const std::string& key) {
    for (const auto& profile : Eden::Profiles::Read())
        if (profile.Key() == key) return profile.name;
    return {};
}
} // namespace

Eden::Multiplayer::Snapshot EdenServices::room_status() { return rooms_.GetSnapshot(); }
Eden::Multiplayer::Connection EdenServices::room_connection() {
    auto result = Eden::LoadRoomConnection();
    if (result.nickname.empty()) result.nickname = "PlayerPS5";
    return result;
}
bool EdenServices::connect_room(const Eden::Multiplayer::Connection& connection) {
    if (!Eden::SaveRoomConnection(connection)) return false;
    return rooms_.Connect(connection);
}
void EdenServices::leave_room() { rooms_.Leave(); }
bool EdenServices::can_launch_game() {
    const auto phase = rooms_.GetSnapshot().phase;
    return phase == Eden::Multiplayer::Phase::Idle || phase == Eden::Multiplayer::Phase::Connected;
}
std::optional<std::string> EdenServices::room_text(int field, const std::string& initial,
                                                const std::atomic<bool>& stop) {
    if (field < 0 || field > 3) return std::nullopt;
    const char16_t* titles[]{u"Room address", u"UDP port", u"Nickname", u"Room password"};
    const std::size_t limits[]{253, 5, 20, 128};
    Eden::TextRequest request;
    request.title = titles[field];
    request.initial = Common::UTF8ToUTF16(initial);
    request.max_length = limits[field];
    request.numbers = field == 1;
    request.password = field == 3;
    const auto answer = Eden::AskSystemKeyboard(request, stop);
    if (answer.outcome == Eden::TextOutcome::unavailable)
        throw std::runtime_error("The system keyboard is unavailable");
    if (answer.outcome != Eden::TextOutcome::accepted) return std::nullopt;
    return Common::UTF16ToUTF8(answer.text);
}

EdenServices::EdenServices(std::string launch_error, Eden::Multiplayer::RoomClient& rooms)
    : rooms_(rooms), launch_error_(std::move(launch_error)) {
    (void)mkdir(Eden::ConfigDir().c_str(), 0777);
    // Who is playing (profiles.h): the profile this PS5 user chose last, else the one chosen last.
    (void)sceUserServiceInitialize(nullptr); // already done by the controller code: refused, harmless
    if (sceUserServiceGetForegroundUser(&user_) < 0) user_ = -1;
    const auto who = Eden::Profiles::Resolve(user_);
    if (!who.profiles.empty())
        Eden::Report("profile", (who.profiles[static_cast<std::size_t>(who.current)].name + " (" +
                                 std::to_string(who.current + 1) + " of " + std::to_string(who.profiles.size()) +
                                 ")").c_str());
    KeepSaveSyncFile(who.profiles);
    setup_ = eden_startup_error();
    Eden::Report("setup", setup_.empty() ? "Keys and firmware startup checks passed" : setup_.c_str());
    // The download sources' games and the download queue (remote/remote.h): only with filesystem access,
    // where the game files folder can be written.
    if (Eden::FilesystemAccess())
        Eden::Remote::Start({Eden::ConfigFile("remote"), Eden::CoversDir(), Eden::AssetsPath("roms"),
                             Eden::AssetsPath("updates"), Eden::AssetsPath(".remote-downloads")},
                            WriteCover, ListFolder);
    // The games played since their save data was last synced after them.
    if (Eden::FilesystemAccess()) {
        std::vector<SyncJob> jobs;
        for (const auto& [profile, file] : ReadPending()) jobs.push_back({profile, file});
        if (!jobs.empty()) StartSync(std::move(jobs), false);
    }
}

EdenServices::~EdenServices() {
    if (Eden::FilesystemAccess()) Eden::Remote::Stop();
    {
        const std::lock_guard lock(sync_lock_);
        sync_stop_ = true;
    }
    sync_changed_.notify_all();
    if (sync_thread_.joinable()) sync_thread_.join();
    pair_stop_ = true;
    if (pair_thread_.joinable()) pair_thread_.join();
}

pe::ui::Home EdenServices::home() {
    const std::lock_guard lock(bridge_);
    pe::ui::Home home;
    home.setup_ready = setup_.empty() && Eden::FilesystemAccess();
    if (!Eden::FilesystemAccess()) {
        // No data is kept anywhere else (storage_paths.h): say so, and start nothing.
        home.status = fill(tr("ProsperoEden could not get access to the console's storage, so it cannot read your settings, saves and games. Nothing was changed. Close ProsperoEden and open it again. (Code {0})"),
                           {std::to_string(Eden::FilesystemAccessStatus())});
    } else if (!home.setup_ready) {
        home.status = fill(tr("Setup required: {0} Open Settings, Game files to choose the folder that holds your "
                              "keys, firmware and roms folders (or add the files to {1}), then reopen ProsperoEden."),
                           {SetupMessage(setup_), Eden::AssetsDir()});
    } else if (launch_error_.starts_with(Eden::Crash::kNotice)) {
        // The previous run ended with a crash report (headless/crash_report.h).
        home.status = fill(tr("ProsperoEden stopped because of an error. A report was saved to {0}."),
                           {launch_error_.substr(Eden::Crash::kNotice.size())});
        home.launch_failed = true;
    } else if (!launch_error_.empty()) {
        home.status = fill(tr("Game could not start: {0} Details: {1}"),
                           {LaunchError(launch_error_), Eden::LogFile("stderr.log")});
        home.launch_failed = true;
    }

    home.last_file = Eden::LoadLastGame();
    // A game no longer in the game files folder is not offered: the most recent one that is
    // takes its place (or none).
    if (!home.last_file.empty() && !IsFile(Eden::AssetsPath("roms/" + home.last_file))) {
        home.last_file.clear();
        for (const auto& name : Eden::LoadRecentGames()) {
            if (IsFile(Eden::AssetsPath("roms/" + name))) {
                home.last_file = name;
                break;
            }
        }
    }
    const std::string last_path = Eden::AssetsPath("roms/" + home.last_file);
    home.last_exists = !home.last_file.empty() && IsFile(last_path);
    if (!home.last_file.empty()) {
        std::string title = GameTitle(home.last_file);
        std::string cover = CoverPath(home.last_file);
        bool has_cover = Eden::FileExists(cover);
        if (home.last_exists && home.setup_ready && !has_cover) {
            cover = EnsureCover(home.last_file, &title);
            has_cover = !cover.empty();
        }
        home.last_title = title;
        home.last_caption = home.last_exists ? tr("Last game opened") :
                                               tr("ROM missing from the game files folder");
        home.last_caption_warning = !home.last_exists;
        if (has_cover) home.last_cover = cover;
    }
    // The last game's update and DLC and the language it will use; when it does not offer the
    // chosen one, the caption says so.
    if (home.setup_ready && home.last_exists) {
        eden_scan_addons(Eden::AssetsPath("updates").c_str(), Eden::AssetsPath("keys").c_str());
        const uint64_t title_id = eden_game_title_id(last_path.c_str());
        const GameLanguage language = LanguageFor(last_path, title_id, Eden::PreferencesFor(title_id).language);
        home.last_title_id = title_id;
        home.last_addons = AddOnSummary(title_id);
        home.last_language = language.label;
        if (!language.note.empty()) {
            home.last_caption = language.note;
            home.last_caption_warning = true;
        }
    }

    auto history = Eden::LoadRecentGames();
    if (history.empty() && home.last_exists) {
        if (!Eden::SaveRecentGame(home.last_file))
            Eden::Report("history", "Could not seed recent games from last played game");
        history.push_back(home.last_file);
    }
    for (const auto& name : history) {
        if (!IsFile(Eden::AssetsPath("roms/" + name))) continue;
        home.recents.push_back({name, GameTitle(name), EnsureCover(name)});
    }
    const int installed = CountInstalledGames();
    home.system_status = fill(installed == 1 ? tr("{0} game installed") : tr("{0} games installed"),
                              {std::to_string(installed)}) +
        "  /  " + (home.setup_ready ? tr("Firmware ready") : tr("Setup required"));
    return home;
}

std::string EdenServices::clock() {
    const std::time_t now = std::time(nullptr);
    char label[32]{};
    if (const std::tm* local = std::localtime(&now))
        (void)std::strftime(label, sizeof(label), "%H:%M", local);
    return label;
}

unsigned EdenServices::controllers() { return radio_input_controllers(); }

std::string EdenServices::version() {
    // "01.000.040" reads as v1.000.040.
    const char* text = Eden::kAppVersion;
    while (text[0] == '0' && text[1] != '.' && text[1] != '\0') ++text;
    return std::string("v") + text;
}

std::vector<pe::ui::Game> EdenServices::games() {
    // The launcher reads the list beside its menu (pe/ui/library.cpp), so the metadata reader
    // is used by one thread at a time.
    const std::lock_guard lock(bridge_);
    std::vector<pe::ui::Game> games;
    (void)mkdir(Eden::ConfigDir().c_str(), 0777);
    (void)mkdir(Eden::CoversDir().c_str(), 0777);
    std::error_code directory_error;
    auto entries = Eden::ReadNativeDirectory(Eden::AssetsPath("roms"), directory_error);
    // Without a roms folder there are still the download sources' games.
    if (directory_error) entries.clear();
    eden_scan_addons(Eden::AssetsPath("updates").c_str(), Eden::AssetsPath("keys").c_str());
    // The download sources' games, each title once: one that is on the console can be downloaded
    // again, and is deleted from the console in its settings.
    const std::vector<Eden::Remote::Title> titles = Titles();
    const std::map<std::string, std::string> names = SourceNames();
    std::vector<bool> local(titles.size(), false);
    const auto sources_of = [&](const Eden::Remote::Title& title) {
        std::vector<std::string> sources;
        for (const Eden::Remote::Game& game : title.games)
            sources.push_back(names.contains(game.source) ? names.at(game.source) : game.source);
        return sources;
    };
    for (const auto& entry : entries) {
        const std::string file = entry.path().filename().string();
        const std::size_t dot = file.find_last_of('.');
        if (file == "." || file == ".." || dot == std::string::npos) continue;
        std::string format = file.substr(dot + 1);
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (format != "NSP" && format != "XCI") continue;
        const std::string path = Eden::AssetsPath("roms/" + file);
        struct stat info {};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        pe::ui::Game game;
        game.name = CleanTitle(file);
        game.format = format;
        game.size = SizeLabel(static_cast<double>(info.st_size));
        game.file = file;
        // A game whose data cannot be read is still listed, by its file name.
        try {
            char title[513]{};
            // The cover is replaced in one step: the menu may be loading the old one right now.
            const std::string cover = CoverPath(file);
            const std::string staged = cover + ".new";
            const int metadata = eden_extract_game_metadata(path.c_str(), Eden::AssetsPath("keys").c_str(),
                                                            staged.c_str(), title, sizeof(title));
            if (metadata & EDEN_METADATA_TITLE) {
                game.name = title;
                SaveTitle(file, game.name);
            }
            if ((metadata & EDEN_METADATA_COVER) && std::rename(staged.c_str(), cover.c_str()) == 0)
                game.cover = cover;
            else
                (void)std::remove(staged.c_str());
            game.title_id = eden_game_title_id(path.c_str());
            // The game's own language (Library > Game settings > Language) or Settings > Language.
            const GameLanguage language = LanguageFor(path, game.title_id, Eden::PreferencesFor(game.title_id).language);
            game.addons = AddOnSummary(game.title_id);
            game.addons_short = AddOnSummary(game.title_id, true);
            game.language = language.label;
            game.language_note = language.note;
        } catch (const std::exception& error) {
            Eden::Report("library", (file + ": " + error.what()).c_str());
        }
        games.push_back(std::move(game));
    }
    // Which games of the console a title is: by title ID, by name or by file (Remote::SameAsLocal).
    for (pe::ui::Game& game : games) {
        const std::string normal_name = Eden::Remote::NormalName(game.name);
        for (std::size_t i = 0; i < titles.size(); ++i)
            if (std::any_of(titles[i].games.begin(), titles[i].games.end(), [&](const Eden::Remote::Game& entry) {
                    return Eden::Remote::SameAsLocal(entry, game.title_id, normal_name, game.file);
                })) {
                game.sources = sources_of(titles[i]);
                game.key = titles[i].key;
                local[i] = true;
                break;
            }
    }
    // The titles that are not on the console (yet); one that is shows as above.
    for (std::size_t i = 0; i < titles.size(); ++i) {
        if (local[i]) continue;
        const Eden::Remote::Game& entry = titles[i].games.front();
        pe::ui::Game game;
        game.name = entry.name.empty() ? CleanTitle(entry.file) : entry.name;
        game.format = entry.file.size() > 4 ? entry.file.substr(entry.file.size() - 3) : std::string{};
        std::transform(game.format.begin(), game.format.end(), game.format.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        game.size = entry.size > 0 ? SizeLabel(static_cast<double>(entry.size)) : std::string{"-"};
        game.file = entry.file;
        // The first source's cover, or another one's when it has none.
        for (const Eden::Remote::Game& other : titles[i].games)
            if (game.cover.empty()) game.cover = other.cover;
        game.sources = sources_of(titles[i]);
        game.key = titles[i].key;
        game.remote = true;
        games.push_back(std::move(game));
    }
    std::sort(games.begin(), games.end(),
              [](const pe::ui::Game& a, const pe::ui::Game& b) { return a.name < b.name; });
    return games;
}

std::string EdenServices::game_path(const std::string& file) { return Eden::AssetsPath("roms/" + file); }

bool EdenServices::take_update(pe::ui::UpdateOffer* offer) {
    Eden::UpdateNotice::Offer found;
    if (!Eden::UpdateNotice::Take(&found)) return false;
    offer->version = found.version;
    offer->size = found.size;
    offer->installable = found.installable;
    offer->notes = found.notes;
    offer->notes_truncated = found.notes_truncated;
    return true;
}

bool EdenServices::start_update() { return Eden::UpdateNotice::Begin(); }

pe::ui::UpdateStatus EdenServices::update_status() {
    const Eden::UpdateNotice::Progress progress = Eden::UpdateNotice::Poll();
    pe::ui::UpdateStatus status;
    status.phase = static_cast<pe::ui::UpdatePhase>(progress.phase);  // the same order
    status.done = progress.done;
    status.total = progress.total;
    status.error = progress.error;
    return status;
}

void EdenServices::cancel_update() { Eden::UpdateNotice::Cancel(); }

bool EdenServices::apply_update() { return Eden::UpdateNotice::Apply(); }

void EdenServices::finish_update() { Eden::UpdateNotice::Finish(); }

std::vector<pe::ui::Profile> EdenServices::profiles() {
    const auto who = Eden::Profiles::Resolve(user_);
    std::vector<pe::ui::Profile> list;
    for (std::size_t index = 0; index < who.profiles.size(); ++index)
        list.push_back({who.profiles[index].name, static_cast<int>(index) == who.current});
    return list;
}

bool EdenServices::choose_profile(int index) {
    if (rooms_.GetSnapshot().phase != Eden::Multiplayer::Phase::Idle) return false;
    const auto who = Eden::Profiles::Resolve(user_);
    if (index < 0 || index >= static_cast<int>(who.profiles.size())) return false;
    const auto& profile = who.profiles[static_cast<std::size_t>(index)];
    if (!Eden::Profiles::Choose(profile, user_)) return false;
    Eden::Report("profile", ("Now playing: " + profile.name).c_str());
    return true;
}

int EdenServices::add_profile() {
    auto list = Eden::Profiles::Read();
    if (list.empty() || list.size() >= Eden::Profiles::kMax) return -1;
    // A signed-in PS5 user's name when no profile has it yet, else the next "Player" name.
    std::string name = Eden::Profiles::FreeName(list);
    for (const std::string& candidate : ProfileNames()) {
        if (std::none_of(list.begin(), list.end(), [&](const auto& p) { return p.name == candidate; })) {
            name = candidate;
            break;
        }
    }
    list.push_back(Eden::Profiles::Make(name));
    if (!Eden::Profiles::Write(list)) return -1;
    // It starts with the settings of whoever made it; from then on they are its own.
    (void)Eden::Profiles::Seed(list.back());
    KeepSaveSyncFile(list);
    Eden::Report("profile", ("Added: " + name).c_str());
    return static_cast<int>(list.size()) - 1;
}

bool EdenServices::rename_profile(int index, int step) {
    auto list = Eden::Profiles::Read();
    if (index < 0 || index >= static_cast<int>(list.size())) return false;
    auto& profile = list[static_cast<std::size_t>(index)];
    // The names nobody else has; this profile's own name keeps its place among them.
    std::vector<std::string> names;
    for (const std::string& name : ProfileNames()) {
        const bool taken = std::any_of(list.begin(), list.end(), [&](const auto& other) {
            return &other != &profile && other.name == name;
        });
        if (!taken) names.push_back(name);
    }
    if (names.empty()) return false;
    const auto at = std::find(names.begin(), names.end(), profile.name);
    const int count = static_cast<int>(names.size());
    const int from = at == names.end() ? (step > 0 ? -1 : 0) : static_cast<int>(at - names.begin());
    profile.name = names[static_cast<std::size_t>(((from + step) % count + count) % count)];
    if (!Eden::Profiles::Write(list)) return false;
    KeepSaveSyncFile(list);
    return true;
}

bool EdenServices::remove_profile(int index) {
    const auto who = Eden::Profiles::Resolve(user_);
    auto list = who.profiles;
    if (list.size() < 2 || index < 0 || index >= static_cast<int>(list.size()) || index == who.current) return false;
    const std::string name = list[static_cast<std::size_t>(index)].name;
    const auto gone = list[static_cast<std::size_t>(index)];
    list.erase(list.begin() + index);
    if (!Eden::Profiles::Write(list)) return false;
    (void)Eden::Profiles::Forget(gone);
    KeepSaveSyncFile(list);
    // Its save data stays where it is (nand/user/save/.../<ID>): removing a name destroys nothing.
    Eden::Report("profile", ("Removed from the list: " + name + " (its save data stays on the console)").c_str());
    return true;
}

bool EdenServices::game_exists(const std::string& file) {
    return Eden::ValidRomFilename(file) && IsFile(Eden::AssetsPath("roms/" + file));
}

bool EdenServices::docked(std::uint64_t title_id) { return Eden::LoadGameDocked(title_id); }

bool EdenServices::set_docked(std::uint64_t title_id, bool docked) {
    return Eden::SaveGameDocked(title_id, docked);
}

static_assert(pe::ui::kGameButtons == Eden::kGameButtons && pe::ui::kPadButtons == Eden::kPadButtons &&
              pe::ui::kDefaultMapping == Eden::kDefaultMapping, "the launcher's button mapping differs");
static_assert(std::tuple_size_v<decltype(pe::ui::GameSettings::performance)> == Eden::kPerformanceSwitches);

pe::ui::GameSettings EdenServices::game_settings(std::uint64_t title_id) {
    const Eden::GameSettings saved = Eden::LoadGameSettings(title_id);
    pe::ui::GameSettings result;
    result.renderer = saved.renderer;
    result.resolution = saved.resolution;
    result.filter = saved.upscaling_filter;
    result.refresh = saved.refresh;
    result.hud = saved.hud;
    result.frame_gen = saved.frame_gen;
    result.frame_gen_target = saved.frame_gen_target;
    result.frame_gen_multiplier = saved.frame_gen_multiplier;
    result.volume = saved.volume;
    result.mute = saved.mute;
    result.vibration = saved.vibration;
    result.language = saved.language;
    result.controller = saved.controller;
    result.own_mapping = saved.own_mapping;
    result.mapping = saved.mapping;
    result.performance = saved.performance;
    return result;
}

bool EdenServices::set_game_settings(std::uint64_t title_id, const pe::ui::GameSettings& settings) {
    Eden::GameSettings value;
    value.renderer = settings.renderer;
    value.resolution = settings.resolution;
    value.upscaling_filter = settings.filter;
    value.refresh = settings.refresh;
    value.hud = settings.hud;
    value.frame_gen = settings.frame_gen;
    value.frame_gen_target = settings.frame_gen_target;
    value.frame_gen_multiplier = settings.frame_gen_multiplier;
    value.volume = settings.volume;
    value.mute = settings.mute;
    value.vibration = settings.vibration;
    value.language = settings.language;
    value.controller = settings.controller;
    value.own_mapping = settings.own_mapping;
    value.mapping = settings.mapping;
    value.performance = settings.performance;
    const bool saved = Eden::SaveGameSettings(title_id, value);
    if (!saved) Eden::Report("settings", "Could not write game settings");
    return saved;
}

pe::ui::Preferences EdenServices::preferences() {
    const Eden::Preferences saved = Eden::LoadPreferences();
    pe::ui::Preferences result;
    result.hud = saved.hud;
    result.volume = saved.volume;
    result.mute = saved.mute;
    result.detailed_logging = saved.detailed_logging;
    result.immediate_logs = saved.immediate_logs;
    result.renderer = saved.backend == Eden::GraphicsBackend::OpenGL ? 0 : 1;
    result.resolution = saved.resolution;
    result.filter = saved.upscaling_filter;
    result.refresh = saved.refresh;
    result.output = saved.output;
    result.frame_gen = saved.frame_gen;
    result.frame_gen_target = saved.frame_gen_target;
    result.frame_gen_multiplier = saved.frame_gen_multiplier;
    result.vibration = saved.vibration;
    result.language = saved.language;
    result.menu_volume = saved.menu_volume;
    result.large_text = saved.large_text;
    result.high_contrast = saved.high_contrast;
    result.reduce_motion = saved.reduce_motion;
    // Settings > Performance: the general values (settings_store.h).
    const Eden::PerformanceSettings speed = Eden::LoadPerformance(0);
    result.block_list = speed.block_list;
    result.async_shaders = speed.async_shaders;
    result.fast_gpu = speed.fast_gpu;
    result.unsafe_cpu = speed.unsafe_cpu;
    result.unsafe_dma = speed.unsafe_dma;
    result.reactive_flushing = speed.reactive_flushing;
    result.skip_invalidation = speed.skip_invalidation;
    result.mapping = saved.mapping;
    return result;
}

bool EdenServices::set_preferences(const pe::ui::Preferences& preferences) {
    Eden::Preferences value;
    value.hud = preferences.hud;
    value.volume = preferences.volume;
    value.mute = preferences.mute;
    value.detailed_logging = preferences.detailed_logging;
    value.immediate_logs = preferences.immediate_logs;
    value.backend = preferences.renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan;
    value.resolution = preferences.resolution;
    value.upscaling_filter = preferences.filter;
    value.refresh = preferences.refresh;
    value.output = preferences.output;
    value.frame_gen = preferences.frame_gen;
    value.frame_gen_target = preferences.frame_gen_target;
    value.frame_gen_multiplier = preferences.frame_gen_multiplier;
    value.vibration = preferences.vibration;
    value.language = preferences.language;
    value.menu_volume = preferences.menu_volume;
    value.large_text = preferences.large_text;
    value.high_contrast = preferences.high_contrast;
    value.reduce_motion = preferences.reduce_motion;
    value.mapping = preferences.mapping;
    Eden::PerformanceSettings speed = Eden::LoadPerformance(0);
    speed.block_list = preferences.block_list;
    speed.async_shaders = preferences.async_shaders;
    speed.fast_gpu = preferences.fast_gpu;
    speed.unsafe_cpu = preferences.unsafe_cpu;
    speed.unsafe_dma = preferences.unsafe_dma;
    speed.reactive_flushing = preferences.reactive_flushing;
    speed.skip_invalidation = preferences.skip_invalidation;
    const bool saved = Eden::SavePreferences(value) && Eden::SavePerformance(0, speed);
    if (!saved) Eden::Report("settings", "Could not write preferences");
    return saved;
}

const std::vector<std::string>& EdenServices::resolution_labels() {
    static const std::vector<std::string> labels = Labels(Eden::kResolutionLabels);
    return labels;
}

const std::vector<std::string>& EdenServices::resolution_keys() {
    static const std::vector<std::string> labels = Labels(Eden::kResolutionKeys);
    return labels;
}

// The library is the user's own copy from Lossless Scaling: <user folder>/lossless/Lossless.dll.
int EdenServices::frame_gen_state() {
#ifdef EDEN_PS5_FRAMEGEN
    return Eden::FileExists(Eden::UserDir() + "/lossless/Lossless.dll") ? 2 : 1;
#else
    return 0;
#endif
}

const std::vector<std::string>& EdenServices::filter_labels() {
    static const std::vector<std::string> labels = Labels(Eden::kUpscalingFilterLabels);
    return labels;
}

const std::vector<std::string>& EdenServices::language_labels() {
    static const std::vector<std::string> labels = Labels(Eden::kLanguageLabels);
    return labels;
}

std::string EdenServices::language_region(int language) {
    static constexpr const char* kRegions[] = {TR("Japan"), TR("USA"), TR("Europe"), TR("Australia"), TR("China"),
                                               TR("Korea"), TR("Taiwan")};
    if (language < 0 || language >= int(std::size(Eden::kLanguageRegions))) return {};
    return tr(kRegions[Eden::kLanguageRegions[language]]);
}

std::string EdenServices::setup_details() {
    return setup_.empty() ?
        tr("Keys and firmware: startup checks passed. Game-specific compatibility is checked at launch.") :
        SetupMessage(setup_);
}

bool EdenServices::folders(const std::string& directory, std::vector<std::string>* names) {
    bool ok = false;
    *names = ListEntries(directory, true, ok);
    return ok;
}

pe::ui::FolderInfo EdenServices::folder_info(const std::string& directory) {
    pe::ui::FolderInfo info;
    info.keys = Eden::FileExists(JoinPath(directory, "keys/prod.keys"));
    info.firmware = CountFiles(JoinPath(directory, "firmware"), {".nca"});
    info.games = CountFiles(JoinPath(directory, "roms"), {".nsp", ".xci"});
    return info;
}

std::string EdenServices::files_folder() { return Eden::AssetsDir(); }
std::string EdenServices::saved_files_folder() { return Eden::LoadSavedAssetsDir(); }
std::string EdenServices::default_files_folder() { return Eden::kDefaultAssetsDir; }

bool EdenServices::set_files_folder(const std::string& directory) {
    const bool saved = Eden::SaveAssetsDir(directory);
    if (!saved) Eden::Report("settings", "Could not write the game files folder");
    return saved;
}

int EdenServices::filesystem_access() { return Eden::FilesystemAccessStatus(); }

#ifdef EDEN_SAVE_IMPORT
// Save transfer (ryujinx_saves.h): a save comes in from save-import/<title ID>/ or a Ryujinx data
// folder in ryujinx/, and goes out to save-export/, all next to roms/.
bool EdenServices::save_transfer_available() { return true; }

pe::ui::SaveSource EdenServices::save_import_source(std::uint64_t title_id) {
    switch (eden_save_import_source(title_id)) {
    case EDEN_SAVE_FOLDER: return pe::ui::SaveSource::folder;
    case EDEN_SAVE_RYUJINX: return pe::ui::SaveSource::ryujinx;
    default: return pe::ui::SaveSource::none;
    }
}

bool EdenServices::save_import(std::uint64_t title_id, std::string* message) {
    char backup[256]{};
    switch (eden_save_import(title_id, backup, sizeof(backup))) {
    case EDEN_SAVE_DONE:
        *message = backup[0] ? tr("Imported. The save it replaced was backed up.") : tr("Imported.");
        return true;
    case EDEN_SAVE_NOTHING: {
        char title[17]{};
        std::snprintf(title, sizeof(title), "%016llX", static_cast<unsigned long long>(title_id));
        *message = fill(tr("To import, copy a Ryujinx folder to ryujinx/ or a save to save-import/{0}/, next to roms/."),
                        {title});
        return false;
    }
    case EDEN_SAVE_NO_USER:
        *message = tr("Start any game once before importing a save.");
        return false;
    default:
        *message = tr("Import failed. The current save is unchanged.");
        return false;
    }
}

bool EdenServices::save_export(std::uint64_t title_id, std::string* message) {
    char folder[256]{};
    switch (eden_save_export(title_id, folder, sizeof(folder))) {
    case EDEN_SAVE_DONE: {
        // The end of the path tells it apart: save-export/<title ID>-<date>-<time>.
        const std::string path = folder;
        const std::size_t name = path.rfind("save-export/");
        *message = fill(tr("Exported to {0}."), {name == std::string::npos ? path : path.substr(name)});
        return true;
    }
    case EDEN_SAVE_NOTHING:
        *message = tr("This game has no save to export yet.");
        return false;
    default:
        *message = tr("Export failed. Check that the game files folder can be written.");
        return false;
    }
}
#else
bool EdenServices::save_transfer_available() { return false; }
pe::ui::SaveSource EdenServices::save_import_source(std::uint64_t) { return pe::ui::SaveSource::none; }
bool EdenServices::save_import(std::uint64_t, std::string*) { return false; }
bool EdenServices::save_export(std::uint64_t, std::string*) { return false; }
#endif

// Mods (headless/mods.h): what the game files folder's mods/<title ID>/ holds for a game, which
// of them are switched off and which cheats are chosen (settings_store.h).
std::vector<pe::ui::Mod> EdenServices::mods(std::uint64_t title_id) {
    std::vector<pe::ui::Mod> result;
    const auto off = Eden::LoadDisabledMods(title_id);
    const auto chosen = Eden::LoadChosenCheats(title_id);
    for (const Eden::Mods::Mod& mod : Eden::Mods::List(Eden::AssetsPath("mods"), title_id)) {
        std::string kind;
        const auto add = [&kind](const char* text) {
            if (!kind.empty()) kind += ", ";
            kind += tr(text);
        };
        if (mod.kinds & Eden::Mods::kCode) add(TR("Patch"));
        if (mod.kinds & Eden::Mods::kFiles) add(TR("Files"));
        if (mod.kinds & Eden::Mods::kCheats) add(TR("Cheats"));
        result.push_back({mod.name, kind, std::find(off.begin(), off.end(), mod.name) == off.end(), {}});
        for (const auto& cheat : mod.cheats)
            result.back().cheats.push_back(
                {cheat.name, std::find(chosen.begin(), chosen.end(), cheat.id) != chosen.end()});
    }
    return result;
}

bool EdenServices::set_cheat_enabled(std::uint64_t title_id, const std::string& name, const std::string& cheat,
                                     bool enabled) {
    bool saved = false;
    for (const Eden::Mods::Mod& mod : Eden::Mods::List(Eden::AssetsPath("mods"), title_id))
        if (mod.name == name)
            saved = Eden::SaveChosenCheats(
                title_id, Eden::Mods::ChooseCheat(mod, cheat, enabled, Eden::LoadChosenCheats(title_id)));
    if (!saved) Eden::Report("settings", "Could not write the game's cheats");
    return saved;
}

bool EdenServices::set_mod_enabled(std::uint64_t title_id, const std::string& name, bool enabled) {
    const bool saved = Eden::SaveModEnabled(title_id, name, enabled);
    if (!saved) Eden::Report("settings", "Could not write the game's mods");
    return saved;
}

bool EdenServices::mods_enabled(std::uint64_t title_id) { return Eden::LoadModsEnabled(title_id); }

bool EdenServices::set_mods_enabled(std::uint64_t title_id, bool enabled) {
    const bool saved = Eden::SaveModsEnabled(title_id, enabled);
    if (!saved) Eden::Report("settings", "Could not write the game's mods switch");
    return saved;
}

std::string EdenServices::mods_folder(std::uint64_t title_id) {
    return "mods/" + Eden::Mods::TitleName(title_id) + "/";
}

bool EdenServices::make_mods_folder(std::uint64_t title_id) {
    const std::string root = Eden::AssetsPath("mods");
    if (!Eden::Mods::TitleFolder(root, title_id).empty()) return true;
    (void)mkdir(root.c_str(), 0777);
    return mkdir(Eden::Mods::TitleFolderToCreate(root, title_id).c_str(), 0777) == 0;
}

pe::ui::Sources EdenServices::sources() {
    pe::ui::Sources sources;
    sources.setup_file = Eden::ConfigFile("remote/sources.json");
    if (!Eden::FilesystemAccess()) {
        sources.error = "Download sources need filesystem access";
        return sources;
    }
    const Eden::Remote::Status status = Eden::Remote::Current();
    sources.configured = status.configured;
    sources.error = status.error;
    sources.generation = status.generation;
    sources.ftp_port = status.ftp_port;
    for (const Eden::Remote::SourceStatus& source : status.sources)
        sources.list.push_back({source.name, source.address, source.refreshing, source.online, source.error,
                                static_cast<int>(source.games)});
    return sources;
}

void EdenServices::refresh_sources() {
    if (Eden::FilesystemAccess()) Eden::Remote::Refresh();
}

std::vector<Eden::Remote::Title> EdenServices::Titles() {
    if (!Eden::FilesystemAccess()) return {};
    const std::lock_guard lock(titles_lock_);
    RefreshTitles();
    return titles_;
}

void EdenServices::RefreshTitles() {
    const std::uint64_t generation = Eden::Remote::Current().generation;
    if (generation != titles_generation_) {
        titles_ = Eden::Remote::Titles();
        titles_generation_ = generation;
    }
}

std::string EdenServices::TitleKeyOf(const std::string& source, const std::string& id) {
    const std::lock_guard lock(titles_lock_);
    RefreshTitles();
    for (const Eden::Remote::Title& title : titles_)
        for (const Eden::Remote::Game& game : title.games)
            if (game.source == source && game.id == id) return title.key;
    return {};
}

bool EdenServices::download(const pe::ui::Game& game, int source, bool first) {
    // The title on its sources, in the order of Game::sources.
    for (const Eden::Remote::Title& title : Titles())
        if (title.key == game.key && source >= 0 && source < static_cast<int>(title.games.size()))
            return Eden::Remote::Enqueue(title.games[static_cast<std::size_t>(source)].source,
                                         title.games[static_cast<std::size_t>(source)].id, first);
    return false;
}

bool EdenServices::cancel_download(const std::string& key) {
    if (!Eden::FilesystemAccess()) return false;
    for (const Eden::Remote::Title& title : Titles())
        if (title.key == key)
            for (const Eden::Remote::Game& game : title.games)
                if (Eden::Remote::Cancel(game.source, game.id)) return true;
    return false;
}

std::vector<pe::ui::Download> EdenServices::downloads() {
    std::vector<pe::ui::Download> list;
    if (!Eden::FilesystemAccess()) return list;
    // Asked four times a second: nothing more is read while the queue is empty.
    const std::vector<Eden::Remote::Download> queue = Eden::Remote::Downloads();
    if (queue.empty()) return list;
    const std::map<std::string, std::string> names = SourceNames();
    for (const Eden::Remote::Download& entry : queue) {
        pe::ui::Download download;
        static_assert(static_cast<int>(pe::ui::DownloadState::verifying) == static_cast<int>(Eden::Remote::State::verifying) &&
                      static_cast<int>(pe::ui::DownloadState::failed) == static_cast<int>(Eden::Remote::State::failed));
        download.state = static_cast<pe::ui::DownloadState>(entry.state); // the same order
        download.done = entry.done;
        download.total = entry.total;
        download.rate = entry.rate;
        download.error = entry.error;
        download.source = names.contains(entry.source) ? names.at(entry.source) : entry.source;
        Eden::Remote::Game game;
        if (Eden::Remote::Find(entry.source, entry.id, &game)) {
            download.name = game.name.empty() ? CleanTitle(game.file) : game.name;
            download.file = game.file;
            download.cover = game.cover;
        } else {
            download.name = entry.id;
        }
        // Its title: the game as the Library lists it.
        download.key = TitleKeyOf(entry.source, entry.id);
        list.push_back(std::move(download));
    }
    return list;
}

bool EdenServices::delete_game(const pe::ui::Game& game, std::string* message) {
    if (!Eden::FilesystemAccess() || game.sources.empty() || game.remote || !Eden::ValidRomFilename(game.file))
        return false;
    const std::lock_guard lock(bridge_);
    const std::string rom = Eden::AssetsPath("roms/" + game.file);
    // The game, what came with it from its sources, and every update and DLC of it in updates/
    // (also one put there by hand), found by the title IDs the files hold.
    std::vector<std::string> files{rom};
    for (const Eden::Remote::Title& title : Titles())
        if (title.key == game.key)
            for (const Eden::Remote::Game& entry : title.games)
                for (std::string& path : Eden::Remote::Files(entry.source, entry.id)) files.push_back(std::move(path));
    const std::uint64_t title_id = game.title_id ? game.title_id : eden_game_title_id(rom.c_str());
    (void)eden_game_addon_files(title_id, Eden::AssetsPath("updates").c_str(), Eden::AssetsPath("keys").c_str(),
                                [](void* user, const char* path) {
                                    static_cast<std::vector<std::string>*>(user)->emplace_back(path);
                                },
                                &files);
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    int deleted = 0;
    std::uint64_t bytes = 0;
    std::string kept;
    for (const std::string& path : files) {
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;
        if (std::remove(path.c_str()) == 0) {
            ++deleted;
            bytes += static_cast<std::uint64_t>(info.st_size);
        } else if (kept.empty()) {
            kept = path;
        }
    }
    // The Library reads its list again: the game is on its sources only now.
    Eden::Remote::Changed();
    if (!kept.empty()) {
        *message = fill(tr("Could not delete {0}."), {kept});
        return false;
    }
    *message = fill(deleted == 1 ? tr("Deleted 1 file ({0}). Save data and settings are kept.") :
                                   tr("Deleted {1} files ({0}). Save data and settings are kept."),
                    {SizeLabel(static_cast<double>(bytes)), std::to_string(deleted)});
    return true;
}

bool EdenServices::load_image(const std::string& path, pe::gfx::Image* image) {
    // Covers have full paths; the launcher's own art is named from its ui folder.
    return pe::gfx::load_tga(!path.empty() && path[0] == '/' ? path : Eden::AppFile("ui/" + path), image);
}

// ---- save sync ----

bool EdenServices::save_sync_wanted(const std::string& file) {
    SyncSetup setup;
    return !file.empty() && SyncSetupOf(CurrentProfile(user_), &setup) && setup.automatic;
}

void EdenServices::start_save_sync(const std::string& file, bool before) {
    if (before) {
        StartSync({{CurrentProfile(user_), file}}, true);
        return;
    }
    std::vector<SyncJob> jobs;
    for (const auto& [profile, pending] : ReadPending())
        if (pending == file) jobs.push_back({profile, pending});
    StartSync(std::move(jobs), false);
}

void EdenServices::StartSync(std::vector<SyncJob> jobs, bool before) {
    if (sync_thread_.joinable()) {
        {
            const std::lock_guard lock(sync_lock_);
            if (sync_.stage == pe::ui::SaveSyncStage::working || sync_.stage == pe::ui::SaveSyncStage::conflict) return;
        }
        sync_thread_.join();
    }
    // What each needs, read here; its title ID on the thread: Eden's reader takes one caller at a
    // time, and the menu's scan of the games can hold it a while.
    struct Work {
        SyncJob job;
        Eden::Remote::SyncGame game;
        SyncSetup setup;
        bool syncs = false;
        std::string profile_name;
    };
    std::vector<Work> work;
    for (SyncJob& job : jobs) {
        Work item;
        item.job = std::move(job);
        item.game.file = item.job.file;
        item.game.name = GameTitle(item.job.file);
        // Turned off (save-sync.json's "auto"): neither before a game nor what waits from after one.
        item.syncs = SyncSetupOf(item.job.profile, &item.setup) && item.setup.automatic;
        item.profile_name = ProfileName(item.job.profile);
        work.push_back(std::move(item));
    }
    if (work.empty()) return;
    {
        const std::lock_guard lock(sync_lock_);
        sync_ = {};
        sync_.stage = pe::ui::SaveSyncStage::working;
        sync_.before = before;
        sync_.game = work.front().game.name;
        sync_.profile = work.front().profile_name;
        sync_choice_.reset();
    }
    sync_stop_ = false;
    sync_thread_ = std::thread([this, work = std::move(work), before] {
        pe::ui::SaveSync last;
        last.stage = pe::ui::SaveSyncStage::done;
        last.before = before;
        for (Work item : work) {
            if (sync_stop_) break;
            if (const std::string path = Eden::AssetsPath("roms/" + item.job.file); item.syncs && Eden::FileExists(path)) {
                const std::lock_guard lock(bridge_);
                item.game.title_id = eden_game_title_id(path.c_str());
            }
            {
                const std::lock_guard lock(sync_lock_);
                sync_.game = item.game.name;
                sync_.profile = item.profile_name;
                sync_.stage = pe::ui::SaveSyncStage::working;
            }
            Eden::Remote::SyncResult result;
            if (!item.syncs || item.game.title_id == 0) {
                // The profile no longer syncs, or the game is gone: nothing left to do for it.
                result.outcome = Eden::Remote::SyncOutcome::same;
            } else {
                const std::string& profile = item.job.profile;
                const std::string title = Eden::Remote::TitleFolder(item.game.title_id);
                Eden::Remote::SyncPlaces places;
                places.save = Eden::UserDir() + "/nand/user/save/0000000000000000/" + profile + "/" + title;
                places.store = SyncFolder() + "/" + profile;
                places.work = places.store + "/work";
                places.backups = std::string{Eden::kDataDir} + "/backup/save-sync/" + profile;
                places.lister = ListSaveFolder;
                result = Eden::Remote::SyncSaveData(
                    item.setup.type, item.setup.settings, item.game, places,
                    [this](const Eden::Remote::SavePlan& plan, const Eden::Remote::LocalSave& local) {
                        std::unique_lock lock(sync_lock_);
                        sync_.stage = pe::ui::SaveSyncStage::conflict;
                        sync_.console_time = local.updated;
                        sync_.server_time = plan.remote_updated;
                        sync_.server_device = plan.remote_device;
                        sync_choice_.reset();
                        sync_changed_.wait(lock, [this] { return sync_choice_.has_value() || sync_stop_.load(); });
                        sync_.stage = pe::ui::SaveSyncStage::working;
                        const pe::ui::SaveChoice choice = sync_choice_.value_or(pe::ui::SaveChoice::neither);
                        return choice == pe::ui::SaveChoice::console ? Eden::Remote::SyncChoice::console :
                               choice == pe::ui::SaveChoice::server  ? Eden::Remote::SyncChoice::server :
                                                                       Eden::Remote::SyncChoice::neither;
                    },
                    [this] { return sync_stop_.load(); });
                const std::string url = TextOf(item.setup.settings, "url");
                last.server = result.user.empty() ? url : result.user + " @ " + url;
                Eden::Report("save sync", (item.game.name + " (" + item.profile_name + "): " +
                                           (result.outcome == Eden::Remote::SyncOutcome::failed ? result.message :
                                            result.outcome == Eden::Remote::SyncOutcome::uploaded ? "uploaded" :
                                            result.outcome == Eden::Remote::SyncOutcome::downloaded ? "downloaded" :
                                            result.outcome == Eden::Remote::SyncOutcome::kept ? "conflict left as it is" :
                                            result.outcome == Eden::Remote::SyncOutcome::no_game ? "the server does not have the game" :
                                                                                                    "already in step"))
                                              .c_str());
            }
            // After a game: done with it unless it failed (then it is tried again on the next start),
            // or the menu closed in the middle (a conflict not answered is not an answer).
            if (!before && result.outcome != Eden::Remote::SyncOutcome::failed && !sync_stop_) {
                auto pending = ReadPending();
                std::erase(pending, std::pair{item.job.profile, item.job.file});
                WritePending(pending);
            }
            last.game = item.game.name;
            last.profile = item.profile_name;
            if (result.outcome == Eden::Remote::SyncOutcome::failed) {
                last.stage = pe::ui::SaveSyncStage::failed;
                last.error = result.message;
                last.too_old = result.too_old;
                last.server_version = result.version;
                last.needed_version = result.needed;
            } else if (last.stage != pe::ui::SaveSyncStage::failed) {
                last.outcome = result.outcome == Eden::Remote::SyncOutcome::uploaded   ? pe::ui::SaveSyncOutcome::uploaded :
                               result.outcome == Eden::Remote::SyncOutcome::downloaded ? pe::ui::SaveSyncOutcome::downloaded :
                               result.outcome == Eden::Remote::SyncOutcome::kept       ? pe::ui::SaveSyncOutcome::kept :
                               result.outcome == Eden::Remote::SyncOutcome::no_game    ? pe::ui::SaveSyncOutcome::no_game :
                                                                                         pe::ui::SaveSyncOutcome::same;
            }
        }
        const std::lock_guard lock(sync_lock_);
        sync_ = last;
    });
}

pe::ui::SaveSync EdenServices::save_sync() {
    const std::lock_guard lock(sync_lock_);
    return sync_;
}

void EdenServices::choose_save_data(pe::ui::SaveChoice choice) {
    {
        const std::lock_guard lock(sync_lock_);
        sync_choice_ = choice;
    }
    sync_changed_.notify_all();
}

void EdenServices::end_save_sync() {
    const std::lock_guard lock(sync_lock_);
    if (sync_.stage == pe::ui::SaveSyncStage::done || sync_.stage == pe::ui::SaveSyncStage::failed) sync_ = {};
}

void EdenServices::stop_save_sync() {
    {
        const std::lock_guard lock(sync_lock_);
        sync_stop_ = true;
    }
    sync_changed_.notify_all();
}

void EdenServices::will_play(const std::string& file) {
    if (!save_sync_wanted(file)) return;
    auto pending = ReadPending();
    const std::pair<std::string, std::string> job{CurrentProfile(user_), file};
    if (std::find(pending.begin(), pending.end(), job) == pending.end()) pending.push_back(job);
    WritePending(pending);
}

// ---- Settings > Save sync ----

pe::ui::SaveSyncSetup EdenServices::save_sync_setup() {
    pe::ui::SaveSyncSetup setup;
    setup.file = Eden::SaveSync::File();
    if (!Eden::FilesystemAccess()) return setup;
    const auto who = Eden::Profiles::Resolve(user_);
    const Eden::SaveSync::Config config = Eden::SaveSync::Read();
    if (!config.readable) setup.error = config.error;
    setup.automatic = config.automatic;
    for (std::size_t index = 0; index < who.profiles.size(); ++index) {
        const auto& profile = who.profiles[index];
        pe::ui::SaveSyncProfile item;
        item.name = profile.name;
        item.current = static_cast<int>(index) == who.current;
        const Eden::SaveSync::Entry* entry = config.readable ? Eden::SaveSync::Find(config, profile.Key()) : nullptr;
        item.linked = entry != nullptr && !entry->type.empty();
        if (item.linked) {
            const std::string url = TextOf(entry->settings, "url");
            const std::string user = TextOf(entry->settings, "__server_user");
            item.server = user.empty() ? url : user + " @ " + url;
            const Eden::Remote::Backend* backend = Eden::Remote::FindBackend(entry->type);
            if (backend == nullptr || backend->saves == nullptr)
                item.note = "Unknown type \"" + entry->type + "\" in save-sync.json";
        }
        setup.profiles.push_back(std::move(item));
    }
    return setup;
}

std::vector<pe::ui::PairServer> EdenServices::pair_servers() {
    std::vector<pe::ui::PairServer> list;
    for (const PairSource& source : PairSources()) list.push_back({source.name, source.address});
    return list;
}

bool EdenServices::start_pairing(int profile_index, int server_index) {
    {
        const std::lock_guard lock(pair_lock_);
        if (pair_.stage == pe::ui::PairingStage::asking || pair_.stage == pe::ui::PairingStage::waiting) return false;
    }
    if (pair_thread_.joinable()) pair_thread_.join();
    const auto profiles = Eden::Profiles::Read();
    const std::vector<PairSource> sources = PairSources();
    if (profile_index < 0 || profile_index >= static_cast<int>(profiles.size()) || server_index < 0 ||
        server_index >= static_cast<int>(sources.size()))
        return false;
    const auto profile = profiles[static_cast<std::size_t>(profile_index)];
    const PairSource source = sources[static_cast<std::size_t>(server_index)];
    {
        const std::lock_guard lock(pair_lock_);
        pair_ = {};
        pair_.stage = pe::ui::PairingStage::asking;
        pair_.profile = profile.name;
        pair_.server = source.name;
    }
    pair_stop_ = false;
    KeepSaveSyncFile(profiles); // the profile's entry is there to be filled in
    pair_thread_ = std::thread([this, key = profile.Key(), name = profile.name, source] {
        const std::string folder = SyncFolder() + "/" + key;
        const auto fail = [&](std::string error, bool denied = false, bool expired = false) {
            const std::lock_guard lock(pair_lock_);
            pair_.stage = pe::ui::PairingStage::failed;
            pair_.error = std::move(error);
            pair_.denied = denied;
            pair_.expired = expired;
            Eden::Report("save sync", ("pairing " + name + " with " + source.name + ": " +
                                       (denied ? "denied" : expired ? "expired" : pair_.error)).c_str());
        };
        Eden::Remote::PairingStart start;
        std::string error;
        const auto stopped = [this] { return pair_stop_.load(); };
        if (!source.pairing->start(source.settings, folder, &start, &error, stopped)) {
            if (pair_stop_) {
                const std::lock_guard lock(pair_lock_);
                pair_ = {};
                return;
            }
            return fail(error);
        }
        if (pair_stop_) {
            const std::lock_guard lock(pair_lock_);
            pair_ = {};
            return;
        }
        {
            const std::lock_guard lock(pair_lock_);
            pair_.stage = pe::ui::PairingStage::waiting;
            pair_.code = start.user_code;
            pair_.address = start.address;
            pair_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(start.expires_in);
        }
        int interval = start.interval;
        for (;;) {
            // Waits the interval, looking now and then whether it is cancelled.
            for (int tenth = 0; tenth < interval * 10; ++tenth) {
                if (pair_stop_) {
                    const std::lock_guard lock(pair_lock_);
                    pair_ = {};
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (std::chrono::steady_clock::now() > pair_until_) return fail({}, false, true);
            start.interval = interval; // slow_down adds to the interval used so far
            const Eden::Remote::PairingResult result = source.pairing->poll(source.settings, folder, start, stopped);
            // Cancelled while it asked: not linked, whatever the answer.
            if (pair_stop_) {
                const std::lock_guard lock(pair_lock_);
                pair_ = {};
                return;
            }
            switch (result.state) {
            case Eden::Remote::PairingState::pending:
                interval = std::max(1, result.interval);
                continue;
            case Eden::Remote::PairingState::denied:
                return fail({}, true);
            case Eden::Remote::PairingState::expired:
                return fail({}, false, true);
            case Eden::Remote::PairingState::failed:
                return fail(result.error);
            case Eden::Remote::PairingState::approved: {
                Eden::SaveSync::Json fields = Eden::SaveSync::Json::parse(result.entry.dump());
                if (!result.user.empty()) fields["__server_user"] = result.user;
                if (!Eden::SaveSync::SetEntry(key, fields)) return fail("save-sync.json cannot be written");
                const std::lock_guard lock(pair_lock_);
                pair_.stage = pe::ui::PairingStage::done;
                pair_.user = result.user;
                Eden::Report("save sync", ("paired " + name + " with " + source.name + " as " + result.user).c_str());
                return;
            }
            }
        }
    });
    return true;
}

pe::ui::PairingStatus EdenServices::pairing() {
    const std::lock_guard lock(pair_lock_);
    pe::ui::PairingStatus status = pair_;
    if (status.stage == pe::ui::PairingStage::waiting)
        status.seconds_left = static_cast<int>(std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::seconds>(pair_until_ - std::chrono::steady_clock::now()).count()));
    return status;
}

void EdenServices::cancel_pairing() {
    pair_stop_ = true;
    const std::lock_guard lock(pair_lock_);
    if (pair_.stage == pe::ui::PairingStage::done || pair_.stage == pe::ui::PairingStage::failed) pair_ = {};
}

bool EdenServices::unlink_profile(int profile_index) {
    const auto profiles = Eden::Profiles::Read();
    if (profile_index < 0 || profile_index >= static_cast<int>(profiles.size())) return false;
    const auto& profile = profiles[static_cast<std::size_t>(profile_index)];
    const bool unlinked = Eden::SaveSync::SetEntry(profile.Key(), Eden::SaveSync::Json::object());
    if (unlinked) Eden::Report("save sync", ("unlinked " + profile.name).c_str());
    return unlinked;
}
