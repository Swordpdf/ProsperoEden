// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The Library's games as a file other homebrew can read: a forwarder maker listing the games to
// make a home screen tile for, with their names and covers (heydemoura/ps5-forwarder-manager#26).
// The launcher writes it whenever it reads the Library; docs/FORWARDER.md describes it.
#pragma once
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Eden::LibraryFile {

inline constexpr int kVersion = 1;

// One game on the console, as the Library lists it.
struct Game {
    std::string file;       // its path in roms/: "Game.nsp" or "Game/Game.nsp"; what --rom takes
    std::string path;       // its full path
    std::string title;      // the name the Library shows
    std::uint64_t title_id = 0;  // 0 when its data could not be read
    std::string cover;      // full path of its cover (TGA), or empty when it has none
};

inline std::string TitleIdText(std::uint64_t title_id) {
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016" PRIX64, title_id);
    return text;
}

// The file's text. Readers take "version" first: a reader of version 1 reads any later version
// that keeps these fields, and new fields are only ever added.
inline std::string Text(const std::string& game_files, const std::vector<Game>& games) {
    nlohmann::json list = nlohmann::json::array();
    for (const Game& game : games) {
        nlohmann::json entry{{"file", game.file}, {"path", game.path}, {"title", game.title}};
        entry["title_id"] = game.title_id ? nlohmann::json(TitleIdText(game.title_id)) : nlohmann::json(nullptr);
        entry["cover"] = game.cover.empty() ? nlohmann::json(nullptr) : nlohmann::json(game.cover);
        list.push_back(std::move(entry));
    }
    const nlohmann::json document{
        {"version", kVersion}, {"app", "PPSA99008"}, {"game_files", game_files}, {"games", std::move(list)}};
    // Names may hold any character a file name can; invalid UTF-8 is replaced, never refused.
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

// Writes the file in one step (a reader never sees half of it), and only when it changed: the
// Library is read often. False when it could not be written.
inline bool Write(const std::string& path, const std::string& text) {
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        std::string old;
        char buffer[4096];
        for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) old.append(buffer, count);
        std::fclose(file);
        if (old == text) return true;
    }
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (ok && std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    std::remove(temporary.c_str());
    return false;
}

} // namespace Eden::LibraryFile
