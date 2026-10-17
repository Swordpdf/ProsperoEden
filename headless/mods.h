// SPDX-License-Identifier: GPL-3.0-or-later
// Mods: what the player put beside a game, in the layout Eden's patch manager reads and mod
// archives come in. In the game's own folder in roms/ (issue 168):
//
//   roms/<game>/mods/<mod name>/exefs/      code patches (*.pchtxt, *.ips) or whole replacement
//                                           files (main, subsdk0, ...)
//                              /romfs/      game files that replace the game's own
//                              /romfs_ext/  (also romfslite/)
//   roms/<game>/cheats/<build ID>.txt       cheats, one file per version of the game
//
// and in the game files folder, for any game: mods/<title ID, 16 hex digits>/<mod name>/ (the same
// layout, a mod's own cheats/<build ID>.txt and loose cheat_<anything>.txt files too) and
// cheats/<title ID>/<build ID>.txt. A mod of the same name in both is one mod, its files from the
// game's folder first; the cheat files of both cheats/ folders make one mod, kCheatsFolder (the game's folder's
// file first when both have one for a version). The derived bis_factory.cpp shows Eden the same
// (game_folder_vfs.h).
//
// Eden applies every mod of the running game, in the order of their names, except those the
// launcher switched off (settings_store.h keeps their names per game; main.cpp hands them to
// Eden). A mod with one cheat runs it whenever the mod is on. A mod that lists several, as cheat
// collections do, has each of them chosen on its own (patch_library.h reads their names; the
// chosen ones are kept per game too, and cheats.h switches the others off in Eden's list): none
// of them runs until it is chosen. This header only looks: the launcher lists a game's mods with it and main.cpp names
// them in the log. The title's folder is found whatever the case of its letters (the derived
// bis_factory.cpp does the same for Eden), and is never created behind the player's back.
#pragma once
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cheats.h"
#include "patch_library.h"
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

namespace Eden::Mods {
namespace fs = std::filesystem;

// The mod the cheats/ folders make (roms/<game>/cheats/ and cheats/<title ID>/).
inline constexpr std::string_view kCheatsFolder = "Cheats folder";

// Where a game's mods are: the game files folder's mods/ and cheats/, and the game's own folder in
// roms/ (empty for a game directly in roms/).
struct Places {
    std::string mods;
    std::string cheats;
    std::string game_folder;
};

// The game folder of the game being started, for Eden's patch manager (game_folder_vfs.h); empty
// for a game directly in roms/. main.cpp sets it before the game boots.
inline std::string& SessionGameFolder() {
    static std::string folder;
    return folder;
}

enum : unsigned {
    kCode = 1,    // exefs: patches or replacement code
    kFiles = 2,   // romfs: replacement game files
    kCheats = 4,  // cheats
};

struct Mod {
    std::string name;  // the folder's (or cheat file's) name: what Eden's disabled list holds
    unsigned kinds = 0;
    // Its cheats when it lists several: each is chosen on its own. Empty for a single cheat.
    std::vector<Patches::Entry> cheats;
};

inline std::string Lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

inline std::string TitleName(uint64_t title_id) {
    char name[17];
    std::snprintf(name, sizeof(name), "%016" PRIX64, title_id);
    return name;
}

inline std::vector<fs::directory_entry> ListFolder(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error) || error) return {};
#if defined(__PROSPERO__)
    // The console's directory iterator needs this reader (native_directory.h).
    auto entries = Eden::ReadNativeDirectory(folder, error);
    if (error) entries.clear();
    return entries;
#else
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it{folder, error}, end; !error && it != end; it.increment(error))
        entries.push_back(*it);
    return entries;
#endif
}

inline bool IsFolder(const fs::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error) && !error;
}

// The game's folder in mods/ (root), or empty when it has none.
inline std::string TitleFolder(const std::string& root, uint64_t title_id) {
    if (title_id == 0) return {};
    const std::string wanted = Lower(TitleName(title_id));
    for (const auto& entry : ListFolder(root))
        if (IsFolder(entry) && Lower(entry.path().filename().string()) == wanted) return entry.path().string();
    return {};
}

