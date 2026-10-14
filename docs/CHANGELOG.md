# Changelog

What changed in each release of ProsperoEden, newest first. A release's notes on GitHub are its section here.

## v1.002.000

- **More free memory for demanding games.** The console's memory is one pool shared by the emulated console, the emulator itself and the graphics, and part of it was held without being used. The emulated console's memory, the code caches and a reserve on the system's side now take memory only when they need it. A large open-world game at 2x has about 3.5 GiB free during play instead of 2.2 GiB, at the same frame rate, so a long session runs out of memory far later.
- **PS5 Pro memory mode.** On a PS5 Pro, ProsperoEden asks for the larger memory pool the console offers: 13.5 GiB instead of 12 GiB. A line at launch (`memory:` in `stderr.log`) says whether it is active. Contributed by [OverkillLabs](https://github.com/OverkillLabs3).
- **"Ran out of graphics memory" less often above 1x.** The part that frees textures no game is using could stop looking before it had freed anything, so graphics memory filled up although much of it could be freed. Contributed by [mzet97](https://github.com/mzet97).
- **Vibration strength (new setting).** **Settings > Controls > Vibration strength** sets how strong the DualSense vibrates: 25%, 50%, 75% or 100% of what a game asks for. The controller answers each change with a short vibration at the new strength, so the level is chosen by feel.
- **A game without your language starts in English.** With a language chosen that a game does not have, the game started in the next language of the console's own order: with Brazilian Portuguese that was Spanish. It now gets the chosen language or a variant of it when it has one, and English otherwise.
- **The FPS overlay is a small system monitor.** Turned on in **Settings > Video > FPS overlay** (or in a game's own settings), it shows over the game: the graphics backend, the frame rate against the game's own, the frame time, each of the four emulated CPU cores, the GPU's busy time measured by the GPU itself, the memory taken of the pool and the graphics' part of it, for example `VLK 30/60FPS 34.6MS CPU 92% 45% 38% 3% GPU 71% MEM 7.7/12.1G GFX 4.1G`. **Settings > Video > Overlay options** turns each piece on or off, sets its size, position and colours, and with **CPU by core** off shows only the busiest core; a game's settings can give it its own size and position. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Variable refresh rate on older system software.** On system software 6.02 the console did not know the way ProsperoEden declared a variable refresh rate and listed the app without one. It is now declared the way older system software knows.

**Thanks** to the people whose work is in this release:

- [OverkillLabs](https://github.com/OverkillLabs3), for PS5 Pro memory mode, measured on a PS5 Pro.
- [Bauls Deep](https://github.com/bawsdeep), for the FPS overlay's new look as a system monitor, with its options and the settings per game.
- [mzet97](https://github.com/mzet97), for finding and fixing why the texture collector stopped freeing graphics memory.
- [Mihawk](https://github.com/mihawk-99), for the measurements of the console's memory pools that the memory work builds on, and for the Vulkan driver all of this runs on.
- [tonysantosl](https://github.com/tonysantosl), for reporting the language that games fell back to, and [zh522130](https://github.com/zh522130), for asking for a vibration that can be turned down.

And to everyone who tests ProsperoEden on their own console and reports what they find.

## v1.001.000

- **A new launcher.** The menu was rebuilt. Five sections sit on a rail at the left (Home, Library, Multiplayer, Settings, About): L1 and R1 go from one to the next, and Left from a screen's edge opens the rail. Home is built around the game to continue, with its cover and what it runs with. The Library shows your games as covers, with the chosen game's details beside them. A game's settings are a page of their own, and Settings is a board of tiles.
- **Themes.** The launcher's look is a theme, chosen in **Settings > Theme**. This version comes with one, Canopy; [docs/THEMES.md](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/THEMES.md) says what a theme is made of.
- **An opening.** ProsperoEden appears out of the console's own launch picture without a cut: its mark comes forward over the picture, then goes to its place as the menu arrives. Any button skips it, and it plays only when the app is started.
- **A loading screen with the game's cover.** While a game starts, its cover stands in the middle with a line of light going round it as the start advances, in front of the cover's own colours. Under it are the step the start is at, the shaders built so far, and the seconds a slow step has lasted.
- **The controller's buttons are drawn.** **Settings > Controls** and the button mapping show Touchpad, L1, R1 and the other buttons as pictures instead of names.
- **Game settings from the home screen.** The button beside **Play**, and Triangle on the game to continue or on a recent game, open that game's settings.
- **Demanding games keep their frame rate in heavy scenes.** A game that times its own frames could fall from 30 to 20 frames a second whenever its frames came a few percent late. The clock the game reads now gives by up to 15% in those moments: the game keeps its frame rate and runs a little slower instead. Sound, the display and timeouts keep the real clock.
- **Full speed on newer system software.** On consoles where the system does not tell the processor's cores apart, the emulator's threads were left to share cores and games ran markedly slower. The cores are now found by a timing, and each thread gets its own.
- **No more drops to 10 or 15 frames a second in new scenery.** Textures in a format the PS5's GPU cannot read (ASTC) were converted inside the draw that first used them, which held up the whole frame. In play they are now converted in the background; on loading screens, menus and title screens they are still converted at once, so those pictures appear complete.
- **ASTC textures (new setting).** **Settings > Video > ASTC textures**, also in each game's settings. **Compressed** (the default) keeps such textures as BC3, a quarter of the graphics memory they need decoded; **Full quality** keeps them decoded.
- **"Ran out of graphics memory" far less often.** Four causes are fixed. The count of graphics memory in use read twice what was held, so games were refused memory that was free. ASTC textures took four times the memory they need. The reserve that memory is freed for was smaller than a game can ask for at once. And memory the app keeps for its whole life sat in the middle of the pool, so every game after the first had less room in one piece.
- **Fewer freezes and long frames.** Freeing textures when graphics memory ran short could stop the picture for half a second. A game that read unmapped memory in bursts stalled for up to a second on its own log lines. And the graphics driver held up one frame every ten seconds, in every game, to write a line of statistics. All three are gone.
- **A crash with "jumped to an unmapped address" is fixed.** It came from where the console happened to place the game's memory, so it struck some starts and not others.
- **RNG seed per game (new).** In a game's settings under **System**. With a seed the game is loaded at the same place in memory at every start, which some mods need. Contributed by [EIREXE](https://github.com/EIREXE).
- **Games that open the microphone no longer crash.** Contributed by [rpromas](https://github.com/rpromas).
- **Faster CPU emulation** no longer turns off the fast path for a game's exclusive memory accesses when it is switched on. Contributed by [OverkillLabs](https://github.com/OverkillLabs3).
- **PS5 OpenGL SDK 1.0.2** for the launcher and the OpenGL renderer (it was 1.0.0). A game whose shaders are compiled inside its first frames is no longer ended on its first run with the OpenGL renderer, the menu no longer loses a frame of about 180 ms every few seconds while **Write logs at once** is on, and drawing fixes from [Mihawk](https://github.com/mihawk-99) and [mpereiraesaa](https://github.com/mpereiraesaa) are in.
- **A single Joy-Con held upright.** A game that gives a player one Joy-Con to hold upright is played with the whole DualSense: the shapes are that Joy-Con's four buttons, the D-pad and either stick are its stick, and L1/R1 and L2/R2 are its shoulder buttons.

**Thanks** to the people whose work is in this release:

- [Bauls Deep](https://github.com/bawsdeep), for the texture collector's lookahead, where the fix for the half-second freezes began, and for the development timers and the recorded-input replay.
- [EIREXE](https://github.com/EIREXE), for the RNG seed per game.
- [rpromas](https://github.com/rpromas), for finding and fixing the crash in games that open the microphone.
- [OverkillLabs](https://github.com/OverkillLabs3), for the fix that keeps the fast path for exclusive memory accesses with Faster CPU emulation on.
- [Mihawk](https://github.com/mihawk-99) and [mpereiraesaa](https://github.com/mpereiraesaa), for the drawing fixes in the OpenGL SDK, and Mihawk for the Vulkan driver all of this runs on.

And to everyone who tests ProsperoEden on their own console and reports what they find.

## v1.000.110

- **Multiplayer through Eden rooms (new, experimental).** ProsperoEden can join an Eden room, so a game's Local Wireless mode reaches other players over the internet: other PS5 consoles running ProsperoEden, and Eden on a PC. Open **Multiplayer** (a section of the menu: R1 twice from the home screen), join a room, then start the game and choose its Local Wireless mode. The home screen shows when a room is connected, the game overlay shows the delay to the room in milliseconds, and when the connection is lost the game closes and the launcher says why. See the [setup guide](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/MULTIPLAYER.md).
- **Private room.** Enter a room's address, UDP port, a nickname and its password. They are saved as you type them, and joining a public room does not replace them. Enter the room's IPv4 address: looking up a hostname does not work on the console yet.
- **Public rooms.** A list of the rooms in Eden's public directory, with each room's game, how many players it has and whether it asks for a password. Rooms you can join come first, the busiest at the top; Left and Right move a page at a time and Triangle refreshes. Rooms that use another protocol version are marked and cannot be joined.
- **For a stable multiplayer session**, everyone needs the same game and the same update of it, and the player who hosts the lobby should use 1x resolution with frame generation off. A host that cannot hold full speed ends the session for everyone.
- **Connection problems are explained.** When a room cannot be joined, a panel says what happened and what to try: a wrong password, a full room, a room that did not answer, a name already in use. **Settings > Diagnostics** has the detailed log to send with a report.
- **Self-update keeps the version it replaces.** After an update, the previous version's files stay in `/data/prosperoeden/previous-version/PPSA99008-backup` until the next update. See [Going back to the previous version](INSTALL.md#going-back-to-the-previous-version).
- **One place for your data.** Settings, saves and covers are only ever in `/data/prosperoeden`. When ProsperoEden could not get access to the console's storage at a start, it used to carry on with empty settings and no saves, which looked as if they had been lost; it now says so on the home screen and starts no game. Keys, firmware and games that an early version kept inside the app's folder are moved to `/data/prosperoeden` at the first start, so that replacing the app cannot remove them.
- **120 Hz holds on a display with a variable refresh rate.** With **Refresh rate** at 120 Hz, such a display was read as not having switched and the output went back to 60 Hz.
- **No more lost graphics device in shader-heavy scenes.** Long scenes could stop a game with `VK_ERROR_DEVICE_LOST`; draws are now handed to the GPU in smaller batches. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Eden updated** to its revision of 8 October (`67bada77`), together with the Vulkan driver (Mesa), its tools and the Payload SDK.
- **The helper that gives ProsperoEden access to storage** no longer reads a title's kernel data after that title has exited during its start.

## v1.000.100

- **Frame generation (new).** Between two pictures a game draws, ProsperoEden can show one made up from them, so a 30 FPS game looks like 60. It is in **Settings > Video** and in each game's own settings, works with the Vulkan renderer, and needs your own copy of Lossless Scaling: without its `Lossless.dll` the settings are greyed out. See [Frame generation](SETTINGS.md#frame-generation). Contributed by [gyn7561](https://github.com/gyn7561).
- **A new loading screen.** It lists the steps of a game's start (reading the game, starting graphics, building shaders, starting the game) with a progress bar; while shaders are built it counts them. A step that lasts gets its seconds shown, so a start that is stuck can be told from one that is slow.
- **Cheats in a file of their own work.** A cheat file placed straight in a game's mods folder (`cheat_<name>.txt`) was listed and switched on, but changed nothing in the game. It does now. The session log also names the game's build ID, the name a cheat file for that version of the game has.
- **A game no longer fails every start after a crash.** A crash, or a console that went down, could leave a game's shader cache file damaged; that game then crashed at every start on the same renderer. The damaged part is now cut off and the rest is kept.
- **No more stutter from log writes.** Games dropped frames every few seconds while the logs were written. Logs are now written in the background once a second; **Settings > Diagnostics > Write logs at once** brings the old behaviour back, to find a crash. Contributed by [matschi95](https://github.com/matschi95).
- **The FPS overlay with frame generation.** It shows the game's own rate (N) beside the rate on screen (F) while frames are being added, and its usual single figure otherwise. Contributed by [gyn7561](https://github.com/gyn7561).
- **The log says why frame generation is not running**, in lines that start with `framegen:`. Contributed by [gyn7561](https://github.com/gyn7561).
- **A Nix shell for building.** `shell.nix` sets up the whole build environment. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Issues are open on GitHub**, with forms for bug reports and game reports; they ask for the save data and the debug logs. See [Issues and reports](../README.md#issues-and-reports).

## v1.000.095

A bug fix for v1.000.090. If you are on v1.000.090, update.

- **Stutter and crashes while playing are fixed.** In v1.000.090 every button press could make a game bring up its controller screen again: play stuttered, and some games crashed after a minute or two. A second, handheld controller was being connected beside the player's own at the first press. It is now only used when **Handheld** is the game's Controller type, or the only controller the game takes.

## v1.000.090

- **Download sources (new, early).** Games on a [RomM](https://github.com/rommapp/romm) server in your network appear in the Library and are downloaded to the console when you want to play them; see [Download sources](DOWNLOADS.md#download-sources). Contributed by [matschi95](https://github.com/matschi95).
- **Save sync (new, early).** Each profile can keep its save data in step with a RomM server, synced before a game starts and after it ends, and paired by scanning a QR code; see [Save sync](SAVES.md#save-sync). Replaced save data is kept as a backup first. Contributed by [matschi95](https://github.com/matschi95).
- **Controller type per game.** **Library > Game settings > Controls > Controller type** gives a game a Pro Controller, the Handheld controller, Dual Joy-Cons, a Left Joy-Con or a Right Joy-Con, for games that take a Pro Controller and then only work with another.
- **Typing text.** When a game asks for text, the PS5's own on-screen keyboard opens, and what you enter goes to the game.
- **Firmware 7.40 and 7.61: games no longer end at their first picture.** Contributed by [v0ltfault](https://github.com/v0ltfault).
- **Game names without question marks.** A name with a look-alike character (a sequel's Roman numeral, full-width letters) is shown with plain letters, a name that is not text is skipped, and characters no font has are left out.
- **System language in the start-up trace**, with the menu's language and the console fonts found, for reports from other consoles.
- **Builds on GitHub.** Releases are now built by GitHub Actions from the tagged source.

## v1.000.080

- **Single Joy-Con games with the DualSense held as usual.** A game that has a single Joy-Con held sideways no longer needs the DualSense turned sideways: the stick, the four buttons and the motion sensors are turned to match, for each player. L1 and R1 stay SL and SR.
- **What's new in the update dialog.** When an update is offered, **What's new** shows that release's notes before you decide.
- **Start a game directly.** ProsperoEden takes `--rom <file>` and `--exit-after-game` as launch arguments, so a home screen forwarder can open one game; see [docs/FORWARDER.md](FORWARDER.md).
- **Filesystem access on firmware 13.60.** The bundled helper is updated with a fix for that firmware.
- **Release ZIP opens with the right permissions.** Its files are stored open to all, as the console wants an app's files; unpacked with their permissions kept, they could give "Can't start the game or app" (CE-107750-0).

## v1.000.070

- **ProsperoEden starts wherever it is installed.** On a USB drive, extended storage, an etaHEN games folder or any other place ShadowMountPlus mounts it from, it could not find its own files and closed after a moment. It now reads its files from where the PS5 mounts the running app.
- **Updates from the menu work on those installs too.** The update helper updates the folder ShadowMountPlus mounted the app from; an app installed as an image is not changed, and the dialog says to replace the image.
- **Clearer error when the menu cannot start**, naming the file and the reason.
- **Start-up trace for bug reports.** Each start-up step is written to the kernel log (`[ProsperoEden diag]`) and to `/data/prosperoeden/logs/boot-trace.txt`.

## v1.000.060

- **Updates from the menu.** When a newer release is listed on homebrew.page, ProsperoEden offers it each time it opens. **Update now** downloads and checks it, unpacks it beside the app, then closes so the update helper can replace its files; **Skip** keeps the current version until the next time it opens. Your saves, settings and own files in the app folder are kept. See [Updating](INSTALL.md#updating).
- **Lapy elevation included.** ProsperoEden carries its own one-shot helper built from upstream Lapy and sends it over the local ELF loader on port 9021; loading Lapy separately is no longer needed.
- **The elevation helper no longer crashes when ProsperoEden starts.** Its start-up runs on one thread, and it is built from a corrected upstream release.

## v1.000.050

- **Profiles.** **Settings > Profiles** lists the people who play on the console, up to eight. Each profile keeps its own save data, its own settings (everything under Settings, and each game's own) and its own recently played games. See [Profiles](SETTINGS.md#profiles). If you update from an earlier version, your saves, settings and games played become the first profile's, with nothing to do.
- **Button mapping.** Choose which DualSense button presses each of the game's buttons, for every controller, in **Settings > Controls**, or for one game in its settings. See [Button mapping](SETTINGS.md#button-mapping).
- **Settings per game.** A game's settings now have Video, Performance, Audio, Controls and Language; each value follows Settings until you choose another. See [Settings per game](SETTINGS.md#settings-per-game).
- **Performance settings.** **Settings > Performance** has seven switches that trade accuracy for speed, for every game or for one. See [Performance settings](SETTINGS.md#performance-settings).
- **Cheats chosen one by one.** A mod that lists several cheats shows each under it with its own switch, and only the chosen ones run. See [Mods](MODS.md#mods).
- **Update notice.** When a newer release is listed on homebrew.page, the menu says so at the top right for ten seconds. Nothing is downloaded or installed. See [Updating](INSTALL.md#updating).
- **Leaving a game takes ten seconds at most.** If a game has not stopped ten seconds after Touchpad + L1, or after it ended by itself, ProsperoEden starts again and opens at the menu. Some games took minutes to close.
- **Touchpad + L1 works while a game loads.** A load that hangs can be left; before, the only way out was closing ProsperoEden.
- **A game's controller screen always gets an answer.** A game that takes only the handheld controller gets it in Docked mode too, and a game that names no controller gets a Pro Controller. Both used to leave the game waiting.
- **Removed games leave the menu.** A game whose file leaves the game files folder leaves Continue Playing, Recently Played and the Library within two seconds, and is not started.
- **Touchpad by its name.** The menu and this document say Touchpad + L1 and Touchpad + R1.
- **The graphics driver's shader cache works again**, so shaders compiled in earlier sessions load when a game starts.
- **A crash when a game with cheats stops is fixed.**
- **Games start a little sooner**: the instruction decoder's tables are built from one reading of each pattern.
- The graphics driver is built from Mihawk's current PS5 Mesa and Vulkan.

## v1.000.040

- **The launcher in your language.** It follows the language the PS5 is set to, in 29 languages. Arabic, Chinese, Greek, Japanese, Korean and Thai are drawn with the console's own fonts. Text that runs longer in another language makes its own room instead of being cut.
- **Accessibility.** **Settings > Accessibility** adds **Larger text**, **High contrast** and **Reduce motion**. Warnings carry a mark as well as a colour.
- **Save data in and out.** Import a game's save from a folder or from a Ryujinx data folder, and export a copy. See [Moving save data](SAVES.md#moving-save-data).
- **A ten-second slowdown with the AMD FSR filter is fixed.** In a large open-world game, gameplay could start at 3-9 FPS for about ten seconds, and frames of about a tenth of a second kept coming afterwards. The texture cache was throwing away images the GPU had drawn, with a wait for the GPU each time, as soon as memory use passed a mark that the FSR filter's own images pushed it over. It now keeps them until graphics memory is really short.
- **Smoother heavy scenes.** The renderer hands its work to the Vulkan worker in larger batches, and the emulated cores wait less on the GPU caches' locks. In the heaviest area of a test walk this removed drops to 26-28 FPS and cut the GPU thread's work by about a quarter.
- **Touchpad as Minus in games.** A tap of the touchpad presses the game's Minus button, and a longer press holds it. The Touchpad + L1 and Touchpad + R1 shortcuts never reach the game as a press.
- **A crash a few seconds into some games is fixed.** The motion sensors' updates could reach a part of the controller service that was not set up yet.
- **Games' own system screens.** A game's error dialog, profile picker and similar screens use Eden's built-in versions, and its error dialog now answers the game instead of leaving it waiting. The firmware's versions could end a session with an out-of-memory error.
- **More graphics memory.** The PS5 gives an app one pool of memory that the CPU and the GPU share, and ProsperoEden held about 3 GiB of it without using it: its heap took 3 GiB at start and the emulated console's page table 1 GiB. Both now take memory as they need it, which gives graphics about 2.6 GiB more in the largest game tested. That game ran out of graphics memory while loading at 2x; it now runs at 2x with the AMD FSR filter at a steady 30 FPS, with about 2 GiB to spare. The texture cache also measures its memory use from what is really left of the pool, instead of a driver figure that counted every allocation twice.
- **A clearer message when a game runs out of graphics memory.** If a game still needs more than there is at a high resolution, give it its own lower one: Triangle on it in the Library, then **Resolution**.
- **Games that accept only single Joy-Cons** now get one, and L1 and R1 are its SL and SR buttons, which those games ask for on their controller screen.
- **Mods.** A game's patches, replacement files and cheats load from `mods/<title ID>/` next to `roms/`, and the game's settings list them with a switch each. `.ips` and text `.pchtxt` patches, which the emulator core could not apply, now work. The Library has one switch per game for all of its mods. See [Mods](MODS.md#mods).
- **120 Hz output.** **Refresh rate** in **Settings > Video**, and in each game's settings, asks the display for 120 Hz while a game runs. A display that cannot show it stays at 60 Hz, and the menu always runs at 60 Hz. After a game at 120 Hz the menu takes about five seconds to come back while the display changes rate.
- **Output resolution.** **Settings > Video** has a new **Output resolution** row: 1080p (as before), 1440p or 2160p. It is the size of the picture ProsperoEden puts out, for the menu and for games. Until now a game's picture was made at 1080p and enlarged, whatever its internal resolution; at 2160p the upscaling filter scales the game straight to a 4K picture. A larger picture needs more graphics memory.
- **3x and 4x resolution.** The internal resolution now goes up to 4x. These need far more graphics memory than 2x; a game that runs out says so, and can be given its own lower resolution.
- **Patches for more frames than the display shows.** A game patched to run faster than the output refreshes keeps its pace: the frames the display has no refresh for are left out.
- **Crash reports.** If ProsperoEden stops because of an error, it writes `crash-<date>-<time>.txt` to `/data/prosperoeden/logs`, starts again and says on the home screen where the report is. That session's logs are kept beside it, and the five newest reports stay. A report holds what failed, where in the app's code, and what was running; it never holds the contents of memory.
- The launcher and the OpenGL renderer use the PS5 OpenGL 4.6 SDK 1.0.0.
- The graphics driver's shader cache moved to `/data/prosperoeden/cache`, so a read-only package install keeps it.
- **Experimental: block list.** With an empty file named `block-list.txt` in `/data/homebrew/PPSA99008`, ProsperoEden saves which code a 64-bit game compiled and compiles it again on a spare CPU when the game next starts. In a repeat session of a large open-world game, the emulated cores then compiled 426 blocks during play instead of 138,262, and gameplay started at 30 FPS instead of 22. It is off by default until more games have run with it.

## v1.000.030

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

## v1.000.020

- **Vulkan is now the recommended and default renderer.** OpenGL remains available in Settings.
- **Choose where your game files live.** The new **Settings > Game files** browser selects any folder, including external drives and USB devices. The old `assets/` folder keeps working until you choose one.
- **App data moved to `/data/prosperoeden`.** Config, logs, covers, and saves now live outside the app folder, so updating the app never touches them.
- **Settings are now stored in one JSON file.** Earlier text settings are migrated automatically on first launch.
- **Faster Library navigation**, and covers and recent games that survive folder moves.
- **New icons** for controller actions, pages, and the Handheld / Docked mode.

**Known issues:** Ending a game with Touchpad + L1 can take several seconds. Some games can still hang on the loading screen, and some demanding games remain slow. This is a testing pre-release, not a compatibility guarantee.
