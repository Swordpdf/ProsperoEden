# Settings

The settings that take more than a line to explain: the Performance switches and speed limit, frame generation, profiles, settings per game, button mapping, language, accessibility and update notices.

## Performance settings

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
| Compact memory | `compact_memory` | off | Gives back memory a game wrote but left mostly empty, 16 KiB at a time instead of 2 MiB (see [Memory](#memory)). More free memory; a game that writes there again waits about 0.3 ms for it, so a game that keeps rewriting such memory can stutter. Measured: 100 to 300 MiB more free in two games, at the same frame rate. |

A game can have its own values: in its settings, **Performance** (see [Settings per game](#settings-per-game)). In the settings file, `"performance"` at the top level is what the switches set, and one inside a game's entry under `"games"` is that game's, whose values go first:

```json
{
  "performance": { "block_list": false, "async_shaders": false, "fast_gpu": false, "unsafe_cpu": false,
                   "unsafe_dma": false, "reactive_flushing": true, "skip_invalidation": false,
                   "compact_memory": false },
  "games": { "0100000000010000": { "performance": { "fast_gpu": true } } }
}
```

The accuracy switches help only where a game is held back by what they relax. In the scene they were measured in on the console, a large game standing at its frame rate limit, switching all of them on changed neither the frame rate nor how busy the emulated processor and the graphics thread were. Leave them off unless a game runs under its frame rate, and switch them back off if its graphics go wrong.

Resolution, the upscaling filter, the renderer and the refresh rate, which change speed too, are in **Settings > Video** and in a game's own settings.

### Speed limit

**Settings > Performance > Speed limit** is how fast a game may run, as a percentage of the console it was made for (Eden's *Limit Speed Percentage*). Left and right move over **100%** (the default, and what every earlier version did), **150%**, **200%**, **300%** and **Unlimited**. A game can have its own in **Library > Game settings > Performance**, which follows Settings until you choose one; it applies when a game starts.

Above 100%, the emulated console's display refreshes that much more often, so a game that waits for it runs faster when the PS5 has the time to spare. A game locked to 30 FPS can then reach 60 at 200%, without a 60 FPS mod. It speeds up the whole game, not only its frame rate: a game that keeps time by counting its frames (many do) also plays faster, its animations, timers, music cues and physics included, while a game that keeps time by the clock shows more frames at the same pace. Where the console cannot keep up, the game runs as fast as it can and no faster. **Unlimited** switches the limit off: the game runs as fast as the console allows.

| In the menu | In the file |
|---|---|
| 100% | `"100"` (the default) |
| 150% | `"150"` |
| 200% | `"200"` |
| 300% | `"300"` |
| Unlimited | `"unlimited"` |

```json
{
  "performance": { "speed_limit": "100" },
  "games": { "0100000000010000": { "performance": { "speed_limit": "200" } } }
}
```

The launch line says what a session got (`Performance: ... speed limit 200%` in `stderr.log`).

### Adaptive guest clock

Games use the normal guest tick rate by default. The automatic clock adjustment introduced in
v1.001.000 can feed back on itself: if a game waits on its tick counter to pace a frame, slowing
that counter lengthens the wait, which the adjustment mistakes for more work. A 30 FPS game can
then settle at 25.5 FPS even after a brief delay.

For a game that drops its frame rate in steps (for example, from 30 to 20 FPS), the old workaround
remains available as an advanced option in `config/prosperoeden.json`, under that game's title ID:

```json
{
  "games": { "0100000000010000": { "adaptive_clock": true } }
}
```

This is a file-only option; there is no menu switch. Leave the game before editing the file, and
start it again afterwards. It reduces the guest's tick rate by at most 15%; audio and kernel
timeouts keep their normal clocks. Remove the option or set it to `false` if play slows down.
The launch log records `adaptive clock on` or `adaptive clock off` for each session.

### Console memory

A game's console has 4 GiB of memory, and its kernel divides them between the game, the applets and the system. **Settings > Performance > Memory** gives games the larger memory the console's developer units have: how much the game and everything running in it is told there is. Left and right move over the three; it applies when a game starts, so leave the game and start it again.

| In the menu | In the file | The game is told | |
|---|---|---|---|
| 4 GiB | `4Gb` (the default) | 4 GiB | The memory a game's console has. Everything works with it. |
| 6 GiB | `6Gb` | 6 GiB | |
| 8 GiB | `8Gb` | 8 GiB | |

A mod that keeps memory of its own asks the kernel how much there is and takes what it wants from the answer. A mod that wants more than 4 GiB then finds less than it asked for, and the game stops before its first picture; that mod's game needs 6 or 8 GiB, as [Mods](MODS.md) describes.

```json
{
  "performance": { "memory_layout": "6Gb" },
  "games": { "0100000000010000": { "performance": { "memory_layout": "8Gb" } } }
}
```

The menu is `"performance" / "memory_layout"` in the settings file, so the two are the same thing and either can be changed with the other. A game can still be given one of its own in the file, inside its entry under `"games"`, which comes first: the menu's value is for every game. Above 4 GiB the guest's memory is backed where the game's kernel uses it, so a 6 GiB layout holds only what a session touches; the memory pool the processor shares with the graphics still has to take that much, so a demanding game at a high resolution can go from running out of graphics memory to running out of the pool. Give the larger memory to the game that needs it rather than to every game. A line at launch says which one a game got (`Memory layout:` in `stderr.log`).

## Memory

A game's console has 4 GiB of memory (or the larger layout chosen under **Memory**), and a game is
handed most of it. ProsperoEden takes the console's memory for it only where the game writes: a part
that was never written, or that the game's console cleared or freed whole, reads as zeroes from one
shared block and takes nothing, and a part that holds only zeroes again is given back now and then.
A 64-bit game holds 600 MiB to 2 GiB less of the console's memory this way, measured at the same
frame rate; a 32-bit game keeps the whole block. **Compact memory** does the same in 16 KiB pieces
inside memory the game wrote.

The launch log says it (`EDEN_CRAM on` in `stderr.log`, and every 30 s what is held), and
development builds can switch it off with `cram=off` in `dev-settings.txt`.

## Post-processing

**Settings > Video > Post-processing** draws a look over the game's picture: one of Eden's presets of ReShade FX effects, or off (the default). Each game can have its own in **Library > Game settings > Video**, as the other video settings.

Six presets come with the app:

| Preset | What it does |
|---|---|
| Anime | Cleans the picture, then flat bands of light with inked edges and a small glow |
| Cinematic | Soft glow on highlights, a filmic contrast curve, cool shadows and warm highlights, fine grain, a gentle vignette |
| Clean | Removes dithering and compression noise, smooths banded skies, brings edge detail back; a safe start |
| Dreamy | Soft focus: the centre stays sharp while the rest blurs, with a wide glow over highlights |
| Retro | An old television: colour fringing at the edges, curved scanlines with a phosphor mask, darker corners |
| Vivid | Warmer, more saturated colour, deeper blacks and brighter whites, a light sharpen |

The presets are files in `/data/prosperoeden/user/post_presets` (`.fxp`; the app writes the six there the first time), built from the effects in `/data/prosperoeden/user/post_shaders` (`.fx`).

| Folder | What goes in it |
|---|---|
| `/data/prosperoeden/user/post_presets/` | Presets (`.fxp`): a name, a description and the chain of effects |
| `/data/prosperoeden/user/post_shaders/` | ReShade FX effects (`.fx`) and the files they include (`.fxh`) |
| `roms/<game>/reshade/` | One game's own presets and effects, in its game folder (see [Game files folder](INSTALL.md#game-files-folder)) |

A game's own `reshade/` folder works like both folders at once: its presets (`.fxp`) are offered in that game's settings only, listed first, and the effects it holds (`.fx`, `.fxh`, and textures in `Textures/`) are used for that game, before an effect of the same name in `post_shaders/`. So a game folder can carry a preset made for it together with the effects it needs.

Restart ProsperoEden after adding files: the list is read when the menu opens. A ReShade `.ini` preset does not load as it is; write its effects into a `.fxp` chain instead (below). A preset file of your own in that folder is listed beside them by its name: its `chain=` line names the effects in order, each with its technique and values (`Bloom.fx|Bloom|Radius=1.2,Amount=0.25;Sharpen.fx|Sharpen|Amount=0.75`). An effect it names that the console does not have is left out.

- It is drawn by the **Vulkan** renderer; with OpenGL the game shows as it is.
- It applies when a game starts. A line at launch (`Post-processing:` in `stderr.log`) names the preset, or says why it is off.
- Each effect costs GPU time every frame, more at higher resolutions and at 4K output. A game that is already at its limit can lose frame rate.

## Frame generation

Frame generation shows, between two pictures a game draws, one that is made up from them. A game that runs at 30 FPS then looks like 60. It does not make the game itself faster, it adds a little delay between a button and its effect, and fast motion can show faults in the made-up pictures. It works with the Vulkan renderer.

**Frame gen method** chooses how the pictures are made: **MihawkGen**, a neural network that comes with the app and needs nothing else, or Lossless Scaling. A method that is not available on the console is skipped.

Lossless Scaling's frame generation ([Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)) cannot be included with ProsperoEden: for it you need your own copy of that program.

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

## Profiles

**Settings > Profiles** lists the people who play on this console, up to eight. Each profile keeps its own save data, its own settings (everything under Settings, and each game's own settings) and its own Continue Playing and Recently Played. The game files folder is the console's and is the same for everyone.

- **Cross** plays as the highlighted profile. The home screen names who is playing once there is more than one profile.
- **New profile** adds one. It starts with the settings of the profile that made it, and with no save data.
- **Left and right** change a profile's name: the names of the PS5 users signed in, then "Player 1" to "Player 8".
- **Square**, pressed twice, takes a profile off the list. Its save data stays on the console.

Each profile can keep its save data on a server of its own; see [Save sync](SAVES.md#save-sync).

The menu opens with the profile that the PS5 user in front chose last. If you used an earlier version, your saves, settings and recently played games are the first profile's, where they always were.

## Settings per game

**Settings** holds what every game uses. A game can differ from it: in the Library, press Triangle on the game (or choose **Game settings** on the home screen, for the game you played last). Its settings are a page that lists **Video**, **Performance**, **Audio**, **Controls**, **System** and **Language**, and each one says how many of its settings the game changes. Inside, a setting shows **Default** with the value Settings has until you choose another one for this game; going past the last choice comes back to **Default**. A change applies the next time the game starts.

| Kind | What a game can have of its own |
|---|---|
| Video | Renderer, resolution, upscaling filter, ASTC textures, refresh rate, FPS overlay, frame generation with its target and multiplier |
| Performance | Each of the switches, and the speed limit. The game's console memory is one value for every game in **Settings > Performance > Memory**; a game's own is in the settings file ([Console memory](#console-memory)) |
| Audio | Game volume, mute |
| Controls | Vibration, and a button mapping of its own |
| System | RNG seed: with a seed the game is loaded at the same place in memory at every start, which some mods need. Left and right switch between **Random** and the seed; Cross opens the keyboard to type another one. The game's own random numbers still change from start to start |
| Language | The language the game is given |

The output resolution, the menu's sounds and the accessibility settings are the same for every game.

## Button mapping

**Settings > Controls > Vibration strength** sets how strong the DualSense vibrates, from 25% to 100% of what a game asks for; **Vibration** turns it off altogether.

**Settings > Controls > Button mapping** lists the game's buttons (A, B, X, Y, L, R, ZL, ZR, Plus, Minus and the two stick presses) with the DualSense button that presses each one. Left and right choose another DualSense button; when another game button had it, the two swap, so one DualSense button never presses two game buttons. Square puts every button back. The mapping applies to every controller.

A game can have its own: in its settings, **Controls**, set **Button mapping** to **This game**, then press Cross to change it. It starts as a copy of the one in Settings.

The D-pad, the sticks and the [in-game shortcuts](../README.md#in-game-shortcuts) do not change. With the usual mapping the touchpad is the game's Minus; the Create button presses whatever the touchpad presses, unless it has a game button of its own.

## Language and accessibility

The launcher follows the language the PS5 is set to: Arabic, Chinese (simplified and traditional), Czech, Danish, Dutch, Finnish, French, German, Greek, Hungarian, Indonesian, Italian, Japanese, Korean, Norwegian, Polish, Portuguese, Romanian, Russian, Spanish, Swedish, Thai, Turkish, Ukrainian and Vietnamese (with the regional variants the PS5 has for French, Portuguese and Spanish), and English otherwise. Arabic, Chinese, Greek, Japanese, Korean and Thai are drawn with the PS5's own system fonts. Arabic text runs right to left; the screens themselves are not mirrored. To use another one, put a file named `language.txt` holding its tag (for example `en-US` or `pt-BR`) in `/data/homebrew/PPSA99008`. The language *games* use is a separate setting, **Settings > Language**.

**Settings > Accessibility** has three switches. **Larger text** draws the menu's small text about a third larger. **High contrast** uses solid panels, brighter text and an outlined highlight. **Reduce motion** stops the background drifting and the screens sliding, in the menu and on the loading screen. There is no screen reader.

## Update notices

**Settings > Diagnostics > Update notices** (on by default) is the offer at the start when [homebrew.page](https://homebrew.page/ps5) lists a newer release of ProsperoEden: the update dialog, or the **Update available** notification when the app cannot install it itself (see [Updating](INSTALL.md#updating)). **Skip** answers one start; switched off, the app opens without the offer every time. It still asks homebrew.page, and the About screen names the newer version beside the one running. In the settings file it is `"system": { "update_notices": false }`, and it applies the next time ProsperoEden opens.
