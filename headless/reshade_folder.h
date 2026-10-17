// SPDX-License-Identifier: GPL-3.0-or-later
// A game's own post-processing files: the reshade/ folder of its game folder in roms/ (issue 168),
// with its presets (.fxp) and the ReShade FX effects they use (.fx, and the .fxh and textures
// those include). Eden's catalogs read it before the app's own post_presets/ and post_shaders/
// (headless/CMakeLists.txt, "A game's own post-processing"): a preset there is offered for that
// game only, and an effect there takes the place of one of the same name for that game. The
// launcher sets it while it shows a game's settings, main.cpp for the game that starts.
#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace Eden::Reshade {

// The folder, or empty when the game has none (a game directly in roms/, or the general settings).
inline std::filesystem::path& GameFolder() {
    static std::filesystem::path folder;
    return folder;
}

// Sets the folder for a game folder (empty: none); true when it changed.
inline bool SetGameFolder(const std::string& game_folder) {
    std::filesystem::path folder;
    std::error_code error;
    if (!game_folder.empty())
        for (const char* name : {"reshade", "ReShade", "Reshade", "RESHADE"})
            if (std::filesystem::is_directory(std::filesystem::path{game_folder} / name, error)) {
                folder = std::filesystem::path{game_folder} / name;
                break;
            }
    if (folder == GameFolder()) return false;
    GameFolder() = std::move(folder);
    return true;
}

// Where an effect of a preset's chain is read from: the game's own copy first.
inline std::filesystem::path EffectPath(const std::filesystem::path& root, const std::string& file) {
    std::error_code error;
    if (!GameFolder().empty())
        if (const auto own = GameFolder() / file; std::filesystem::is_regular_file(own, error)) return own;
    return root / file;
}

} // namespace Eden::Reshade
