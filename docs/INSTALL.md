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
│   ├── Game.nsp
│   └── Game.xci
├── updates/                            # optional: update and DLC files
│   └── Game update.nsp
├── mods/                               # optional: mods, one folder per title ID
│   └── <title ID>/                     # the 16-character code in the game's file name
│       ├── <mod name>/
│       │   ├── exefs/                  # code patches: *.pchtxt, *.ips
│       │   ├── romfs/                  # replacement game files
│       │   └── cheats/
│       │       └── <build ID>.txt      # cheats for one version of the game
│       └── cheat_<name>.txt            # optional: cheats without a mod folder
├── save-import/                        # optional: saves to import, one folder per title ID
├── ryujinx/                            # optional: a Ryujinx data folder to import saves from
├── save-export/                        # written by "Export a copy"
└── .remote-downloads/                  # downloads that are not finished yet (see Download sources)
```

The folder browser shows how many keys, firmware files, and games each folder holds, so you can check a folder before selecting it. Moving your files later only needs a new selection in Settings; saved settings, covers, and recently played games carry over.

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
