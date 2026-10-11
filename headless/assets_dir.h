// SPDX-License-Identifier: GPL-3.0-or-later
// The game files folder: keys/, firmware/ and roms/ in the folder chosen in Settings > Game
// files (/data/prosperoeden by default).
// Resolved once per process; a new choice applies when ProsperoEden is reopened. Where the rest
// lives: storage_paths.h; what is saved: settings_store.h.
#pragma once
#include <string>
#include <string_view>

#include "settings_store.h"
#include "storage_paths.h"

namespace Eden {
// The saved folder, else /data/prosperoeden, except that an install from before the setting
// keeps the app folder's assets/ while /data/prosperoeden has no keys (main.cpp moves those
// files to /data/prosperoeden when it can). Without filesystem access nothing can be read.
inline std::string ResolveAssetsDir() {
    const std::string legacy = AppFile("assets");
    if (!FilesystemAccess()) return kDefaultAssetsDir;
    if (std::string saved = LoadSavedAssetsDir(); !saved.empty()) return saved;
    const std::string keys = "/keys/prod.keys";
    if (!FileExists(kDefaultAssetsDir + keys) && FileExists(legacy + keys)) return legacy;
    return kDefaultAssetsDir;
}

// The game files folder in use by this process.
inline const std::string& AssetsDir() {
    static const std::string directory = ResolveAssetsDir();
    return directory;
}
inline std::string AssetsPath(std::string_view child) { return AssetsDir() + "/" + std::string(child); }
} // namespace Eden
