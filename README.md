<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="ProsperoEden icon">
</p>

<h1 align="center">ProsperoEden</h1>

<p align="center">
  <strong>An unofficial Eden emulator port for PlayStation 5 homebrew</strong>
</p>

**ProsperoEden is an unofficial PlayStation 5 port of [Eden](https://github.com/eden-emulator/mirror)** - an accurate, high-performance emulator. All credit for the emulator core belongs to the Eden project and its contributors. ProsperoEden is not affiliated with or endorsed by the Eden team or Sony.

This is an early alpha. Video, audio, controller input, and saves have been confirmed working. Compatibility and performance will vary between games. The current release is **v1.000.100**.

## Source code

The complete ProsperoEden source is in this repository: the PS5 frontend and launcher in `headless/`, and the build and packaging tools in `tools/`. To build it yourself, run `make` on Linux (Ubuntu 26.04; WSL works). It fetches every dependency at its pinned revision and writes the release files to `dist/`; `make help` lists the other targets. See [docs/BUILDING.md](docs/BUILDING.md). A release ZIP built by GitHub Actions can be checked with `gh attestation verify <ZIP> -R blackbearreloaded/ProsperoEden` (GitHub CLI); this covers releases built from now on (after v1.000.090), not earlier ones.

Work on local-wireless room multiplayer is experimental. See the [setup guide and current
limitations](docs/MULTIPLAYER.md) and [validation status](docs/MULTIPLAYER_IMPLEMENTATION.md);
PS5 gameplay and Tailscale forwarding are not yet qualified.

## Project foundation

> [!IMPORTANT]
> **Built on the [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), the same native foundation used by ProsperoLight.**
> It provides the native PS5 application structure, runtime, packaging, and homebrew deployment foundation.

> [!IMPORTANT]
> **Graphics are powered by [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl).**
> This OpenGL implementation provides the native PS5 rendering layer used by the Eden graphics backend.

> [!IMPORTANT]
> **Vulkan is powered by Mihawk's [PS5 Mesa](https://github.com/mihawk-99/PS5_Mesa) and [PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan).**
> Mihawk's Mesa/RADV driver for the PS5 runs ProsperoEden's Vulkan renderer. Many thanks to Mihawk for this work and for [all of the PS5 projects](https://github.com/mihawk-99) behind it.

> [!IMPORTANT]
> **Thanks to [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) by mpereiraesaa**, an experimental Vulkan graphics and compute API for native PS5 homebrew.

## Features

- **Vulkan renderer (recommended)** - the default backend, running on Mihawk's PS5 Mesa (RADV) driver.
- **OpenGL renderer** - still available through ps5-opengl. Switch between them in **Settings > Video**.
- **Resolution and upscaling** - render at 0.5x to 4x of the game's resolution and choose the filter that scales it to your TV (Bilinear, AMD FSR, Bicubic or Nearest) in **Settings > Video**.
- **Output resolution** - the picture is made at 1080p, 1440p or 2160p: **Output resolution** in **Settings > Video**. The menu is drawn at that size too, and the PS5 scales it to your TV.
- **120 Hz output** - on a display that shows 120 Hz, games can run on a 120 Hz output: **Refresh rate** in **Settings > Video**, or in one game's settings. A frame that is a little late is then shown 8 ms later instead of 17 ms, and patches for more than 60 FPS need it. The menu stays at 60 Hz.
- **Frame generation** - between two of a game's own pictures ProsperoEden can show one made up from them, so a 30 FPS game looks like 60: **Frame generation** in **Settings > Video**, or in one game's settings. It needs your own copy of Lossless Scaling; see [Frame generation](#frame-generation).
- **Game files anywhere** - keys, firmware, and games can live in any folder the PS5 can read: internal storage, an M.2 or external drive, or a USB device.
- **Folder browser** - pick the game files folder in **Settings > Game files**. It shows how many keys, firmware files, and games each folder holds. Hold L1/R1 to page quickly.
- **Library** - game covers, **Continue Playing**, and **Recently Played**, which keep working after you move your files.
- **Launcher** - an animated interface drawn with OpenGL, with sound effects (their level is in **Settings > Audio**) and a loading screen while a game starts. The home screen shows which controllers are connected.
- **Profiles** - everyone who plays has their own save data, settings and recently played games: **Settings > Profiles**; see [Profiles](#profiles).
- **Settings per game** - a game can differ from Settings in its video, performance, audio, controls and language, and has its own Handheld / Docked mode (Triangle in the Library); see [Settings per game](#settings-per-game).
- **Button mapping** - choose which DualSense button presses each of the game's buttons, for every controller, in **Settings > Controls**, or for one game in its settings; see [Button mapping](#button-mapping).
- **Your language** - the launcher follows the language the PS5 is set to (29 languages; English otherwise).
- **Accessibility** - larger text, high contrast and reduced motion, in **Settings > Accessibility**.
- **Save data in and out** - import a game's save from a folder or from a Ryujinx data folder, and export a copy (Triangle in the Library, then **Save data**).
- **Game updates and DLC** - put update and DLC files (NSP or XCI) in the `updates` folder next to `roms`. They apply when the game starts, and each game's details show the update version and DLC count.
- **Download sources** - the games on servers in your network, such as a [RomM](https://github.com/rommapp/romm) server, appear in the Library next to the ones on the console. A game is downloaded when you play it, or ahead of time through a download queue, and then runs from the console like any other: nothing is streamed. Downloads need an FTP server running on the console; see [Download sources](#download-sources).
- **Save sync** - each profile can keep its save data on a server, such as a [RomM](https://github.com/rommapp/romm) server, as Steam does: it is synced before a game starts and after it ended, and when it changed on two devices you choose which one stays. A profile is paired with its server by a QR code, no typing. See [Save sync](#save-sync).
- **Mods** - patches, replacement game files and cheats for a game, from a `mods` folder next to `roms`, each switched on or off in the game's settings (Triangle in the Library, then **Mods**).
- **Performance switches** - seven switches that trade accuracy for speed (compiling a game's code ahead, asynchronous shaders, faster GPU, CPU and DMA emulation, and more) in **Settings > Performance**; see [Performance settings](#performance-settings).
- **Shader cache** - shaders compiled in earlier sessions are loaded when a game starts, so an effect stutters only the first time it appears.
- **In-game shortcuts** - a performance overlay (Touchpad + R1), and Touchpad + L1 to end the game and return to the library. Leaving takes ten seconds at most, also while a game is still loading.
- **Settings in one place** - a single JSON file under `/data/prosperoeden`, with game volume, mute, and detailed logging options. Logs keep the previous session.
- **Updates from the menu** - when a newer release is listed on [homebrew.page](https://homebrew.page/ps5), ProsperoEden offers to install it each time it opens; **Skip** keeps the current version; see [Updating](#updating).
- **Crash reports** - if ProsperoEden stops because of an error, it saves a report with that session's logs, starts again and shows where the report is.
- **Controllers, audio, and saves** - up to four DualSense controllers (one per signed-in PS5 user) with rumble and motion controls, game audio, and save data work out of the box. Games that have a single Joy-Con held sideways are played with the DualSense held as usual: the stick, the buttons and the motion sensors are turned to match.
- **Controller type per game** - some games take a Pro Controller and then only work with another controller. In **Library > Game settings > Controls**, **Controller type** gives the game a Pro Controller, the Handheld controller, Dual Joy-Cons, a Left Joy-Con or a Right Joy-Con. Left on **Automatic**, the game gets what it takes, a Pro Controller first.
- **Typing text** - when a game asks for text (a character's name, an answer during an event), the PS5's own on-screen keyboard opens, and what you enter goes to the game. A USB or Bluetooth keyboard the PS5 supports types into it too.

## Install

> [!TIP]
> **Use [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) or newer.** It mounts ProsperoEden from the folder (or drive) you copy it to, and it also mounts `/data` and USB and extended storage drives into the app's sandbox, so ProsperoEden reaches its data and your game files with nothing else to load. With an older ShadowMountPlus, ProsperoEden falls back to its bundled [Lapy](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon) helper (see [Elevation credits](#elevation-credits)).

1. Download and extract the release ZIP.
2. Copy the included `PPSA99008` folder to `/data/homebrew/PPSA99008` on the PS5 (or to another folder ShadowMountPlus scans, such as `/mnt/usb0/homebrew/PPSA99008`).
3. Make sure your jailbreak environment's local ELF loader is listening on TCP port 9021 (most setups have it). [Updates from the menu](#updating) use it, and with a ShadowMountPlus older than 1.7beta4 ProsperoEden sends its bundled Lapy helper through it.
4. Put your own legally dumped keys, firmware, and games in a **game files folder** (layout below). It can be anywhere the PS5 can read: internal storage, an M.2 or external drive, or a USB device.
5. Launch **ProsperoEden**, open **Settings > Game files**, browse to that folder and select it. The default is `/data/prosperoeden`.
6. Close and reopen ProsperoEden, then open **Library**. Setup is checked when the app opens, so reopen it after changing the folder or adding keys or firmware.

### Game files folder

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

### App data

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

### Updating

Close ProsperoEden first. Your settings, saves, covers and logs are in `/data/prosperoeden`, outside the app, so an update keeps them. Afterwards the About screen shows the version that is running.

Once each time it opens, ProsperoEden asks [homebrew.page](https://homebrew.page/ps5) which release of it is listed there. The request carries the app's title ID and nothing else. If the listed release is newer than the one running, a dialog offers it, with its version and download size:

- **Update now** downloads the release ZIP from its GitHub release, checks it against the SHA-256 in the catalog's signed list, and unpacks it beside the app. A ring shows how far it is and the time left; Circle cancels, and nothing has changed until the end. Then ProsperoEden closes, the update helper (`self-updater.elf`, sent to the console's payload loader on port 9021) replaces the app's files, and the console shows a notification. Open ProsperoEden again to use the new version. The version it replaced is kept; see [Going back to the previous version](#going-back-to-the-previous-version).
- **What's new** (when the release has notes) shows what changes in it before you decide: the release notes as homebrew.page lists them. Up/Down scroll, L1/R1 jump a page, Cross updates and Circle goes back to the offer. Triangle opens them from the offer too.
- **Skip** keeps the version you have. The dialog shows again the next time ProsperoEden opens, not when you come back from a game.

If the download or the unpacking fails, the dialog says why and offers **Try again**; the app stays as it was. Without a network, or without an answer, nothing is shown. When ProsperoEden cannot install the release itself (no payload loader, or an install it cannot find), the menu says **Update available** at the top right for ten seconds instead, and the steps below still work:

- **Folder install.** Copy the `PPSA99008` folder from the new release ZIP over `/data/homebrew/PPSA99008`, replacing the files it has, then start ProsperoEden. Files you put there yourself, such as `language.txt`, stay.

### Going back to the previous version

When ProsperoEden updates itself, the files of the version it replaces are kept in `/data/prosperoeden/previous-version/PPSA99008-backup`, until the next update replaces them in turn. If a new version misbehaves, you can put the previous one back:

1. Close ProsperoEden.
2. Copy everything inside `/data/prosperoeden/previous-version/PPSA99008-backup` over the app's folder (`/data/homebrew/PPSA99008`), replacing the files it has, for example over FTP.
3. Start ProsperoEden. The About screen shows the version that is running.

The update will be offered again the next time ProsperoEden opens; choose **Skip** to stay on the version you put back. Settings, saves and game files are not part of this: they stay as they are. An update done by hand, by copying a release over the app's folder, keeps no previous version.

### Moving save data

Emulators like Eden keep a save as the files the game wrote, so nothing is converted: folders are copied. In the Library, press Triangle on a game and pick **Save data**. The folders below are next to `roms/` in the game files folder.

- **Import from a folder.** Copy the contents of the game's save folder (what an emulator opens as the game's save directory) into `save-import/<title ID>/`. The title ID is the 16-character code in the game's file name; Save data also shows it when there is nothing to import.
- **Import from Ryujinx.** Copy Ryujinx's data folder (the one that holds `bis/`, or a portable folder around it) to `ryujinx/`. ProsperoEden finds the game's save in it: the first user's, and the device save when there is one.
- **Export.** Square writes `save-export/<title ID>-<date>-<time>/`, with `account/` and `device/` inside. An exported folder can be imported again: put it in `save-import/` under the title ID.

Cross imports, and asks before it replaces a save. The save it replaces is first moved to `/data/prosperoeden/backup/save-import`, so nothing is lost.

### Mods

A mod changes a game: a patch to its code (`.pchtxt` or `.ips` files in an `exefs` folder), replacement game files (a `romfs` folder), or cheats (a `cheats` folder). Mods made for other emulators of the same console come in this layout.

- **Add a mod.** Each mod is a folder. Copy it to `mods/<title ID>/`, next to `roms/` in the game files folder, so that a patch ends up at `mods/<title ID>/<mod name>/exefs/<file>.pchtxt`. The title ID is the 16-character code in the game's file name. The Mods screen names the exact folder, and Square creates it. The About screen shows where the `mods` folder is.
- **Add cheats.** Cheats are text files in the layout that cheat collections for other emulators of the same console use: each cheat starts with its name in square brackets (`[Infinite health]`), followed by its code lines. Put the file at `mods/<title ID>/<mod name>/cheats/<build ID>.txt`, the same place as a mod (any mod folder name; a collection's own folder works as it is). A single cheats file can also go straight into the title's folder as `mods/<title ID>/cheat_<name>.txt`; it is listed as its own mod. The build ID is the code the file is named after in cheat collections: each version of a game has its own, so pick the file for the version you run (with its update in `updates/`). Folder names may be in any letter case.
- **Switch it on or off.** In the Library, press Triangle on the game and pick **Mods**. Every mod found is listed with a switch. A mod is on unless you switch it off, and a change applies the next time the game starts.
- **Switch all of a game's mods off or on.** In the Library, a game that has mods shows a **Mods** switch under its console mode; Square flips it. Off, the game starts without any of its mods, and each mod keeps its own switch for when you turn it back on.
- **Choose cheats one by one.** A cheats file can hold many cheats, as the collections made for a game do. When a mod lists more than one, the Mods screen shows each cheat under the mod with its own switch, and none of them runs until you switch it on. Cheats that replace each other, such as two frame rates or two resolutions, take each other's place: switching one on switches the other off. A mod with a single cheat runs it whenever the mod is on.
- **See what a game has.** The home screen and the Library count a game's mods next to its update and DLC (`Update 1.2.0, 2 DLC, 1 mod`), and say so when some are switched off (`1 of 2 mods on`).
- **Match the game's version.** A patch is made for one version of a game. One made for another version is ignored without a message, so check that the mod matches the update you have in `updates/`.
- **Frame rate.** A patch that makes a 30 FPS game run at 60 FPS works on the 60 Hz output ProsperoEden uses. A patch for more than 60 FPS needs the 120 Hz output: set **Refresh rate** to 120 Hz in the game's settings (Triangle in the Library) or in **Settings > Video**. That takes a display that shows 120 Hz and the PS5's own 120 Hz output setting; without them the game runs at 60 Hz. A 60 FPS patch can gain from it too: a game that misses some frames at 60 Hz has twice as many chances to show them. A patch for more frames than the output shows (240 FPS on the 120 Hz output, 120 FPS on the 60 Hz one) still runs at its own pace: the frames the display has no refresh for are left out.

ProsperoEden does not include or download mods.

### Quick start with RomM

Download sources and save sync both work with a [RomM](https://github.com/rommapp/romm) server in your network (5.0.0 or newer for the save sync). In short:

1. **Put the games on the server.** Games go to RomM's library under `roms/switch/`: a game as one `.nsp` or `.xci` file, or as a folder with the game's file, its updates in `update/` and its DLC in `dlc/`. Then start a scan in RomM (its **Scan** page) so it lists them.
2. **One RomM user per person.** Each person who plays on the console gets a user of their own in RomM; their save data stays apart on the server.
3. **Downloads.** In RomM, create a client API token (your profile, **Client API tokens**) with the scopes `platforms.read` and `roms.read`. Write `/data/prosperoeden/config/remote/sources.json` over FTP with the server's address and that token (see [Download sources](#download-sources)), start an FTP server on the console on port 2121 (ftpsrv), and open **Settings > Downloads**: the server and its games show up, and the Library lists them.
4. **Save sync.** Open **Settings > Save sync**, Cross on a profile and choose the server. Scan the QR code with your phone, sign in to RomM as that profile's user and approve the code. From then on the profile's save data is synced before and after each game (see [Save sync](#save-sync)). Pairing uses the server from `sources.json`; without download sources, fill in `save-sync.json` over FTP instead. The save sync does not need the FTP server.

### Download sources

A download source is a server in your network that has games, such as a [RomM](https://github.com/rommapp/romm) server. Its games show up in the Library without taking any space on the console. A game is downloaded to the game files folder when you want it; from then on it is a game on the console like one you copied there. Nothing is streamed. **Downloads need an FTP server running on the console** (port 2121, see below): it writes the downloaded files.

- **Set them up.** Make sure an FTP server runs on the console, then save `/data/prosperoeden/config/remote/sources.json` on the console (over FTP, like the keys) and open **Settings > Downloads**, or reopen ProsperoEden. It lists the sources, each with its `"type"`, a `"name"` for the menu and what that type needs (see [RomM](#romm) for a RomM server):

  ```json
  { "sources": [
      { "type": "romm", "name": "Home", "url": "http://192.168.1.20:3000", "token": "rmm_..." },
      { "type": "romm", "name": "Office", "url": "https://games.example.org", "token": "rmm_..." }
  ] }
  ```

  **Settings > Downloads** shows each source, whether it answered, and why not when it did not, and the FTP server's port. Without a source it shows what the file is called, with an example for a RomM server. When you ask it to read the lists again (Cross on a source), it says when they are in.
- **The Library.** The sources' games are listed with the console's own, by name, with their cover. A game that is not on the console has a darker cover with a cloud, and instead of NSP or XCI its row names its source (or how many have it), or says **Queued**, how far its download is, or **Download failed**. Its details name its sources. The same game on several sources is listed once, and a game that is on the console already is not listed again. Its settings, mods and console mode are there once it is downloaded.
- **Which game is which.** Each source says what it knows of a game: its title ID, its ids at metadata providers (IGDB, ScreenScraper and the like) and whether its name comes from such metadata. Two sources' games are the same game when the first of these that both know agrees: the title ID, then an id at a provider both have one of, then the name (when both names come from metadata; case, accents and punctuation do not count), else the file name. A game on the console is the source's game when it has the file name the source gives it (a downloaded one does), else when their title IDs agree (when the source knows one), else by its name.
- **Play.** Cross on a game that is not on the console downloads it first. A ring shows how far it is and the time left, and the game starts once all of it is there. Circle lets it download in the background without starting it; Square cancels it. When several sources have the game, ProsperoEden asks which one first.
- **Download ahead.** Square on such a game puts it in the download queue, and Square again takes it out. The queue downloads one game at a time while the menu is open, also after ProsperoEden was closed and opened again. **Settings > Downloads** lists it: Square cancels a download, Cross tries a failed one again, and Cross on a source reads the sources' game lists again (also done by itself when the menu opens after more than 15 minutes).
- **Where the files go.** The game's file goes to `roms/`, under its name on the source; its updates and DLC go to `updates/`. What a source counts as the game, an update or DLC is up to it (a RomM server: its file categories). A game downloaded from a source keeps the file name it has there. Only `.nsp` and `.xci` files are downloaded, and an entry without a game file (an update or DLC on its own) is not listed.
- **An FTP server on the console.** Downloads are written by an FTP server running on the console (such as [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv), which most setups have for copying the keys), on port 2121: ProsperoEden hands it the bytes over loopback. Written by the app itself, a game of several gigabytes slowed the drive down to a few MB/s and stopped the menu; from a payload the drive keeps its speed. Without such a server a download fails and says so. `"ftp_port"` next to `"sources"` names another port (etaHEN's own FTP server, for example), and `"ftp_user"` and `"ftp_password"` a sign-in when the server asks for one (else `anonymous`). The server must support `REST` with `STOR`, to go on with a download.
- **Interrupted downloads.** A download is written to `.remote-downloads/` in the game files folder, and once all of the game's files are complete they are moved to `roms/` and `updates/`, so those folders only ever hold whole files of whole games. It stops while a game runs and when ProsperoEden closes, and goes on from where it was the next time the menu is open; the last 4 MB are downloaded again then, in case the console lost power while they were written. A cancel deletes what was downloaded (and never touches `roms/` or `updates/`), and what `.remote-downloads/` holds for a game that is not in the queue is deleted when the menu opens, so nothing is left behind. When the drive is full, the download stops and says so, as the FTP server reports it ("No space left on device"); nothing reaches `roms/` or `updates/`, what was downloaded stays in `.remote-downloads/`, and once there is room, Cross on it in **Settings > Downloads** goes on from there (Square cancels it and deletes it). The drive's free space is not checked before a download starts: an app on the console cannot ask for it.
- **Checked as it comes.** RomM does not hash these files, so a download is checked on its way in, as Eden's integrity check checks a game: each NCA of an NSP or XCI against the SHA-256 it is named after, with no keys needed. A file found damaged is deleted and the download fails saying so; trying again downloads it again. A download that goes on reads what it had from the drive first (**Checking** in the queue). What the console's FTP server writes is not read back, and files that are no NSP or XCI are taken as they are.
- **Making room.** A game that a source has can be deleted from the console again: Triangle in the Library, then **Delete from console** (Cross twice). Its file and all of its updates and DLC in `updates/` are deleted, also ones put there by hand; its save data, its settings and its mods stay. The Library then lists it as on its sources again, and it can be downloaded any time.

Sign-ins are kept in `sources.json` only, and each is sent to its own source only. They are stored there as plain text, and whoever can reach the console's FTP server can read the file: give a source a token made for it, with only the scopes it needs, rather than your password. A file that is on the console already is never replaced by a download of the same name. Download sources need filesystem access (the payload loader on port 9021), as the game files folder does, and an FTP server on the console (see above). Save data is synced apart from the sources; see [Save sync](#save-sync).

#### RomM

`"type": "romm"` is a [RomM](https://github.com/rommapp/romm) server (a current version); as with any source, its games are written by the console's FTP server, which has to run. Its entry in `sources.json` has the server's `"url"` and a `"token"`: in RomM, create a client API token (your profile, **Client API tokens**) with the scopes `platforms.read` and `roms.read`. `"username"` and `"password"` work instead of `"token"`, and `"platform"` names the platform's slug when the server does not call it `switch`. RomM's file categories decide what is the game and what are its updates and DLC; manuals, mods, soundtracks and the like stay on the server. In RomM a game is a folder, with its updates in `update/` and its DLC in `dlc/`. What RomM matched a game to tells it apart: its ids at the metadata providers RomM uses, its name when RomM identified it, and its title ID when the RomM server has the keys to read it.

### Save sync

Each profile can keep its save data on a server, such as a [RomM](https://github.com/rommapp/romm) server (5.0.0 or newer), the way Steam keeps it in the cloud: before a game starts, its save data is put in step with the server's, and after the game ended the new save data goes to the server. Played on another device in between (Eden on Android with [Argosy](https://github.com/rommapp/argosy-launcher), or another console), the newer save data comes to the console; changed on both sides, ProsperoEden asks which one stays. Save sync is set up for each profile and does not depend on the download sources: games copied to the console by hand sync too, as long as the server has the game.

- **Pair a profile, without typing.** **Settings > Save sync** lists the profiles and the server each one keeps its save data on. Cross on a profile pairs it with one of the download sources' servers (asked first when several can): the dialog shows a QR code and a short code. Scan it with your phone (or open the address under it), sign in to the server as the user this profile's save data belongs to, check that the code is the same and approve. The console gets a sign-in of its own for that user and writes it into the profile's entry; the code lasts ten minutes. Square, pressed twice, unlinks a profile: its save data stays on the console and on the server. Each profile can be paired with a user of its own, so two people's save data stay apart. Pairing needs the server among the download sources (`sources.json`); without one, set it up over FTP as below.
- **Or set it up over FTP.** ProsperoEden writes `/data/prosperoeden/config/remote/save-sync.json` with an entry for every profile, and keeps it in step with the profiles: a new profile gets an empty entry, a removed one's entry goes, and a renamed one's `"__profile_name"` changes. Fill in the entry of each profile that should sync (over FTP, like the keys), with the server's `"type"`, `"url"` and sign-in (see [RomM](#romm-1) for a RomM server):

  ```json
  { "version": 1, "auto": true,
    "profiles": [
      { "profile": "1F1E1D1C1B1A19181716151413121110", "__profile_name": "Player 1",
        "type": "romm", "url": "http://192.168.1.20:3000", "token": "rmm_..." },
      { "profile": "AFAEADACABAAA9A8A7A6A5A4A3A2A1A0", "__profile_name": "Kids",
        "type": "romm", "url": "http://192.168.1.20:3000", "token": "rmm_..." },
      { "profile": "000102030405060708090A0B0C0D0E0F", "__profile_name": "Guest",
        "type": "", "url": "", "token": "" }
    ] }
  ```

  A paired entry has its `"token"` and a `"__server_user"` (who it signed in as, for the menu). `"profile"` is the profile's ID, which tells the entries apart (a name can change, the ID does not); leave it as it is. Fields that start with `__` are only there to read: ProsperoEden writes them, and what you write into them changes nothing. An entry with an empty `"type"` does not sync. `"auto": false` turns the save sync off for every profile, also for the games still waiting to be synced after they were played. Everything else you write stays as it is, also fields of your own. A file that is not valid JSON is left alone, and no profile syncs until it is fixed (the log says so).
- **One server user for each person.** Give each profile the sign-in of its own user on the server (in RomM: a client API token of that user): the server keeps each user's save data apart, so two people's save data of the same game stay two. Two profiles with the same user would share their save data.
- **Before a game.** Starting a game opens a dialog while its save data is synced; the game starts when it is done. When the server's save data is newer, it replaces the console's. Circle goes back without the game (the sync stops, nothing more changes). When the server cannot be reached, the dialog says why: **Play anyway** (with the console's save data; it is synced after the game), **Try again** or **Back**.
- **After a game.** Once the menu is back, the game's save data goes to the server, and a notice at the top right says so. When that fails (no network), it is tried again each time the menu opens, until it worked.
- **Changed on both sides.** When the console and the server both have save data the other does not know (played on two devices without a sync between), a dialog asks which one stays: **Keep this console's** (it goes to the server), **Take the server's** (it replaces the console's) or **Change nothing** (you are asked again next time). It shows when each was saved, and on which device the server's was.
- **Nothing is lost.** Save data replaced on the console is first moved to `/data/prosperoeden/backup/save-sync/<profile>/<title ID>/<time>/`; the last three of each game are kept. A RomM server keeps the last ten versions of each game's save data as well. To go back to one, copy that folder's contents over the game's save data while ProsperoEden is closed. If ProsperoEden is closed in the middle of a replacement, the console's save data is put back the next time the game syncs.
- **What is synced.** A game's save data of the profile: its folder `user/nand/user/save/0000000000000000/<profile ID>/<title ID>/`, as one zip with that folder at its top, the way Argosy packs Eden's save data (so it goes between the two). Save data a game keeps for the whole console (not for a user) is not synced. The server must have the game: it is found as a download source's game is found on the console (by its file name, its title ID, or its name).

#### RomM

`"type": "romm"` keeps the save data on a [RomM](https://github.com/rommapp/romm) server with save sync, RomM 5.0.0 or newer (checked with 5.0.0 to 5.3.1). With an older server nothing is synced, and every sync says so in a dialog to confirm, with the server's version and the one it needs. Paired from Settings > Save sync, RomM's device authorization does the signing in: the code is approved on the server's page `/pair/device`, and RomM makes a device of the console with a client API token bound to it. Set up over FTP instead, its entry in `save-sync.json` has the server's `"url"` and a `"token"` of the profile's own RomM user: in RomM, create a client API token (your profile, **Client API tokens**) with the scopes `platforms.read`, `roms.read`, `assets.read`, `assets.write`, `devices.read` and `devices.write` (and `me.read`, for the log to name the user); a scope that is missing is named when a sync fails. `"username"` and `"password"` work instead of `"token"`, and `"platform"` names the platform's slug when the server does not call it `switch`. The console is registered once as a device of that user, **ProsperoEden (PS5)**, and RomM knows from it which version of each game's save data the console last had. A game's save data is in its slot `autosave`, for the emulator `eden`, as Argosy keeps Eden's; the game's other slots and other emulators' save data are left alone. Which side changed is told by the save data itself, not by the times RomM compares: the console keeps what it last had in step (`config/remote/save-sync/<profile>/synced.json`). Save data deleted in RomM is uploaded again by the console that still has it.

### Performance settings

**Settings > Performance** trades accuracy for speed, for every game. A change applies when a game starts.

| Setting | In the file | Default | What it does |
|---|---|---|---|
| Compile ahead | `block_list` | off | The code a 64-bit game compiled in earlier sessions is compiled again on a spare CPU as it starts, so the game stutters less when it reaches that code. In the one large game it was measured in, the game loaded sooner and its first seconds of play held their frame rate, while the emulated processor worked about a tenth harder afterwards. It has run in one game on the console so far; switch it off if a game misbehaves. |
| Asynchronous shaders | `async_shaders` | off | A new effect is drawn once its shader is ready instead of pausing the game for it. Things can be missing for a moment. With the Vulkan renderer only: the OpenGL renderer ignores it. |
| Faster GPU emulation | `fast_gpu` | off | The emulator's lowest GPU accuracy. Faster in demanding games; graphics can be wrong. |
| Faster CPU emulation | `unsafe_cpu` | off | Less exact floating-point math. A few games misbehave. |
| Faster DMA | `unsafe_dma` | off | Less exact memory transfers to the GPU. A few games show wrong graphics. |
| Reactive flushing | `reactive_flushing` | on | Keeps what a game reads back from the GPU exact. Off is faster; some effects break. |
| Skip CPU invalidation | `skip_invalidation` | off | Skips some checks when a game changes memory the GPU uses. Textures can be stale. |

A game can have its own values: in its settings, **Performance** (see [Settings per game](#settings-per-game)). In the settings file, `"performance"` at the top level is what the switches set, and one inside a game's entry under `"games"` is that game's, whose values go first:

```json
{
  "performance": { "block_list": false, "async_shaders": false, "fast_gpu": false, "unsafe_cpu": false,
                   "unsafe_dma": false, "reactive_flushing": true, "skip_invalidation": false },
  "games": { "0100000000010000": { "performance": { "fast_gpu": true } } }
}
```

The accuracy switches help only where a game is held back by what they relax. In the scene they were measured in on the console, a large game standing at its frame rate limit, switching all of them on changed neither the frame rate nor how busy the emulated processor and the graphics thread were. Leave them off unless a game runs under its frame rate, and switch them back off if its graphics go wrong.

Resolution, the upscaling filter, the renderer and the refresh rate, which change speed too, are in **Settings > Video** and in a game's own settings.

### Frame generation

Frame generation shows, between two pictures a game draws, one that is made up from them. A game that runs at 30 FPS then looks like 60. It does not make the game itself faster, it adds a little delay between a button and its effect, and fast motion can show faults in the made-up pictures. It works with the Vulkan renderer.

It uses the frame generation of [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/), which ProsperoEden cannot include: you need your own copy of that program.

1. On the PC where Lossless Scaling is installed, find `Lossless.dll` in its folder (with Steam: `steamapps/common/Lossless Scaling`).
2. Copy it to the console as `/data/prosperoeden/user/lossless/Lossless.dll`.
3. Set **Frame generation** to on in **Settings > Video**, or for one game: Triangle on the game in the Library, then **Video**. It applies the next time the game starts.

Without the file, the three settings are greyed out and say **Unavailable**.

| Setting | What it does |
|---|---|
| Frame generation | On or off |
| Frame gen target | The rate to reach: as many pictures are added as it takes. **60 Hz** suits a 30 FPS game on the usual output; more than 60 needs **Refresh rate** at 120 Hz and a display that shows it. **Auto** leaves it to the multiplier |
| Frame gen multiplier | With the target on Auto: 2x, 3x or 4x the game's own pictures |

A game that already fills the output, such as one at 60 FPS on the 60 Hz output, gains nothing from it. When it does not run, the log of the session says why, in the lines that start with `framegen:`.

### Profiles

**Settings > Profiles** lists the people who play on this console, up to eight. Each profile keeps its own save data, its own settings (everything under Settings, and each game's own settings) and its own Continue Playing and Recently Played. The game files folder is the console's and is the same for everyone.

- **Cross** plays as the highlighted profile. The home screen names who is playing once there is more than one profile.
- **New profile** adds one. It starts with the settings of the profile that made it, and with no save data.
- **Left and right** change a profile's name: the names of the PS5 users signed in, then "Player 1" to "Player 8".
- **Square**, pressed twice, takes a profile off the list. Its save data stays on the console.

Each profile can keep its save data on a server of its own; see [Save sync](#save-sync).

The menu opens with the profile that the PS5 user in front chose last. If you used an earlier version, your saves, settings and recently played games are the first profile's, where they always were.

### Settings per game

**Settings** holds what every game uses. A game can differ from it: in the Library, press Triangle on the game. Its settings list **Video**, **Performance**, **Audio**, **Controls** and **Language**, and each one says how many of its settings the game changes. Inside, a setting shows **Default** with the value Settings has until you choose another one for this game; going past the last choice comes back to **Default**. A change applies the next time the game starts.

| Kind | What a game can have of its own |
|---|---|
| Video | Renderer, resolution, upscaling filter, refresh rate, FPS overlay, frame generation with its target and multiplier |
| Performance | Each of the seven switches |
| Audio | Game volume, mute |
| Controls | Vibration, and a button mapping of its own |
| Language | The language the game is given |

The output resolution, the menu's sounds and the accessibility settings are the same for every game.

### Button mapping

**Settings > Controls > Button mapping** lists the game's buttons (A, B, X, Y, L, R, ZL, ZR, Plus, Minus and the two stick presses) with the DualSense button that presses each one. Left and right choose another DualSense button; when another game button had it, the two swap, so one DualSense button never presses two game buttons. Square puts every button back. The mapping applies to every controller.

A game can have its own: in its settings, **Controls**, set **Button mapping** to **This game**, then press Cross to change it. It starts as a copy of the one in Settings.

The D-pad, the sticks and the [in-game shortcuts](#in-game-shortcuts) do not change. With the usual mapping the touchpad is the game's Minus; the Create button presses whatever the touchpad presses, unless it has a game button of its own.

### Language and accessibility

The launcher follows the language the PS5 is set to: Arabic, Chinese (simplified and traditional), Czech, Danish, Dutch, Finnish, French, German, Greek, Hungarian, Indonesian, Italian, Japanese, Korean, Norwegian, Polish, Portuguese, Romanian, Russian, Spanish, Swedish, Thai, Turkish, Ukrainian and Vietnamese (with the regional variants the PS5 has for French, Portuguese and Spanish), and English otherwise. Arabic, Chinese, Greek, Japanese, Korean and Thai are drawn with the PS5's own system fonts. Arabic text runs right to left; the screens themselves are not mirrored. To use another one, put a file named `language.txt` holding its tag (for example `en-US` or `pt-BR`) in `/data/homebrew/PPSA99008`. The language *games* use is a separate setting, **Settings > Language**.

**Settings > Accessibility** has three switches. **Larger text** draws the menu's small text about a third larger. **High contrast** uses solid panels, brighter text and an outlined highlight. **Reduce motion** stops the background drifting and the screens sliding, in the menu and on the loading screen. There is no screen reader.

### Upgrading from an earlier alpha

Earlier versions read everything from `/data/homebrew/PPSA99008/assets/`. That folder keeps working until you choose a game files folder, and settings are migrated automatically on first launch. To move to the new layout, move `assets/keys`, `assets/firmware` and `assets/roms` into any folder, then select it in **Settings > Game files**. The release ZIP contains no user files, so copy its app files over your installation without deleting your own data.

## Changes in v1.000.100

- **Frame generation (new).** Between two pictures a game draws, ProsperoEden can show one made up from them, so a 30 FPS game looks like 60. It is in **Settings > Video** and in each game's own settings, works with the Vulkan renderer, and needs your own copy of Lossless Scaling: without its `Lossless.dll` the settings are greyed out. See [Frame generation](#frame-generation). Contributed by [gyn7561](https://github.com/gyn7561).
- **A new loading screen.** It lists the steps of a game's start (reading the game, starting graphics, building shaders, starting the game) with a progress bar; while shaders are built it counts them. A step that lasts gets its seconds shown, so a start that is stuck can be told from one that is slow.
- **Cheats in a file of their own work.** A cheat file placed straight in a game's mods folder (`cheat_<name>.txt`) was listed and switched on, but changed nothing in the game. It does now. The session log also names the game's build ID, the name a cheat file for that version of the game has.
- **A game no longer fails every start after a crash.** A crash, or a console that went down, could leave a game's shader cache file damaged; that game then crashed at every start on the same renderer. The damaged part is now cut off and the rest is kept.
- **No more stutter from log writes.** Games dropped frames every few seconds while the logs were written. Logs are now written in the background once a second; **Settings > Diagnostics > Write logs at once** brings the old behaviour back, to find a crash. Contributed by [matschi95](https://github.com/matschi95).
- **The FPS overlay with frame generation.** It shows the game's own rate (N) beside the rate on screen (F) while frames are being added, and its usual single figure otherwise. Contributed by [gyn7561](https://github.com/gyn7561).
- **The log says why frame generation is not running**, in lines that start with `framegen:`. Contributed by [gyn7561](https://github.com/gyn7561).
- **A Nix shell for building.** `shell.nix` sets up the whole build environment. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Issues are open on GitHub**, with forms for bug reports and game reports; they ask for the save data and the debug logs. See [Issues and reports](#issues-and-reports).

## Changes in v1.000.095

A bug fix for v1.000.090. If you are on v1.000.090, update.

- **Stutter and crashes while playing are fixed.** In v1.000.090 every button press could make a game bring up its controller screen again: play stuttered, and some games crashed after a minute or two. A second, handheld controller was being connected beside the player's own at the first press. It is now only used when **Handheld** is the game's Controller type, or the only controller the game takes.

## Changes in v1.000.090

- **Download sources (new, early).** Games on a [RomM](https://github.com/rommapp/romm) server in your network appear in the Library and are downloaded to the console when you want to play them; see [Download sources](#download-sources). Contributed by [matschi95](https://github.com/matschi95).
- **Save sync (new, early).** Each profile can keep its save data in step with a RomM server, synced before a game starts and after it ends, and paired by scanning a QR code; see [Save sync](#save-sync). Replaced save data is kept as a backup first. Contributed by [matschi95](https://github.com/matschi95).
- **Controller type per game.** **Library > Game settings > Controls > Controller type** gives a game a Pro Controller, the Handheld controller, Dual Joy-Cons, a Left Joy-Con or a Right Joy-Con, for games that take a Pro Controller and then only work with another.
- **Typing text.** When a game asks for text, the PS5's own on-screen keyboard opens, and what you enter goes to the game.
- **Firmware 7.40 and 7.61: games no longer end at their first picture.** Contributed by [v0ltfault](https://github.com/v0ltfault).
- **Game names without question marks.** A name with a look-alike character (a sequel's Roman numeral, full-width letters) is shown with plain letters, a name that is not text is skipped, and characters no font has are left out.
- **System language in the start-up trace**, with the menu's language and the console fonts found, for reports from other consoles.
- **Builds on GitHub.** Releases are now built by GitHub Actions from the tagged source.

## Changes in v1.000.080

- **Single Joy-Con games with the DualSense held as usual.** A game that has a single Joy-Con held sideways no longer needs the DualSense turned sideways: the stick, the four buttons and the motion sensors are turned to match, for each player. L1 and R1 stay SL and SR.
- **What's new in the update dialog.** When an update is offered, **What's new** shows that release's notes before you decide.
- **Start a game directly.** ProsperoEden takes `--rom <file>` and `--exit-after-game` as launch arguments, so a home screen forwarder can open one game; see [docs/FORWARDER.md](docs/FORWARDER.md).
- **Filesystem access on firmware 13.60.** The bundled helper is updated with a fix for that firmware.
- **Release ZIP opens with the right permissions.** Its files are stored open to all, as the console wants an app's files; unpacked with their permissions kept, they could give "Can't start the game or app" (CE-107750-0).

## Changes in v1.000.070

- **ProsperoEden starts wherever it is installed.** On a USB drive, extended storage, an etaHEN games folder or any other place ShadowMountPlus mounts it from, it could not find its own files and closed after a moment. It now reads its files from where the PS5 mounts the running app.
- **Updates from the menu work on those installs too.** The update helper updates the folder ShadowMountPlus mounted the app from; an app installed as an image is not changed, and the dialog says to replace the image.
- **Clearer error when the menu cannot start**, naming the file and the reason.
- **Start-up trace for bug reports.** Each start-up step is written to the kernel log (`[ProsperoEden diag]`) and to `/data/prosperoeden/logs/boot-trace.txt`.

## Changes in v1.000.060

- **Updates from the menu.** When a newer release is listed on homebrew.page, ProsperoEden offers it each time it opens. **Update now** downloads and checks it, unpacks it beside the app, then closes so the update helper can replace its files; **Skip** keeps the current version until the next time it opens. Your saves, settings and own files in the app folder are kept. See [Updating](#updating).
- **Lapy elevation included.** ProsperoEden carries its own one-shot helper built from upstream Lapy and sends it over the local ELF loader on port 9021; loading Lapy separately is no longer needed.
- **The elevation helper no longer crashes when ProsperoEden starts.** Its start-up runs on one thread, and it is built from a corrected upstream release.

## Changes in v1.000.050

- **Profiles.** **Settings > Profiles** lists the people who play on the console, up to eight. Each profile keeps its own save data, its own settings (everything under Settings, and each game's own) and its own recently played games. See [Profiles](#profiles). If you update from an earlier version, your saves, settings and games played become the first profile's, with nothing to do.
- **Button mapping.** Choose which DualSense button presses each of the game's buttons, for every controller, in **Settings > Controls**, or for one game in its settings. See [Button mapping](#button-mapping).
- **Settings per game.** A game's settings now have Video, Performance, Audio, Controls and Language; each value follows Settings until you choose another. See [Settings per game](#settings-per-game).
- **Performance settings.** **Settings > Performance** has seven switches that trade accuracy for speed, for every game or for one. See [Performance settings](#performance-settings).
- **Cheats chosen one by one.** A mod that lists several cheats shows each under it with its own switch, and only the chosen ones run. See [Mods](#mods).
- **Update notice.** When a newer release is listed on homebrew.page, the menu says so at the top right for ten seconds. Nothing is downloaded or installed. See [Updating](#updating).
- **Leaving a game takes ten seconds at most.** If a game has not stopped ten seconds after Touchpad + L1, or after it ended by itself, ProsperoEden starts again and opens at the menu. Some games took minutes to close.
- **Touchpad + L1 works while a game loads.** A load that hangs can be left; before, the only way out was closing ProsperoEden.
- **A game's controller screen always gets an answer.** A game that takes only the handheld controller gets it in Docked mode too, and a game that names no controller gets a Pro Controller. Both used to leave the game waiting.
- **Removed games leave the menu.** A game whose file leaves the game files folder leaves Continue Playing, Recently Played and the Library within two seconds, and is not started.
- **Touchpad by its name.** The menu and this document say Touchpad + L1 and Touchpad + R1.
- **The graphics driver's shader cache works again**, so shaders compiled in earlier sessions load when a game starts.
- **A crash when a game with cheats stops is fixed.**
- **Games start a little sooner**: the instruction decoder's tables are built from one reading of each pattern.
- The graphics driver is built from Mihawk's current PS5 Mesa and Vulkan.

## Changes in v1.000.040

- **The launcher in your language.** It follows the language the PS5 is set to, in 29 languages. Arabic, Chinese, Greek, Japanese, Korean and Thai are drawn with the console's own fonts. Text that runs longer in another language makes its own room instead of being cut.
- **Accessibility.** **Settings > Accessibility** adds **Larger text**, **High contrast** and **Reduce motion**. Warnings carry a mark as well as a colour.
- **Save data in and out.** Import a game's save from a folder or from a Ryujinx data folder, and export a copy. See [Moving save data](#moving-save-data).
- **A ten-second slowdown with the AMD FSR filter is fixed.** In a large open-world game, gameplay could start at 3-9 FPS for about ten seconds, and frames of about a tenth of a second kept coming afterwards. The texture cache was throwing away images the GPU had drawn, with a wait for the GPU each time, as soon as memory use passed a mark that the FSR filter's own images pushed it over. It now keeps them until graphics memory is really short.
- **Smoother heavy scenes.** The renderer hands its work to the Vulkan worker in larger batches, and the emulated cores wait less on the GPU caches' locks. In the heaviest area of a test walk this removed drops to 26-28 FPS and cut the GPU thread's work by about a quarter.
- **Touchpad as Minus in games.** A tap of the touchpad presses the game's Minus button, and a longer press holds it. The Touchpad + L1 and Touchpad + R1 shortcuts never reach the game as a press.
- **A crash a few seconds into some games is fixed.** The motion sensors' updates could reach a part of the controller service that was not set up yet.
- **Games' own system screens.** A game's error dialog, profile picker and similar screens use Eden's built-in versions, and its error dialog now answers the game instead of leaving it waiting. The firmware's versions could end a session with an out-of-memory error.
- **More graphics memory.** The PS5 gives an app one pool of memory that the CPU and the GPU share, and ProsperoEden held about 3 GiB of it without using it: its heap took 3 GiB at start and the emulated console's page table 1 GiB. Both now take memory as they need it, which gives graphics about 2.6 GiB more in the largest game tested. That game ran out of graphics memory while loading at 2x; it now runs at 2x with the AMD FSR filter at a steady 30 FPS, with about 2 GiB to spare. The texture cache also measures its memory use from what is really left of the pool, instead of a driver figure that counted every allocation twice.
- **A clearer message when a game runs out of graphics memory.** If a game still needs more than there is at a high resolution, give it its own lower one: Triangle on it in the Library, then **Resolution**.
- **Games that accept only single Joy-Cons** now get one, and L1 and R1 are its SL and SR buttons, which those games ask for on their controller screen.
- **Mods.** A game's patches, replacement files and cheats load from `mods/<title ID>/` next to `roms/`, and the game's settings list them with a switch each. `.ips` and text `.pchtxt` patches, which the emulator core could not apply, now work. The Library has one switch per game for all of its mods. See [Mods](#mods).
- **120 Hz output.** **Refresh rate** in **Settings > Video**, and in each game's settings, asks the display for 120 Hz while a game runs. A display that cannot show it stays at 60 Hz, and the menu always runs at 60 Hz. After a game at 120 Hz the menu takes about five seconds to come back while the display changes rate.
- **Output resolution.** **Settings > Video** has a new **Output resolution** row: 1080p (as before), 1440p or 2160p. It is the size of the picture ProsperoEden puts out, for the menu and for games. Until now a game's picture was made at 1080p and enlarged, whatever its internal resolution; at 2160p the upscaling filter scales the game straight to a 4K picture. A larger picture needs more graphics memory.
- **3x and 4x resolution.** The internal resolution now goes up to 4x. These need far more graphics memory than 2x; a game that runs out says so, and can be given its own lower resolution.
- **Patches for more frames than the display shows.** A game patched to run faster than the output refreshes keeps its pace: the frames the display has no refresh for are left out.
- **Crash reports.** If ProsperoEden stops because of an error, it writes `crash-<date>-<time>.txt` to `/data/prosperoeden/logs`, starts again and says on the home screen where the report is. That session's logs are kept beside it, and the five newest reports stay. A report holds what failed, where in the app's code, and what was running; it never holds the contents of memory.
- The launcher and the OpenGL renderer use the PS5 OpenGL 4.6 SDK 1.0.0.
- The graphics driver's shader cache moved to `/data/prosperoeden/cache`, so a read-only package install keeps it.
- **Experimental: block list.** With an empty file named `block-list.txt` in `/data/homebrew/PPSA99008`, ProsperoEden saves which code a 64-bit game compiled and compiles it again on a spare CPU when the game next starts. In a repeat session of a large open-world game, the emulated cores then compiled 426 blocks during play instead of 138,262, and gameplay started at 30 FPS instead of 22. It is off by default until more games have run with it.

## Changes in v1.000.030

- **New launcher.** Every screen is redrawn with OpenGL: smooth transitions between screens, text that stays sharp, panels that blur the artwork behind them, and sound effects for moving, selecting and going back. **Settings > Audio > Menu sounds** sets their level.
- **Controllers on the home screen.** Four controller icons show which controllers are connected, and change as one joins or leaves.
- **Loading screen.** An animated scene shows while a game starts, on both renderers.
- **The Library opens at once.** The game list is read in the background, and the home screen shows each game's own name instead of its file name.
- **New music** on the PS5 home screen.
- **Smoother first-time gameplay.** The emulated CPU cores now share the code they compile, so each part of a game is compiled once instead of once per core. In large open-world games this halves the compile work and removes most of the stutter when gameplay starts or a new area loads.
- **Faster compiling.** Compiling a game's code now takes about 40% less CPU time, which shortens the remaining stutter when gameplay starts or a new area loads.
- **Startup hang fixed.** A game could stop for good right after starting, because the emulator's memory allocator could leave high-priority threads waiting on each other forever. Development builds also report where a slow start is stuck.
- **Games close in about a second.** Touchpad + L1 used to take 10-25 seconds to return to the library; logging no longer waits on the console's storage, and the emulator skips needless teardown work.
- **Up to four controllers.** Each signed-in PS5 user's controller becomes the next player (player 1 is whoever launched the game); controllers can join or leave during a game, and games that ask for controllers connect every one in use.
- The Touchpad + L1 and Touchpad + R1 shortcuts work from any controller.
- **Vibration and motion.** DualSense rumble for games that use it (turn it off in **Settings > Controls**), and the controller's gyro and accelerometer for motion controls.
- **Resolution and upscaling.** **Settings > Video** now sets the internal rendering resolution (0.5x to 2x) and the filter that scales it to the TV: Bilinear, AMD FSR, Bicubic or Nearest.
- **Per-game settings.** Press Triangle on a game in the Library to give it its own renderer, resolution and upscaling filter.
- **Language setting.** **Settings > Language** picks the language games use (18 languages), with the console region that goes with it. It applies when a game starts.
- **Game updates and DLC.** Put update and DLC files (NSP or XCI) in the `updates` folder next to `roms`; the newest update and all DLC apply when the game starts, and the game details show them.
- **Build it yourself.** `make` fetches every dependency at its pinned revision and builds the release. The OpenGL renderer now uses the published PS5 OpenGL 4.6 SDK 0.6.0.

## Changes in v1.000.020

- **Vulkan is now the recommended and default renderer.** OpenGL remains available in Settings.
- **Choose where your game files live.** The new **Settings > Game files** browser selects any folder, including external drives and USB devices. The old `assets/` folder keeps working until you choose one.
- **App data moved to `/data/prosperoeden`.** Config, logs, covers, and saves now live outside the app folder, so updating the app never touches them.
- **Settings are now stored in one JSON file.** Earlier text settings are migrated automatically on first launch.
- **Faster Library navigation**, and covers and recent games that survive folder moves.
- **New icons** for controller actions, pages, and the Handheld / Docked mode.

**Known issues:** Ending a game with Touchpad + L1 can take several seconds. Some games can still hang on the loading screen, and some demanding games remain slow. This is a testing pre-release, not a compatibility guarantee.

## In-game shortcuts

The touchpad is pressed as a button. On its own, a tap of the touchpad presses the game's Minus button (with the usual [button mapping](#button-mapping)), and a longer press holds it; a shortcut never reaches the game.

| Shortcut | Action |
|---|---|
| Touchpad + R1 | Toggle the performance HUD |
| Touchpad + L1 | End the running game and return to the library |

## Roadmap

- **FPKG support** - install ProsperoEden as a fake package, alongside the current homebrew folder install.
- **More performance** - CPU and GPU work to keep demanding games at their target frame rate: the short stutter when a game reaches new areas (the block list, **Compile ahead** in **Settings > Performance**, once it has run in more games and can be on by default), heavy cutscenes, and games that run slower in Docked mode than in Handheld.
- **More reliable game loading** - a game that hangs while it loads can now be left with Touchpad + L1; the hangs themselves still need a log from a game that does it.
- **Game names in the menu's language** - a game carries a name for each language; the Library shows the first one that is text. Show the one in the menu's language, and the name from an installed update when it has one.
- **Real Joy-Cons and Pro Controllers (investigation)** - play with the original controllers connected to the PS5. The PS5 does not pair them itself, so this first needs to find out whether a homebrew app can read them: a wired Pro Controller over USB looks more likely than Joy-Cons, which only connect over Bluetooth.
- **Keyboard as a controller** - map a USB keyboard's keys to the game's buttons, like the DualSense's. Text entry itself is done: it uses the PS5's on-screen keyboard.
- **Multiplayer with Eden on other systems (investigation)** - join Eden's multiplayer rooms from the PS5 and play with Eden players on PC, Linux and Android, entering a room's address by hand, including a room hosted on your local network (no PSN needed). Rooms only accept the same Eden version on every side, so each release would name the matching PC version.
- **DualShock 4 controllers** - use DualShock 4 controllers as players too, next to the DualSense, so multiplayer games do not need four DualSense controllers.
- **Controller type for each player** - choose, for each of the four connected controllers, which controller the game sees: a Pro Controller, a pair of Joy-Cons, a single left or right Joy-Con, a GameCube controller, or Handheld mode, as Eden does on other systems.
- **Better OpenGL performance** - make the OpenGL renderer faster, and add tuning options for it.
- **More game compatibility** - validate more games on the PS5, and fix what keeps them from running well, such as games that crash at launch.

## Issues and reports

Problems and ideas go to [GitHub issues](https://github.com/blackbearreloaded/ProsperoEden/issues). Reports about a particular game are welcome: name the game, and say what happens and where.

A report that can be acted on has:

- the game and its version (and any update or mod in use);
- the ProsperoEden version, the PS5 model and system software version, and what the console runs (kstuff-lite or etaHEN, ShadowMountPlus version);
- the renderer, resolution and Performance switches;
- the debug logs, attached as a ZIP: turn on **Settings > Diagnostics > Detailed logging**, make the problem happen again, then zip `/data/prosperoeden/logs` (after a crash, the crash report and the logs beside it);
- for a problem at a certain spot in a game, your save data from just before it, attached as a ZIP: Triangle on the game in the Library, **Save data**, **Export a copy**, then zip the folder it writes in `/data/prosperoeden/save-export`. With it the spot can be reached without playing the game up to there.

**No piracy.** ProsperoEden is for games you own and dumped yourself. Do not ask for, post or link to game files, keys or firmware, here or in any of the project's channels, and do not ask where to find them. Such posts are removed.

## Elevation credits

Filesystem elevation uses [PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon),
created by ArkSama. ProsperoEden follows the cooperative owned-root design and implementation
from [ProsperoEden's Lapy fork](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon), based on
[mpereiraesaa's cooperative helper](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon). The build
pins that source, builds its exact-title one-shot helper without local payload changes,
verifies the upstream-generated manifest, and includes the resulting ELF in the release.
The donor-release and firmware 13.60 fixes pinned here were merged upstream in
[PS5-Lapy-JB-Daemon PR #48](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/48) and
[PR #49](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/49).
The pinned `57fcc19` helper also includes the guarded target-lifetime and fail-closed accounting
changes from [PR #50](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/50). The previous
`54a095c` helper passed five automated launch/elevate/close cycles on firmware 6.02 and five on
12.70 with root access and balanced donor references. The pinned helper completed the target-exit
stress and a full one-shot elevation on firmware 12.70; it still requires an attended
ProsperoEden qualification run on firmware 6.02.

## Thanks

Thank you to the people who test ProsperoEden on their own consoles and report what they find:

- **Jones** ([X](https://x.com/Jonesskulls), [GitHub](https://github.com/AgentJonesy)), for helping with testing.
- **Phronesis**, for helping with testing.
- **szampan** ([GitHub](https://github.com/heni0xyz)), for helping with testing.
- **BunkinBacon**, for helping with testing.
- **Cold** ([GitHub](https://github.com/JMUtechnologies)), for helping with testing.
- **Bauls Deep** ([GitHub](https://github.com/bawsdeep)), for helping with testing.

And to the people who contribute code:

- **Mihawk** ([GitHub](https://github.com/mihawk-99)), besides the driver the Vulkan renderer runs on: cheats that can be chosen one by one, the fix for the crash when a game with cheats stops, the driver's shader cache working on the console, a fix for games that come without control data, and the driver build reading its versions from one place.
- **quatrixone** ([GitHub](https://github.com/quatrixone)), for the Performance settings and their seven switches, also per game, and for a much faster first compilation of a game's code.
- **Martin Pham** ([GitHub](https://github.com/MartinPham)), for starting a game straight from its path, which is what forwarders on the home screen use.
- **matschi95** ([GitHub](https://github.com/matschi95)), for the RomM integration (browsing, downloading and playing games from your own server, and save sync) and for finding and fixing the stutter that log writes caused every few seconds.
- **v0ltfault** ([GitHub](https://github.com/v0ltfault)), for tracking down why every game ended at its first picture on system software 7.40, with a fix and a write-up of the findings.
- **gyn7561** ([GitHub](https://github.com/gyn7561)), for bringing frame generation to the PS5, with its settings, and for the log lines that say why it is or is not running.
- **Bauls Deep** ([GitHub](https://github.com/bawsdeep)), for the Nix shell that sets up the whole build environment with one command.
- **lunar-me** ([GitHub](https://github.com/lunar-me)), for a correction to the README.

<!-- bbr-footer:start -->
<!-- Generated by ps5-homebrew-dev-protocol/scripts/readme-footer. Edit the template there, not here. -->

## Credits

Built with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) by John Törnblom (ps5-payload-dev).
Third-party components, authors and licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Copyright © 2026 BlackBearReloaded. Licensed under GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party components keep their own licenses. Binary releases are built from the tagged source in this repository.

## Disclaimer

- **No affiliation.** This is an independent homebrew project. It is not
  affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment.
  "PlayStation", "PS5" and related marks are trademarks of Sony Interactive
  Entertainment Inc. This project is not affiliated with or endorsed by the Eden project.
- **No proprietary material.** No Sony SDK, firmware, encryption keys or
  decrypted system modules are included.
- **No warranty.** This project is provided "as is", without warranty of any
  kind, to the extent permitted by law. See sections 15 and 16 of the GPL.
- **Use at your own risk.** Running homebrew requires a modified console, which
  may void its warranty, breach the platform's terms of service, or cause data
  loss.
- **Legal use only.** Use it only with hardware, accounts and content you own.
  This project does not support or enable piracy.

## AI assistance

This project was developed with AI assistance from OpenAI and/or Anthropic tools.
<!-- bbr-footer:end -->
