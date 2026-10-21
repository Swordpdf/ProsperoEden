// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace Eden {

struct ForwardedArgs {
    std::string rom;
    bool exit_after_game = false;
};

inline ForwardedArgs ParseForwardedArgs(int argc, char** argv) {
    ForwardedArgs result;
    if (argc <= 0 || argv == nullptr) return result;
    constexpr std::string_view rom_flag = "--rom";
    constexpr std::string_view rom_prefix = "--rom=";
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr) break;
        const std::string_view arg{argv[i]};
        if (arg == "--exit-after-game") {
            result.exit_after_game = true;
        } else if (arg == rom_flag) {
            if (i + 1 < argc && argv[i + 1] != nullptr) result.rom = argv[++i];
        } else if (arg.starts_with(rom_prefix)) {
            result.rom = std::string{arg.substr(rom_prefix.size())};
        }
    }
    return result;
}

inline std::string ResolveForwardedRom(std::string_view rom, std::string_view roms_dir) {
    while (!rom.empty() && (rom.back() == ' ' || rom.back() == '\r' || rom.back() == '\n')) rom.remove_suffix(1);
    if (rom.empty()) return {};
    if (rom.front() == '/') return std::string{rom};
    for (std::string_view rest = rom; !rest.empty();) {
        const auto slash = rest.find('/');
        if (rest.substr(0, slash) == "..") return {};
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    std::string path{roms_dir};
    if (!path.empty() && path.back() != '/') path += '/';
    path += rom;
    return path;
}

// A forwarder keeps the path its game had when it was made. Since 1.005.000 a game can have its
// own folder in roms/ (its update, DLC and mods beside it), and a game moved into one is no longer
// at roms/Game.nsp but at roms/<folder>/Game.nsp: the forwarder said it was not found (issue 281).
// For a missing game directly in roms/, find the file of that name one folder down: in the folder
// named after it first, else in the only game folder that has it. Empty when there is none.
inline std::string FindMovedForwardedRom(const std::string& path, const std::string& roms_dir) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path missing{path};
    const fs::path name = missing.filename();
    if (name.empty() || roms_dir.empty()) return {};
    if (fs::path{missing.parent_path().string() + '/'}.lexically_normal() !=
        fs::path{roms_dir + '/'}.lexically_normal())
        return {};
    const fs::path roms{roms_dir};
    const fs::path own = roms / missing.stem() / name;
    if (fs::is_regular_file(own, error)) return own.string();
    std::string found;
    for (fs::directory_iterator it{roms, error}, end; !error && it != end; it.increment(error)) {
        const std::string folder = it->path().filename().string();
        if (folder.empty() || folder.front() == '.' || !it->is_directory(error)) continue;
        const fs::path candidate = it->path() / name;
        if (!fs::is_regular_file(candidate, error)) continue;
        if (!found.empty()) return {}; // in two game folders: not ours to choose
        found = candidate.string();
    }
    return found;
}

}
