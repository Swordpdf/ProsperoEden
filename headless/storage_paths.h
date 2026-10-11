// SPDX-License-Identifier: GPL-3.0-or-later
// Where ProsperoEden keeps things. With filesystem access (elevation/elevation.hpp, requested
// first thing in main) the app uses real console paths:
//   app folder     the install location, normally /data/homebrew/PPSA99008
//   data           /data/prosperoeden: config/ (prosperoeden.json), logs/, covers/, user/
// Without it (no resident service or local elfldr, or the request failed) the app has no data:
// it reads the app folder as /app0, keeps only its logs in /download0, and the launcher says
// that it has no access. It used to keep a second set of settings and saves in /download0
// then, and a console where access came and went showed one set or the other: saves and
// settings seemed to be gone.
#pragma once
#include <cstdio>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace Eden {
inline constexpr const char* kDataDir = "/data/prosperoeden";
inline constexpr const char* kDefaultAssetsDir = "/data/prosperoeden";
inline constexpr const char* kInstallDir = "/data/homebrew/PPSA99008";

// Filesystem access requested at startup: -1 not requested, 0 granted, otherwise the
// elevation::Status that refused it.
inline int& FilesystemAccessStatus() {
    static int status = -1;
    return status;
}
inline bool FilesystemAccess() { return FilesystemAccessStatus() == 0; }

inline bool FileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
inline bool DirectoryExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Where the PS5 mounts the app it runs, whatever the source: a folder anywhere ShadowMountPlus
// scans (/data, ext or USB drives, its manual list) or an image.
inline constexpr const char* kMountedAppDir = "/system_ex/app/PPSA99008";

// The app's own files (eboot, ui/, development markers): /app0 while it is still mounted (some
// kinds of filesystem access keep it), else the PS5's mount of the running app, else the usual
// install folder. The mount follows ShadowMountPlus: an app on a USB drive is found there although
// /data/homebrew may hold nothing or an older copy.
inline const std::string& AppDir() {
    static const std::string directory = [] {
        if (!FilesystemAccess()) return std::string{"/app0"};
        for (const char* candidate : {"/app0", kMountedAppDir, kInstallDir, "/mnt/sandbox/PPSA99008_000/app0"})
            if (FileExists(std::string{candidate} + "/eboot.bin")) return std::string{candidate};
        return std::string{kInstallDir};
    }();
    return directory;
}
inline std::string AppFile(std::string_view name) { return AppDir() + "/" + std::string(name); }

// Settings, logs, covers and Eden's user folder.
inline std::string ConfigDir() { return std::string{kDataDir} + "/config"; }
// The logs alone have a place in the sandbox: they are how a start without access is looked into.
inline std::string LogsDir() { return FilesystemAccess() ? std::string{kDataDir} + "/logs" : "/download0/eden-headless-g7"; }
inline std::string CoversDir() { return std::string{kDataDir} + "/covers"; }
inline std::string UserDir() { return std::string{kDataDir} + "/user"; }
inline std::string ConfigFile(std::string_view name) { return ConfigDir() + "/" + std::string(name); }
inline std::string LogFile(std::string_view name) { return LogsDir() + "/" + std::string(name); }

inline bool ValidAssetsDir(std::string_view path) {
    if (path.empty() || path.size() > 240 || path.front() != '/') return false;
    if (path.size() > 1 && path.back() == '/') return false;
    for (unsigned char c : path)
        if (c < 32 || c == 127 || c == '\\') return false;
    for (std::size_t start = 1; start <= path.size();) {
        const std::size_t end = path.find('/', start);
        const std::string_view part = path.substr(start, end == std::string_view::npos ? path.npos : end - start);
        if (part.empty() || part == "." || part == "..") return path == "/";
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

} // namespace Eden
