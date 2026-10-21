#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a started game is remembered by its name in roms/ (headless/main.cpp, issue 264).

The home page finds the last and the recent games at roms/<name>. A game in a game folder
(roms/Game/Game.nsp) saved by its file name alone was not found there and was left out. This
compiles the frontend's own lines that make the name and checks them on a game at the top of
roms/, one in a game folder and one elsewhere, and that the history is saved by that name.
"""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
start = main.index('        const std::string roms_folder = Eden::AssetsPath("roms/");')
end = main.index(';', main.index('const std::string rom_name', start)) + 1
naming = main[start:end]

# The history is saved by that name, and nothing saves it by the file name alone.
saves = [line.strip() for line in main.splitlines() if 'SaveLastGame(' in line or 'SaveRecentGame(' in line]
assert saves == ['const bool saved_last = Eden::SaveLastGame(rom_name);',
                 'const bool saved_recent = Eden::SaveRecentGame(rom_name);'], saves
# The loading screen's cover goes by the same name.
assert 'Eden::Loading::SetArtFromCover(Eden::CoverFile(rom_name));' in main

code = r'''
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
namespace Eden {
std::string AssetsPath(const std::string& relative) { return "/mnt/usb0/eden/" + relative; }
}
static std::string Name(const std::string& selected_game) {
''' + naming + r'''
    return rom_name;
}
int main() {
    assert(Name("/mnt/usb0/eden/roms/Game [0100000000010000].nsp") == "Game [0100000000010000].nsp");
    assert(Name("/mnt/usb0/eden/roms/Game/Game [0100000000010000].nsp") == "Game/Game [0100000000010000].nsp");
    assert(Name("/mnt/usb0/eden/roms/Kart Carnival/Kart.xci") == "Kart Carnival/Kart.xci");
    // A game outside roms/ (development builds start one from anywhere): its file name.
    assert(Name("/data/assets/roms/Other.nsp") == "Other.nsp");
    // Another folder whose path only begins like roms/ is not in it.
    assert(Name("/mnt/usb0/eden/roms-old/Game.nsp") == "Game.nsp");
    std::puts("recent games: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'recent-games'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-',
                    '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('Recent games: PASS saved by the name in roms/ (game folders too), as the home page finds them')
