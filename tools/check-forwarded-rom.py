#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a forwarder still starts its game after the game is moved into a game folder (issue 281).

A forwarder keeps the path its game had when it was made (roms/Game.nsp). A game moved into its
own folder since (roms/<folder>/Game.nsp) was "not found". This compiles headless/forwarded_launch.h,
makes a roms/ folder and checks which file a forwarded path finds, and that headless/main.cpp
looks there only when the path itself is missing.
"""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
start = main.index('const std::string path = Eden::ResolveForwardedRom(forwarded.rom')
block = main[start:main.index('Eden::BootTrace::Line("forwarded game:', start)]
assert 'Eden::FindMovedForwardedRom(path, Eden::AssetsPath("roms"))' in block
assert block.index('Eden::FileExists(path)) forwarded_game = path;') < block.index('forwarded_game = moved;'), \
    'the path the forwarder gave comes first'
assert 'launch_error = "Forwarded game not found: " + (path.empty() ? forwarded.rom : path);' in block

code = r'''
#include "forwarded_launch.h"
#include <cassert>
#include <cstdio>
#include <fstream>
namespace fs = std::filesystem;
using namespace Eden;
static void Touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "x";
}
// What main.cpp starts for a forwarded --rom.
static std::string Start(const std::string& rom, const std::string& roms) {
    const std::string path = ResolveForwardedRom(rom, roms);
    if (path.empty()) return {};
    if (fs::is_regular_file(path)) return path;
    return FindMovedForwardedRom(path, roms);
}
int main(int, char** argv) {
    const fs::path base = fs::path(argv[1]);
    const std::string roms = (base / "roms").string();
    Touch(base / "roms" / "Still Here.nsp");
    Touch(base / "roms" / "Still Here" / "Still Here.nsp");
    Touch(base / "roms" / "Mario Kart 8 Deluxe" / "Mario Kart 8 Deluxe.nsp");
    Touch(base / "roms" / "Mario Kart 8 Deluxe" / "Mario Kart 8 Deluxe Update.nsp");
    Touch(base / "roms" / "MK8 stuff" / "Kart.xci");
    Touch(base / "roms" / "A" / "Twice.nsp");
    Touch(base / "roms" / "B" / "Twice.nsp");
    Touch(base / "roms" / "Own" / "Own.nsp");
    Touch(base / "roms" / "Other" / "Own.nsp");
    Touch(base / "roms" / ".trash" / "Hidden.nsp");
    fs::create_directories(base / "roms" / "Odd" / "Folder.nsp");
    Touch(base / "roms" / "Deep" / "Deep.xci");
    Touch(base / "roms" / "Outside" / "Outside.nsp");
    // A game still where its forwarder says: that file, not a copy in a folder.
    assert(Start("Still Here.nsp", roms) == roms + "/Still Here.nsp");
    // Moved into the folder named after it, the forwarder made before (relative or absolute).
    assert(Start("Mario Kart 8 Deluxe.nsp", roms) == roms + "/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp");
    assert(Start(roms + "/Mario Kart 8 Deluxe.nsp", roms) == roms + "/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp");
    assert(Start("Mario Kart 8 Deluxe.nsp", roms + "/") == roms + "/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp");
    // Moved into a folder of another name: the only one that has it.
    assert(Start("Kart.xci", roms) == roms + "/MK8 stuff/Kart.xci");
    // In two folders: neither is chosen, unless one is named after it.
    assert(Start("Twice.nsp", roms).empty());
    assert(Start("Own.nsp", roms) == roms + "/Own/Own.nsp");
    // Hidden folders, a folder named like a game, and games that were never there: not found.
    assert(Start("Hidden.nsp", roms).empty());
    assert(Start("Folder.nsp", roms).empty());
    assert(Start("Missing.nsp", roms).empty());
    // Only a game the forwarder put directly in roms/ is looked for: not one in a subfolder, and
    // not one outside roms/.
    assert(Start("rpg/Deep.xci", roms).empty());
    assert(FindMovedForwardedRom((base / "elsewhere" / "Outside.nsp").string(), roms).empty());
    assert(FindMovedForwardedRom(roms + "/Mario Kart 8 Deluxe.nsp", "").empty());
    assert(FindMovedForwardedRom(roms + "/", roms).empty());
    // No roms/ folder at all (file access not granted): nothing, without throwing.
    assert(FindMovedForwardedRom((base / "none" / "Game.nsp").string(), (base / "none").string()).empty());
    std::puts("forwarded rom: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'forwarded-rom'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe), tmp], check=True)
print('Forwarded rom: PASS a forwarder finds its game after the game is moved into a game folder')
