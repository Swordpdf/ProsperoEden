#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a game folder's mods and cheats apply to the game's own title alone (issue 281).

Eden asks for the mods of every title it patches, the game's DLC and update among them. A game
folder's mods were given for all of them, and the game's disabled list (its title's) does not
cover its DLC: a mod switched off for the game still applied to its DLC's data. This compiles
headless/mods.h, makes a game folder with a mod and a cheat file, and checks which titles get
them; and that the patch manager's mod folder (game_folder_vfs.h) and the frontend (main.cpp)
go by it.
"""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
vfs = (root / 'headless/game_folder_vfs.h').read_text()
assert 'Mods::GameFolderFor(title_id)}' in vfs and 'Mods::SessionGameFolder()}' not in vfs, \
    'game_folder_vfs.h must take the game folder for the asked title only'
main = (root / 'headless/main.cpp').read_text()
assert main.count('Eden::Mods::SessionTitle() = 0;') == 1 and main.count('Eden::Mods::SessionTitle() = game_title;') == 1

code = r'''
#include "mods.h"
#include <cassert>
#include <cstdio>
#include <fstream>
namespace fs = std::filesystem;
using namespace Eden::Mods;
int main(int, char** argv) {
    const fs::path game = fs::path(argv[1]) / "roms" / "Mario Kart 8 Deluxe";
    fs::create_directories(game / "mods" / "Some Mod" / "romfs" / "Data");
    std::ofstream(game / "mods" / "Some Mod" / "romfs" / "Data" / "file.bin") << "x";
    fs::create_directories(game / "cheats");
    std::ofstream(game / "cheats" / "6A85262F21B90364.txt") << "[Cheat]\n04000000 00000000 00000000\n";
    constexpr std::uint64_t base = 0x0100152000022000, update = 0x0100152000022800, dlc = 0x0100152000023001;
    SessionGameFolder() = game.string();
    SessionTitle() = base;
    // The game's own title: its folder, and its mod.
    assert(GameFolderFor(base) == game.string());
    const Places own{"", "", GameFolderFor(base)};
    const auto mods = List(own, base);
    const bool has_mod = std::any_of(mods.begin(), mods.end(), [](const Mod& m) { return m.name == "Some Mod"; });
    assert(has_mod);
    assert(!CheatFolderFiles(own, base).empty());
    // Its DLC and its update: no game folder, so neither its mods nor its cheats.
    for (const auto title : {dlc, update, std::uint64_t{0x0100000000010000}}) {
        assert(GameFolderFor(title).empty());
        const Places other{"", "", GameFolderFor(title)};
        assert(List(other, title).empty());
        assert(CheatFolderFiles(other, title).empty());
    }
    // No game being started (or a game directly in roms/): nothing, also for title 0.
    SessionTitle() = 0;
    assert(GameFolderFor(0).empty() && GameFolderFor(base).empty());
    SessionGameFolder().clear();
    SessionTitle() = base;
    assert(GameFolderFor(base).empty());
    std::puts("game folder mods: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'game-folder-mods'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe), tmp], check=True)
print("Game folder mods: PASS a game folder's mods and cheats apply to its own title, not its DLC or update")
