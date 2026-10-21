# Game files, app data and updates

Where ProsperoEden looks for your keys, firmware and games, where it keeps its own data, and how it is updated. The install steps themselves are in the [README](../README.md#install).

## Game files folder

Only these subfolders matter; the folder itself can have any name and location.

```text
<game files folder>/                    # e.g. /data/prosperoeden, /mnt/ext1/eden, /mnt/usb0/eden
├── keys/
│   ├── prod.keys
│   └── title.keys                      # optional
├── firmware/
│   └── *.nca                           # extracted firmware NCAs
├── roms/
│   └── <Game>/                         # one folder per game
│       ├── <game>.nsp                  # the game (NSP or XCI, or NSZ or XCZ: see Compressed games)
│       ├── <game> update.nsp           # optional: its update and DLC files, beside it
│       ├── <game> DLC.nsp
│       ├── mods/                       # optional: its mods, one folder each (see Mods and cheats)
│       │   └── <mod name>/
│       │       ├── exefs/              # code patches: *.pchtxt, *.ips
│       │       ├── romfs/              # replacement game files
│       │       └── romfs_ext/          # files added to the game's own
│       ├── cheats/                     # optional: its cheats
│       │   └── <build ID>.txt          # one file per version of the game
│       └── reshade/                    # optional: its post-processing presets and effects
│           ├── <preset>.fxp            # offered in this game's settings
│           └── <effect>.fx             # and the .fxh files and Textures/ they use
├── save-import/                        # optional: saves to import, one folder per title ID
├── ryujinx/                            # optional: a Ryujinx data folder to import saves from
├── save-export/                        # written by "Export a copy"
└── .remote-downloads/                  # downloads that are not finished yet (see Download sources)
```

Each game sits in a folder of its own in `roms/`, with everything that belongs to it: its update and DLC files, its mods, its cheats and its post-processing presets. ProsperoEden tells the game from its update and DLC by what each file holds, not by its name, so the files can have any names. Folders inside a game folder are not searched for games, and folders whose names start with a dot are skipped. **Delete from console** also deletes the update and DLC files in the game's folder, and the folder once it is empty.

The folder browser shows how many keys, firmware files, and games each folder holds, so you can check a folder before selecting it. Moving your files later only needs a new selection in Settings; saved settings, covers, and recently played games carry over.

### Compressed games (NSZ and XCZ)

Play your compressed library as it is: no converting, no extra copies of games that do not need one.

`NSZ` · `XCZ` · updates and DLC too · compressed with [nsz](https://github.com/nicoboss/nsz)

NSZ and XCZ files are NSPs and XCIs whose larger contents nsz compressed. Put them in `roms/` like any other game. How a game plays depends on how nsz compressed it:

| | **Block** (`nsz -B`) | **Solid** (`nsz -S`) |
|---|---|---|
| nsz's default for | XCZ | NSZ |
| First start | at once | unpacked once, which takes a while for a large game |
| Later starts | at once | at once, from the unpacked copy |
| Space on the console | the file itself | the file, plus the last played game uncompressed |

**Block compression** can be read anywhere in the file. These games play straight from the compressed file.

**Solid compression** can only be read from its start. The first time such a game starts, ProsperoEden unpacks its compressed contents once into `/data/prosperoeden/user/cache/nsz`. That needs about as much free space as the game uncompressed; `stderr.log` shows how far it is. Only the game you play last keeps its unpacked copy: unpacking another one removes it. Touchpad + L1 while it unpacks stops it, and it is done again the next time the game starts.

**In the Library,** a solid-compressed game or update is read at the end of its data when it is first looked at, which takes a few seconds per GB. What that finds is kept beside the unpacked copies (up to 16 MiB per game or update, usually about 5), so it is quick from then on.

**Short on space?** When the console's storage is too full to unpack a game, the log says so, and the game is read without unpacking instead: far too slow to play, so free some space first. To play a solid-compressed game without ever unpacking it, compress it again with block compression on a computer:

```text
nsz -D <game>.nsz
nsz -C -B <game>.nsp
```

The folder `/data/prosperoeden/user/cache/nsz` can be deleted whenever no game is running; it is made again as needed.

## App data

ProsperoEden keeps its own data in `/data/prosperoeden`, separately from the game files folder:

```text
/data/prosperoeden/
├── config/prosperoeden.json            # every profile's settings, and the game files folder
├── config/remote/                      # download sources and save sync, when you use them:
│   ├── sources.json                    #   the sources and how to reach them (written by you)
│   ├── queue.json                      #   the download queue
│   ├── <source>/catalog.json           #   each source's game list, kept for the next start
│   ├── save-sync.json                  #   where each profile syncs its save data (filled in by you)
│   └── save-sync/                      #   what the save sync keeps: the console as a device of
│                                       #   each profile's server, the games still to be synced
├── backup/save-sync/                   # save data the save sync replaced, the last three per game
├── covers/                             # cached game covers
├── logs/                               # current and previous session logs, crash reports
├── previous-version/PPSA99008-backup/  # the version the last self-update replaced
└── user/                               # every profile's saves, and emulator user data
    ├── cache/nsz/                      # solid-compressed NSZ games: the last one played, unpacked
    └── lossless/Lossless.dll           # optional: your copy, for frame generation
```

The app itself stays in `/data/homebrew/PPSA99008`; see [Updating](#updating).

ProsperoEden does not include keys, firmware, games, or other copyrighted console data. Dump these files from hardware and software you own. Do not download or redistribute them.

This is the only place it keeps them. If ProsperoEden cannot get access to the console's storage when it starts, the home screen says so and no game can be started; your data is untouched, and closing ProsperoEden and opening it again is the first thing to try. Earlier versions kept a second set of settings and saves in the app's own sandbox in that case, which made saves and settings seem to disappear on consoles where access came and went.

Keys, firmware and games that an early version kept inside the app's folder (`/data/homebrew/PPSA99008/assets`) are moved to `/data/prosperoeden` the first time this version starts, so that replacing the app cannot remove them.

## Updating

Close ProsperoEden first. Your settings, saves, covers and logs are in `/data/prosperoeden`, outside the app, so an update keeps them. Afterwards the About screen shows the version that is running.

Once each time it opens, ProsperoEden asks [homebrew.page](https://homebrew.page/ps5) which release of it is listed there. The request carries the app's title ID and nothing else. If the listed release is newer than the one running, a dialog offers it, with its version and download size:

- **Update now** downloads the release ZIP from its GitHub release, checks it against the SHA-256 in the catalog's signed list, and unpacks it beside the app. A ring shows how far it is and the time left; Circle cancels, and nothing has changed until the end. Then ProsperoEden closes, the update helper (`self-updater.elf`, sent to the console's payload loader on port 9021) replaces the app's files, and the console shows a notification. Open ProsperoEden again to use the new version. The version it replaced is kept; see [Going back to the previous version](#going-back-to-the-previous-version).
- **What's new** (when the release has notes) shows what changes in it before you decide: the release notes as homebrew.page lists them. Up/Down scroll, L1/R1 jump a page, Cross updates and Circle goes back to the offer. Triangle opens them from the offer too.
- **Skip** keeps the version you have. The dialog shows again the next time ProsperoEden opens, not when you come back from a game.

To stay on a version without being asked at every start, turn off **Settings > Diagnostics > Update notices** (`"system": { "update_notices": false }` in the settings file). ProsperoEden still asks homebrew.page, but neither the dialog nor the notification below shows; the About screen names the newer version beside the one running. It applies the next time ProsperoEden opens.

If the download or the unpacking fails, the dialog says why and offers **Try again**; the app stays as it was. Without a network, or without an answer, nothing is shown. When ProsperoEden cannot install the release itself (no payload loader, or an install it cannot find), the menu says **Update available** at the top right for ten seconds instead, and the steps below still work:

- **Folder install.** Copy the `PPSA99008` folder from the new release ZIP over `/data/homebrew/PPSA99008`, replacing the files it has, then start ProsperoEden. Files you put there yourself, such as `language.txt`, stay.

## Going back to the previous version

When ProsperoEden updates itself, the files of the version it replaces are kept in `/data/prosperoeden/previous-version/PPSA99008-backup`, until the next update replaces them in turn. If a new version misbehaves, you can put the previous one back:

1. Close ProsperoEden.
2. Copy everything inside `/data/prosperoeden/previous-version/PPSA99008-backup` over the app's folder (`/data/homebrew/PPSA99008`), replacing the files it has, for example over FTP.
3. Start ProsperoEden. The About screen shows the version that is running.

The update will be offered again the next time ProsperoEden opens; choose **Skip** to stay on the version you put back. Settings, saves and game files are not part of this: they stay as they are. An update done by hand, by copying a release over the app's folder, keeps no previous version.

## Upgrading from an earlier alpha

Earlier versions read everything from `/data/homebrew/PPSA99008/assets/`. That folder keeps working until you choose a game files folder, and settings are migrated automatically on first launch. To move to the new layout, move `assets/keys`, `assets/firmware` and `assets/roms` into any folder, then select it in **Settings > Game files**. The release ZIP contains no user files, so copy its app files over your installation without deleting your own data.