// The path the launcher tells the player to use for a game's mods.
inline std::string TitleFolderToCreate(const std::string& root, uint64_t title_id) {
    return root + "/" + TitleName(title_id);
}

// What a mod's folder holds, as Eden will use it.
inline unsigned Kinds(const fs::path& mod) {
    static constexpr std::string_view kCodeFiles[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1",
        "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9"};
    unsigned kinds = 0;
    for (const auto& part : ListFolder(mod)) {
        if (!IsFolder(part)) continue;
        const std::string name = Lower(part.path().filename().string());
        const auto inside = ListFolder(part.path());
        if (inside.empty()) continue;
        if (name == "romfs" || name == "romfslite" || name == "romfs_ext") kinds |= kFiles;
        if (name == "cheats") kinds |= kCheats;
        if (name != "exefs") continue;
        for (const auto& file : inside) {
            const std::string file_name = file.path().filename().string();
            const std::string extension = file.path().extension().string();
            if (extension == ".ips" || extension == ".pchtxt" ||
                std::find(std::begin(kCodeFiles), std::end(kCodeFiles), file_name) != std::end(kCodeFiles))
                kinds |= kCode;
        }
    }
    return kinds;
}

// The cheats a mod lists (its cheats/ folder's files, one per build of the game, or a loose cheat
// file), once per name, when there are several.
inline std::vector<Patches::Entry> CheatList(std::vector<fs::path> files, const std::string& name);

inline std::vector<fs::path> TextFiles(const fs::path& folder) {
    std::vector<fs::path> files;
    for (const auto& file : ListFolder(folder))
        if (!IsFolder(file) && Lower(file.path().extension().string()) == ".txt") files.push_back(file.path());
    std::sort(files.begin(), files.end());
    return files;
}

inline std::vector<Patches::Entry> CheatList(const fs::directory_entry& mod) {
    std::vector<fs::path> files;
    if (!IsFolder(mod)) {
        files.push_back(mod.path());
    } else {
        for (const auto& part : ListFolder(mod.path()))
            if (IsFolder(part) && Lower(part.path().filename().string()) == "cheats")
                for (auto& file : TextFiles(part.path())) files.push_back(std::move(file));
    }
    std::sort(files.begin(), files.end());
    return CheatList(std::move(files), mod.path().filename().string());
}

// The files of the cheats/ folders, one per version of the game: the game's folder's first, then
// those of cheats/<title ID>/ for versions it has none for.
inline std::vector<fs::path> CheatFolderFiles(const Places& places, uint64_t title_id) {
    std::vector<fs::path> files;
    std::set<std::string> versions;
    const auto add = [&](const fs::path& folder) {
        for (auto& file : TextFiles(folder))
            if (versions.insert(Lower(file.filename().string())).second) files.push_back(std::move(file));
    };
    if (!places.game_folder.empty())
        for (const auto& entry : ListFolder(places.game_folder))
            if (IsFolder(entry) && Lower(entry.path().filename().string()) == "cheats") add(entry.path());
    if (!places.cheats.empty())
        if (const std::string folder = TitleFolder(places.cheats, title_id); !folder.empty()) add(folder);
    return files;
}

inline std::vector<Patches::Entry> CheatList(std::vector<fs::path> files, const std::string& name) {
    std::vector<Patches::Entry> cheats;
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        if (text.size() > (4u << 20)) continue;  // cheat files are small; anything larger is not one
        for (auto& entry : Patches::ParseCheats(text, name)) {
            const bool known = std::any_of(cheats.begin(), cheats.end(), [&](const Patches::Entry& other) {
                return Eden::Cheats::Key(name, other.name) == Eden::Cheats::Key(name, entry.name);
            });
            if (!known) cheats.push_back(std::move(entry));
        }
    }
    if (cheats.size() < 2) cheats.clear();
    return cheats;
}

