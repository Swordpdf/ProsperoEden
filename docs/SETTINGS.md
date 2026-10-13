# Settings

The settings that take more than a line to explain: the Performance switches, frame generation, profiles, settings per game, button mapping, language and accessibility.

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

## Frame generation

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
| Performance | Each of the seven switches |
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
