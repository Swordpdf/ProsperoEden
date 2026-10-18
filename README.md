<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="ProsperoEden icon">
</p>

<h1 align="center">ProsperoEden</h1>

<p align="center">
  <strong>An unofficial Eden emulator port for PlayStation 5 homebrew</strong>
</p>

**ProsperoEden is an unofficial PlayStation 5 port of [Eden](https://github.com/eden-emulator/mirror)** - an accurate, high-performance emulator. All credit for the emulator core belongs to the Eden project and its contributors. ProsperoEden is not affiliated with or endorsed by the Eden team or Sony.

This is an early alpha. Video, audio, controller input, and saves have been confirmed working. Compatibility and performance will vary between games. The current release is **v1.005.000**. What changed in it, and in every release before, is in the [changelog](docs/CHANGELOG.md#v1005000).

## Source code

The complete ProsperoEden source is in this repository: the PS5 frontend and launcher in `headless/`, and the build and packaging tools in `tools/`. To build it yourself, run `make` on Linux (Ubuntu 26.04; WSL works). It fetches every dependency at its pinned revision and writes the release files to `dist/`; `make help` lists the other targets. See [docs/BUILDING.md](docs/BUILDING.md). A release ZIP built by GitHub Actions can be checked with `gh attestation verify <ZIP> -R blackbearreloaded/ProsperoEden` (GitHub CLI); this covers releases built from now on (after v1.000.090), not earlier ones.

Local-wireless multiplayer through Eden rooms is new in v1.000.110 and experimental. See the
[setup guide and current limitations](docs/MULTIPLAYER.md) and the
[validation status](docs/MULTIPLAYER_IMPLEMENTATION.md); Tailscale forwarding is not yet qualified.

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
- **Resolution and upscaling** - render at 0.5x to 4x of the game's resolution and choose the filter that scales it to your TV (Bilinear, AMD FSR, Bicubic or Nearest) in **Settings > Video**. Textures in a format the PS5's GPU cannot read (ASTC) are kept compressed (as BC3), which takes a quarter of the graphics memory they would need decoded. **ASTC textures** in **Settings > Video**, and in each game's settings, can be set to **Full quality** instead: those textures are then kept decoded, which uses four times the graphics memory for them and can make a game run out of memory above 1x.
- **Output resolution** - the picture is made at 1080p, 1440p or 2160p: **Output resolution** in **Settings > Video**. The menu is drawn at that size too, and the PS5 scales it to your TV.
- **A steadier frame rate in heavy scenes** - some games time their own frames and drop to a lower frame rate (30 to 20, or 60 to 30) as soon as a frame takes a little too long. ProsperoEden lets the game's clock give by up to 15% in those moments, so the game keeps its frame rate and runs a few percent slower instead of losing a third of its frames.
- **120 Hz output** - on a display that shows 120 Hz, games can run on a 120 Hz output: **Refresh rate** in **Settings > Video**, or in one game's settings. A frame that is a little late is then shown 8 ms later instead of 17 ms, and patches for more than 60 FPS need it. The menu stays at 60 Hz.
- **Frame generation** - between two of a game's own pictures ProsperoEden can show one made up from them, so a 30 FPS game looks like 60: **Frame generation** in **Settings > Video**, or in one game's settings. Two methods: **MihawkGen**, which comes with the app, or Lossless Scaling with your own copy of it; see [Frame generation](docs/SETTINGS.md#frame-generation).
- **Post-processing (ReShade FX)** - a look drawn over the game's picture: **Post-processing** in **Settings > Video**, or in one game's settings, with the Vulkan renderer. Six presets come with the app (Anime, Cinematic, Clean, Dreamy, Retro, Vivid). Presets of your own (`.fxp`) go in `/data/prosperoeden/user/post_presets/` and the ReShade FX effects they use (`.fx`, with any `.fxh` they include) in `/data/prosperoeden/user/post_shaders/`; a game's own presets and effects go in its game folder's `reshade/`, and are offered for that game only. They are listed after a restart. See [Post-processing](docs/SETTINGS.md#post-processing).
- **Game files anywhere** - keys, firmware, and games can live in any folder the PS5 can read: internal storage, an M.2 or external drive, or a USB device.
- **Folder browser** - pick the game files folder in **Settings > Game files**. It shows how many keys, firmware files, and games each folder holds. Hold L1/R1 to page quickly.
- **Library** - your games as covers, with the selected game's details beside them. The home screen has **Continue Playing** and **Recently Played**, which keep working after you move your files.
- **Launcher** - an animated interface drawn with OpenGL, with sound effects (their level is in **Settings > Audio**) and a loading screen that shows the game's cover while it starts. It opens from the console's own launch picture without a cut: the mark appears over the picture and goes to its place as the menu arrives (any button skips it). Its five sections (Home, Library, Multiplayer, Settings, About) are on a rail at the left: L1 and R1 go from one to the next, and Left from a screen's edge opens the rail. The home screen shows which controllers are connected.
- **Themes** - the launcher's look is a theme, chosen in **Settings > Theme**. It comes with one, Canopy; [docs/THEMES.md](docs/THEMES.md) says what a theme is made of.
- **Wallpapers** - an image of your own behind the menu instead of the theme's artwork: PNG or JPEG files (also BMP, GIF, TGA and DDS) in `/data/prosperoeden/wallpapers`, chosen in **Settings > Appearance > Wallpaper**; see [docs/WALLPAPERS.md](docs/WALLPAPERS.md).
- **Profiles** - everyone who plays has their own save data, settings and recently played games: **Settings > Profiles**; see [Profiles](docs/SETTINGS.md#profiles).
- **Settings per game** - a game can differ from Settings in its video, performance, audio, controls and language, and has its own Handheld / Docked mode (Triangle in the Library); see [Settings per game](docs/SETTINGS.md#settings-per-game).
- **Button mapping** - choose which DualSense button presses each of the game's buttons, for every controller, in **Settings > Controls**, or for one game in its settings; see [Button mapping](docs/SETTINGS.md#button-mapping).
- **Your language** - the launcher follows the language the PS5 is set to (29 languages; English otherwise).
- **Accessibility** - larger text, high contrast and reduced motion, in **Settings > Accessibility**.
- **Save data in and out** - import a game's save from a folder or from a Ryujinx data folder, and export a copy (Triangle in the Library, then **Save data**).
- **Game updates and DLC** - keep each game in a folder of its own in `roms`, with its update and DLC files (NSP or XCI) beside it; see [Game files folder](docs/INSTALL.md#game-files-folder). They apply when the game starts, and each game's details show the update version and DLC count.
- **Download sources** - the games on servers in your network, such as a [RomM](https://github.com/rommapp/romm) server, appear in the Library next to the ones on the console. A game is downloaded when you play it, or ahead of time through a download queue, and then runs from the console like any other: nothing is streamed. Downloads need an FTP server running on the console; see [Download sources](docs/DOWNLOADS.md#download-sources).
- **Save sync** - each profile can keep its save data on a server, such as a [RomM](https://github.com/rommapp/romm) server, as Steam does: it is synced before a game starts and after it ended, and when it changed on two devices you choose which one stays. A profile is paired with its server by a QR code, no typing. See [Save sync](docs/SAVES.md#save-sync).
- **Mods and cheats** - patches and replacement game files from the `mods` folder in the game's folder, and cheats from its `cheats` folder. Each has a list of its own in the game's settings (Triangle in the Library, then **Mods** or **Cheats**), where every mod and every cheat is switched on or off, and a switch for all of them at once.
- **Performance switches** - seven switches that trade accuracy for speed (compiling a game's code ahead, asynchronous shaders, faster GPU, CPU and DMA emulation, and more) in **Settings > Performance**; see [Performance settings](docs/SETTINGS.md#performance-settings).
- **Shader cache** - shaders compiled in earlier sessions are loaded when a game starts, so an effect stutters only the first time it appears.
- **In-game shortcuts** - a performance overlay (Touchpad + R1), and Touchpad + L1 to end the game and return to the library. Leaving takes ten seconds at most, also while a game is still loading.
- **Settings in one place** - a single JSON file under `/data/prosperoeden`, with game volume, mute, and detailed logging options. Logs keep the previous session.
- **Speed limit** - **Settings > Performance > Speed limit** (and each game's own) lets a game run at 150%, 200%, 300% or without a limit: a game locked to 30 FPS can reach 60 at 200% when the console has time to spare, and a game timed by its frames plays faster with it; see [Speed limit](docs/SETTINGS.md#speed-limit).
- **Updates from the menu** - when a newer release is listed on [homebrew.page](https://homebrew.page/ps5), ProsperoEden offers to install it each time it opens; **Skip** keeps the current version, and **Settings > Diagnostics > Update notices** off stops the offer (About still names the newer version); see [Updating](docs/INSTALL.md#updating).
- **Crash reports** - if ProsperoEden stops because of an error, it saves a report with that session's logs, starts again and shows where the report is. **Settings > Diagnostics** also keeps the FTP log-folder path visible; attach the dated `crash-*.txt` report and its matching logs.
- **Controllers, audio, and saves** - up to four DualSense controllers (one per signed-in PS5 user) with rumble and motion controls, game audio, and save data work out of the box. A game that gives a player a single Joy-Con is played with the whole DualSense: the shapes are that Joy-Con's four buttons, the D-pad and either stick are its stick, and L1/R1 and L2/R2 are its shoulder buttons. When the game has the Joy-Con held sideways, the DualSense is still held as usual: the stick, the buttons and the motion sensors are turned to match.
- **Controller type per game** - some games take a Pro Controller and then only work with another controller. In **Library > Game settings > Controls**, **Controller type** gives the game a Pro Controller, the Handheld controller, Dual Joy-Cons, a Left Joy-Con or a Right Joy-Con. Left on **Automatic**, the game gets what it takes, a Pro Controller first.
- **Typing text** - when a game asks for text (a character's name, an answer during an event), the PS5's own on-screen keyboard opens, and what you enter goes to the game. A USB or Bluetooth keyboard the PS5 supports types into it too.

## Install

> [!TIP]
> **Use [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) or newer.** It mounts ProsperoEden from the folder (or drive) you copy it to, and it also mounts `/data` and USB and extended storage drives into the app's sandbox, so ProsperoEden reaches its data and your game files with nothing else to load. With an older ShadowMountPlus, ProsperoEden falls back to its bundled [Lapy](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon) helper (see [Elevation credits](#elevation-credits)).

1. Download and extract the release ZIP.
2. Copy the included `PPSA99008` folder to `/data/homebrew/PPSA99008` on the PS5 (or to another folder ShadowMountPlus scans, such as `/mnt/usb0/homebrew/PPSA99008`).
3. Make sure your jailbreak environment's local ELF loader is listening on TCP port 9021 (most setups have it). [Updates from the menu](docs/INSTALL.md#updating) use it, and with a ShadowMountPlus older than 1.7beta4 ProsperoEden sends its bundled Lapy helper through it.
4. Put your own legally dumped keys, firmware, and games in a **game files folder** ([its layout](docs/INSTALL.md#game-files-folder)). It can be anywhere the PS5 can read: internal storage, an M.2 or external drive, or a USB device.
5. Launch **ProsperoEden**, open **Settings > Game files**, browse to that folder and select it. The default is `/data/prosperoeden`.
6. Close and reopen ProsperoEden, then open **Library**. Setup is checked when the app opens, so reopen it after changing the folder or adding keys or firmware.

## Guides

<a name="game-files-folder"></a><a name="app-data"></a><a name="updating"></a><a name="going-back-to-the-previous-version"></a><a name="moving-save-data"></a><a name="mods"></a><a name="quick-start-with-romm"></a><a name="download-sources"></a><a name="romm"></a><a name="save-sync"></a><a name="romm-1"></a><a name="performance-settings"></a><a name="frame-generation"></a><a name="post-processing"></a><a name="profiles"></a><a name="settings-per-game"></a><a name="button-mapping"></a><a name="language-and-accessibility"></a><a name="upgrading-from-an-earlier-alpha"></a><a name="roadmap"></a>

| Guide | What is in it |
|---|---|
| [Game files, app data and updates](docs/INSTALL.md) | The layout of the game files folder, where settings, saves and logs are kept, updating from the menu or by hand, going back to the previous version |
| [Settings](docs/SETTINGS.md) | The Performance switches, post-processing (ReShade FX presets), frame generation, profiles, settings per game, button mapping, language and accessibility |
| [Save data](docs/SAVES.md) | Moving save data in and out, and save sync with a server |
| [Download sources and RomM](docs/DOWNLOADS.md) | Games from a RomM server or another source in your network, and the download queue |
| [Mods and cheats](docs/MODS.md) | Patches and replacement game files, and cheats, each in a list of their own |
| [Smash Ultimate 13.0.4](docs/SMASH_MODS.md) | Tested Skyline/ARCropolis setup and moveset installation |
| [Multiplayer](docs/MULTIPLAYER.md) | Local wireless play through Eden rooms: setup and current limits |
| [Forwarders](docs/FORWARDER.md) | Starting a game from its own tile on the PS5's home screen |
| [Themes](docs/THEMES.md) | What a launcher theme is made of |
| [Wallpapers](docs/WALLPAPERS.md) | Your own image behind the launcher's screens, the folder it reads and the formats it supports |
| [Building](docs/BUILDING.md) | Building from source, and how releases are made |
| [Changelog](docs/CHANGELOG.md) | What changed in each release |
| [Roadmap](docs/ROADMAP.md) | What is planned |

Reading this from a release ZIP? The guides are at <https://github.com/blackbearreloaded/ProsperoEden/tree/main/docs>.

## In-game shortcuts

The touchpad is pressed as a button. On its own, clicking the left half of the touchpad presses Minus (or its configured [button mapping](docs/SETTINGS.md#button-mapping)); clicking the right half presses Plus. A tap is delivered on release, and a longer press holds the button. If touch coordinates are unavailable, the configured touchpad mapping is used. Touchpad + L1/R1 shortcuts work from either half and never reach the game.

| Shortcut | Action |
|---|---|
| Touchpad + R1 | Toggle the performance HUD |
| Touchpad + L1 | End the running game and return to the library |

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
- **Cold** ([GitHub](https://github.com/JMUtechnologies)), for testing and for submitting a prototype of per-game presets and in-app compatibility reports ([#127](https://github.com/blackbearreloaded/ProsperoEden/pull/127), not merged).

Thank you also to the code, documentation and translation contributors (several also help with testing):

- **Mihawk** ([GitHub](https://github.com/mihawk-99)), besides the driver the Vulkan renderer runs on and the MihawkGen frame generation: cheats that can be chosen one by one, the fix for the crash when a game with cheats stops, the driver's shader cache working on the console, a fix for games that come without control data, and the driver build reading its versions from one place.
- **quatrixone** ([GitHub](https://github.com/quatrixone)), for the Performance settings and their seven switches, also per game, and for a much faster first compilation of a game's code.
- **Martin Pham** ([GitHub](https://github.com/MartinPham)), for starting a game straight from its path, which is what forwarders on the home screen use.
- **matschi95** ([GitHub](https://github.com/matschi95)), for the RomM integration (browsing, downloading and playing games from your own server, and save sync) and for finding and fixing the stutter that log writes caused every few seconds.
- **v0ltfault** ([GitHub](https://github.com/v0ltfault)), for tracking down why every game ended at its first picture on system software 7.40, with a fix and a write-up of the findings.
- **gyn7561** ([GitHub](https://github.com/gyn7561)), for bringing frame generation to the PS5, with its settings, and for the log lines that say why it is or is not running.
- **Bauls Deep** ([GitHub](https://github.com/bawsdeep)), for testing, the Slate theme and accent presets and their polish, wallpapers of your own, menu button remapping, the Vulkan submission fix, the Nix shell that sets up the whole build environment with one command, for the texture collector's lookahead, where the fix for the half-second freezes began, for the development timers and the recorded-input replay, for the FPS overlay as a system monitor, and for bringing MihawkGen frame generation into the app.
- **lunar-me** ([GitHub](https://github.com/lunar-me)), for a correction to the README.
- **OverkillLabs** ([GitHub](https://github.com/OverkillLabs3)), for testing, the fix that keeps the fast path for exclusive memory accesses with Faster CPU emulation on, and for PS5 Pro memory mode.
- **EIREXE** ([GitHub](https://github.com/EIREXE)), for the RNG seed per game, which lets mods that tell an emulator from a console by where the game is loaded work.
- **rpromas** ([GitHub](https://github.com/rpromas)), for finding and fixing the crash in games that open the microphone.
- **mzet97** ([GitHub](https://github.com/mzet97)), for finding and fixing why the texture collector stopped freeing graphics memory, which let it fill up at resolutions above 1x.
- **Guilherme Amado** ([GitHub](https://github.com/GuilhermeAmado)), for HD rumble through DualSense audio haptics, with motor fallback.
- **Tiang-88** ([GitHub](https://github.com/Tiang-88)), for per-game console memory layouts and showing the game's own resolution in the performance overlay.
- **kasobos67** ([GitHub](https://github.com/kasobos67)), for custom launcher support, and launcher pack updates from a feed with a faster library for packs.
- **Rockiu0** ([GitHub](https://github.com/Rockiu0)), for JIT and memory-access optimizations, GPU command and texture-cache improvements, Vulkan rendering fixes, CPU scheduling and vsync changes, OpenGL queue tuning, build tooling, and OnionHEN elevation support.
- **cippy-9** ([GitHub](https://github.com/cippy-9)), for the Malay (ms-MY) translation.
- **Oliv3rmoon** ([GitHub](https://github.com/Oliv3rmoon)), for letting games with many pipelines start, keeping the driver's pipeline cache when a write is cut short, naming the refused allocation in crash reports, and reading a packed update's languages for a game's language.

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