// The game's mods, in the order Eden applies them: those of its own folder, then those of mods/
// that it has none of the same name of, and the cheats/ folders' cheats as one more.
inline std::vector<Mod> List(const Places& places, uint64_t title_id) {
    std::vector<Mod> mods;
    std::set<std::string> names;
    const auto add_folder = [&](const std::string& folder) {
        for (const auto& entry : ListFolder(folder)) {
            const std::string name = entry.path().filename().string();
            if (name == kCheatsFolder || names.contains(name)) continue;
            if (IsFolder(entry)) {
                if (const unsigned kinds = Kinds(entry.path())) {
                    mods.push_back({name, kinds, kinds & kCheats ? CheatList(entry) : std::vector<Patches::Entry>{}});
                    names.insert(name);
                }
            } else if (name.starts_with("cheat_")) {
                mods.push_back({name, kCheats, CheatList(entry)});
                names.insert(name);
            }
        }
    };
    if (!places.game_folder.empty())
        for (const auto& entry : ListFolder(places.game_folder))
            if (IsFolder(entry) && Lower(entry.path().filename().string()) == "mods") add_folder(entry.path().string());
    if (const std::string folder = TitleFolder(places.mods, title_id); !folder.empty()) add_folder(folder);
    if (auto files = CheatFolderFiles(places, title_id); !files.empty()) {
        const std::string name{kCheatsFolder};
        mods.push_back({name, kCheats, CheatList(std::move(files), name)});
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return mods;
}

inline std::vector<Mod> List(const std::string& root, uint64_t title_id) { return List(Places{root, {}, {}}, title_id); }

// Switching one of a mod's cheats: the game's chosen cheats after it. One of a group (two frame
// rates) takes the other's place.
inline std::vector<std::string> ChooseCheat(const Mod& mod, std::string_view cheat, bool on,
                                            const std::vector<std::string>& chosen) {
    std::set<std::string> ids(chosen.begin(), chosen.end());
    for (std::size_t i = 0; i < mod.cheats.size(); ++i)
        if (mod.cheats[i].name == cheat && ids.contains(mod.cheats[i].id) != on) Patches::Toggle(mod.cheats, ids, i);
    return {ids.begin(), ids.end()};
}

// The cheats that stay off (cheats.h): of the mods that list several, those not chosen.
inline std::vector<std::string> CheatsOff(const std::vector<Mod>& mods, const std::vector<std::string>& chosen) {
    std::vector<std::string> off;
    for (const Mod& mod : mods)
        for (const auto& cheat : mod.cheats)
            if (std::find(chosen.begin(), chosen.end(), cheat.id) == chosen.end())
                off.push_back(Eden::Cheats::Key(mod.name, cheat.name));
    return off;
}

// One line for the log and the crash report: the mods in use, by name, with how many of its
// cheats are chosen after a mod that lists several.
inline std::string Summary(const std::vector<Mod>& mods, const std::vector<std::string>& disabled,
                           const std::vector<std::string>& chosen = {}) {
    std::string used;
    unsigned off = 0;
    for (const Mod& mod : mods) {
        if (std::find(disabled.begin(), disabled.end(), mod.name) != disabled.end()) {
            ++off;
            continue;
        }
        used += (used.empty() ? "" : ", ") + mod.name;
        if (mod.cheats.empty()) continue;
        const auto on = std::count_if(mod.cheats.begin(), mod.cheats.end(), [&](const Patches::Entry& cheat) {
            return std::find(chosen.begin(), chosen.end(), cheat.id) != chosen.end();
        });
        used += " (" + std::to_string(on) + " of " + std::to_string(mod.cheats.size()) + " cheats)";
    }
    if (used.empty()) used = "none";
    if (off != 0) used += " (" + std::to_string(off) + " switched off)";
    return used;
}
} // namespace Eden::Mods
