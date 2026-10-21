#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that Settings > Game files counts the games in game folders.

The GAMES line counted the NSP and XCI files directly in roms/, so a game files folder whose
games were all in game folders (roms/<game>/, since 1.005.000) read "0 games" as a warning.
This compiles headless/prosperoeden/eden_services.cpp's CountGames with the real ValidRomPath
(headless/settings_store.h) and a folder listing like its own, makes a roms/ folder and checks
the count, and that the Game files browser uses it.
"""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
services = (root / 'headless/prosperoeden/eden_services.cpp').read_text()
start = services.index('int CountGames(const std::string& roms) {')
count = services[start:services.index('\n}\n', start) + 3]
assert 'info.games = CountGames(JoinPath(directory, "roms"));' in services
store = (root / 'headless/settings_store.h').read_text()
start = store.index('inline bool ValidRomPath(std::string_view name) {')
valid = store[start:store.index('\n}\n', start) + 3]

code = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;
namespace Eden {
''' + valid + r'''
}
// ListEntries as eden_services.cpp has it: the folders or the regular files, ok when readable.
std::vector<std::string> ListEntries(const std::string& path, bool folders, bool& ok) {
    std::vector<std::string> names;
    std::error_code error;
    fs::directory_iterator it{path, error};
    ok = !error;
    for (fs::directory_iterator end; !error && it != end; it.increment(error))
        if (folders ? it->is_directory() : it->is_regular_file()) names.push_back(it->path().filename().string());
    return names;
}
std::string JoinPath(const std::string& directory, const std::string& name) { return directory + "/" + name; }
''' + count + r'''
static void Touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "x";
}
int main(int, char** argv) {
    const fs::path base{argv[1]};
    // No roms folder: -1 ("No roms folder"); an empty one: 0.
    assert(CountGames((base / "none").string()) == -1);
    fs::create_directories(base / "empty");
    assert(CountGames((base / "empty").string()) == 0);
    // Every game in a game folder, with its update and DLC: one game each.
    const fs::path roms = base / "roms";
    Touch(roms / "Mario Kart 8 Deluxe" / "Mario Kart 8 Deluxe.nsp");
    Touch(roms / "Mario Kart 8 Deluxe" / "Mario Kart 8 Deluxe Update.nsp");
    Touch(roms / "Mario Kart 8 Deluxe" / "Booster Course.nsp");
    Touch(roms / "Mario Kart 8 Deluxe" / "mods" / "Some Mod" / "romfs" / "a.bin");
    Touch(roms / "Kirby" / "Kirby.XCI");
    assert(CountGames(roms.string()) == 2);
    // Games directly in roms/ count as before, next to game folders.
    Touch(roms / "Zelda.nsp");
    Touch(roms / "notes.txt");
    assert(CountGames(roms.string()) == 3);
    // Folders without a game, hidden folders, and games deeper down are not games of the Library.
    Touch(roms / "Only Mods" / "mods" / "x.txt");
    Touch(roms / ".trash" / "Old.nsp");
    Touch(roms / "Series" / "Part 1" / "Part 1.nsp");
    fs::create_directories(roms / "Odd" / "Folder.nsp");
    assert(CountGames(roms.string()) == 3);
    std::puts("game files count: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'game-files-count'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-',
                    '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe), tmp], check=True)
print('Game files count: PASS Settings > Game files counts the games in game folders, one each')
