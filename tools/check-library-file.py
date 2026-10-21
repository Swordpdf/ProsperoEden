#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check library.json, the Library's games for other homebrew to read (headless/library_file.h).

Usage: check-library-file.py [FOLDER]

nlohmann/json comes from JSON_INCLUDE, the system, or a copy found under FOLDER (the release
build passes its scratch folder, where Eden's build fetched one). This compiles the header, writes
a list and reads it back as a forwarder maker would, and checks that the launcher writes the file
from the console's own games only, with filesystem access, and that docs/FORWARDER.md describes it.
"""
from pathlib import Path
import json, os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
services = (root / 'headless/prosperoeden/eden_services.cpp').read_text()
games = services[services.index('std::vector<pe::ui::Game> EdenServices::games() {'):]
games = games[:games.index('\n}\n')]
write = games.index('Eden::LibraryFile::Write(std::string{Eden::kDataDir} + "/library.json",')
assert 'Eden::LibraryFile::Text(Eden::AssetsDir(), listed)' in games
assert games.rfind('if (Eden::FilesystemAccess()) {', 0, write) > games.index('games.push_back(std::move(game));'), \
    'the file is written after the console games are read, with filesystem access'
assert write < games.index('game.remote = true;'), 'only the console games are listed, not the download sources\''
assert '`/data/prosperoeden/library.json`' in (root / 'docs/FORWARDER.md').read_text()


def json_include():
    if os.environ.get('JSON_INCLUDE'):
        return os.environ['JSON_INCLUDE']
    for folder in ('/usr/include', '/usr/local/include', '/opt/homebrew/include'):
        if (Path(folder) / 'nlohmann/json.hpp').exists():
            return folder
    if len(sys.argv) > 1:
        for header in sorted(Path(sys.argv[1]).glob('**/nlohmann/json.hpp')):
            return str(header.parent.parent)
    sys.exit('check-library-file: no nlohmann/json.hpp (set JSON_INCLUDE or pass a folder holding one)')


code = r'''
#include "library_file.h"
#include <cassert>
#include <cstdio>
#include <sys/stat.h>
using namespace Eden::LibraryFile;
int main(int, char** argv) {
    const std::string folder = argv[1];
    const std::vector<Game> games = {
        {"Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp", "/mnt/usb0/eden/roms/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp",
         "Mario Kart 8 Deluxe", 0x0100152000022000, "/data/prosperoeden/covers/1a2b3c4d.tga"},
        {"Zelda.xci", "/mnt/usb0/eden/roms/Zelda.xci", "ゼルダの伝説 \"Tears\"", 0x0100F2C0115B6000, ""},
        {"Broken.nsp", "/mnt/usb0/eden/roms/Broken.nsp", std::string("Bad \xff name"), 0, ""},
    };
    const std::string text = Text("/mnt/usb0/eden", games);
    const std::string path = folder + "/library.json";
    assert(Write(path, text));
    struct stat first {};
    assert(stat(path.c_str(), &first) == 0);
    // The same list again: the file is left as it is.
    assert(Write(path, text));
    struct stat again {};
    assert(stat(path.c_str(), &again) == 0 && again.st_ino == first.st_ino);
    // A changed list replaces it in one step, leaving no temporary file behind.
    assert(Write(path, Text("/mnt/usb0/eden", {games[0]})));
    assert(Write(path, text));
    struct stat temporary {};
    assert(stat((path + ".tmp").c_str(), &temporary) != 0);
    // A folder that cannot be written: false, nothing thrown.
    assert(!Write(folder + "/missing/library.json", text));
    std::puts(path.c_str());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'library-file'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'headless'), '-isystem', json_include(), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    path = subprocess.run([str(exe), tmp], check=True, capture_output=True, text=True).stdout.strip()
    # Read back as a forwarder maker would.
    document = json.loads(Path(path).read_text(encoding='utf-8'))
    assert document['version'] == 1 and document['app'] == 'PPSA99008'
    assert document['game_files'] == '/mnt/usb0/eden'
    kart, zelda, broken = document['games']
    assert kart == {'file': 'Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp',
                    'path': '/mnt/usb0/eden/roms/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp',
                    'title': 'Mario Kart 8 Deluxe', 'title_id': '0100152000022000',
                    'cover': '/data/prosperoeden/covers/1a2b3c4d.tga'}
    assert zelda['title'] == 'ゼルダの伝説 "Tears"' and zelda['title_id'] == '0100F2C0115B6000'
    assert zelda['cover'] is None
    assert broken['title_id'] is None and broken['title'] == 'Bad � name'
print('Library file: PASS library.json lists the console\'s games (path in roms/, title, title ID, cover) for other homebrew')
