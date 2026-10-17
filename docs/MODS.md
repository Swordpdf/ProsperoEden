# Mods and cheats

A mod changes a game: a patch to its code (`.pchtxt` or `.ips` files in an `exefs` folder) or replacement game files (a `romfs` or `romfs_ext` folder). Cheats are codes that change the game while it runs. Both go in the game's own folder in `roms/` (see [Game files folder](INSTALL.md#game-files-folder)), and each has a list of its own in the game's settings.

```text
roms/<Game>/
├── <game>.nsp
├── mods/
│   └── <mod name>/
│       ├── exefs/                      # code patches: *.pchtxt, *.ips
│       ├── romfs/                      # replacement game files
│       └── romfs_ext/                  # files added to the game's own
└── cheats/
    └── <build ID>.txt                  # cheats for one version of the game
```

## Mods

- **Add a mod.** Each mod is a folder. Copy it to the `mods` folder in the game's folder, so that a patch ends up at `roms/<Game>/mods/<mod name>/exefs/<file>.pchtxt`. Mods made for other emulators of the same console come in this layout. The Mods screen names the exact folder, and Square creates it. Folder names may be in any letter case.
- **Switch it on or off.** In the Library, press Triangle on the game and pick **Mods**. Every mod found is listed with a switch. A mod is on unless you switch it off, and a change applies the next time the game starts.
- **Switch all of a game's mods off or on.** In the Library, a game's details say how many of its mods are on; Square switches them all off or on. Off, the game starts without any of its mods, and each mod keeps its own switch for when you turn it back on. The game's cheats are not mods: they keep their own switch.
- **A mod that wants more memory.** Some mods keep a block of memory of their own. They ask the console's kernel how much memory the game has and take what they want out of the answer, so a mod that wants more than the 4 GiB a game's console has finds less than it asked for. The game then stops before its first picture: the log says `jumped to an unmapped address` a moment after the mods were read, which is the mod's own check and not a fault of the game or of ProsperoEden. A mod that needs this says so in its own notes. Give that game the larger memory: **Settings > Performance > Memory**, which is every game's at once, or its own `"memory_layout": "6Gb"`, or `"8Gb"`, inside its `"performance"` entry under `"games"` in the settings file, which comes first; see [Console memory](SETTINGS.md#console-memory). Only the games with such a mod need it, and the larger memory takes more of the console's memory pool while the game runs, so leave the rest at 4 GiB.
- **See what a game has.** The home screen and the Library count a game's mods next to its update and DLC (`Update 1.2.0, 2 DLC, 1 mod`), and say so when some are switched off (`1 of 2 mods on`).
- **Match the game's version.** A patch is made for one version of a game. One made for another version is ignored without a message, so check that the mod matches the update in the game's folder.
- **Frame rate.** A patch that makes a 30 FPS game run at 60 FPS works on the 60 Hz output ProsperoEden uses. A patch for more than 60 FPS needs the 120 Hz output: set **Refresh rate** to 120 Hz in the game's settings (Triangle in the Library) or in **Settings > Video**. That takes a display that shows 120 Hz and the PS5's own 120 Hz output setting; without them the game runs at 60 Hz. A 60 FPS patch can gain from it too: a game that misses some frames at 60 Hz has twice as many chances to show them. A patch for more frames than the output shows (240 FPS on the 120 Hz output, 120 FPS on the 60 Hz one) still runs at its own pace: the frames the display has no refresh for are left out.

## Cheats

- **Add cheats.** Cheats are text files in the layout that cheat collections for other emulators of the same console use: each cheat starts with its name in square brackets (`[Infinite health]`), followed by its code lines. Copy the file to the `cheats` folder in the game's folder, as `roms/<Game>/cheats/<build ID>.txt`. The build ID is the code the file is named after in cheat collections: each version of a game has its own, so pick the file for the version you run (the update in the game's folder). The Cheats screen names the exact folder. A mod that comes with a `cheats` folder of its own keeps it: its cheats are listed with the others.
- **Switch them on or off.** In the Library, press Triangle on the game and pick **Cheats**. Every cheat of the game is listed with its own switch, whichever file it came from, with the file's folder or mod under its name. A file that holds many cheats, as the collections made for a game do, has none of them running until you switch it on; a file with a single cheat runs it unless you switch it off. Cheats that replace each other, such as two frame rates or two resolutions, take each other's place: switching one on switches the other off. A change applies the next time the game starts.
- **Switch all of a game's cheats off or on.** **All cheats**, the first switch of the Cheats screen, starts the game without any of its cheats; each cheat keeps its own switch for when you turn it back on. It is apart from the Mods switch: mods can be off while cheats run, and the other way round. A mod that both changes the game and comes with cheats runs none of them while it is switched off.

ProsperoEden does not include or download mods or cheats.
