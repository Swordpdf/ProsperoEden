# Runtime launcher packs (test implementation)

ProsperoEden can load an external launcher at runtime. The built-in launcher remains the
default and recovery interface. A pack supplies Lua presentation code, images, fonts,
recordings and a loading-screen description. It does not replace the emulator or load
native plugins.

Runtime launcher packs are included in ProsperoEden v1.003.000 as an experimental feature.
The built-in launcher remains available as the recovery interface.

## Installation and selection

Install a pack as `/data/prosperoeden/launchers/<id>/manifest.json`, with its resources in
the same folder. IDs contain lowercase ASCII letters, digits and hyphens, at most 48
characters. Resources use relative paths. Symbolic links and parent-path traversal are
rejected.

In the built-in launcher's Settings, open Appearance. Choose Theme for the built-in
color-theme selector, or Launcher for installed launcher packs. Back returns from
Theme or the launcher picker to Appearance, and then to Settings. Options remains a
shortcut to the launcher picker from Settings. Each external launcher should expose
`launcher.picker` or `launcher.select` in its own settings.

On PS5 the host enumerates packs and sound folders using 64 KiB native directory
reads. Resource validation uses no-follow file descriptors rather than `lstat`,
which the app sandbox can deny for readable files. Missing or invalid packs show a
reason in the launcher picker and emit `PE_LAUNCHER_DISCOVERY` / `PE_LAUNCHER_SKIPPED`
diagnostics to stderr. Discovery changes are logged once, without per-frame log spam.

The selection is saved in `/data/prosperoeden/config/launcher.json`. Pack-specific UI
state is saved in `/data/prosperoeden/config/launcher-state/<id>.json`. The emulator's
profiles, preferences, game files, saves and remote-source configuration keep their
existing locations. No games or keys belong in a launcher pack.

To recover without a working pack, create the empty file
`/data/prosperoeden/config/force-builtin-launcher`. Remove it when recovery is complete.
Script failures also restore the built-in launcher for the current session, preserving
the pack and its saved selection for repair.

## Manifest

```json
{
  "schema": 1,
  "id": "grid-example",
  "name": "Grid example",
  "host_contract": "launcher-v1.0",
  "api": { "major": 1, "min_minor": 0 },
  "entry": "main.lua",
  "state_schema": 1,
  "required_capabilities": ["draw.atlas.v1", "actions.launch.v1", "state.v1"],
  "assets": {},
  "font": "assets/font.pefont",
  "sounds": ""
}
```

Optional `loading_scene` names a JSON file. `assets` maps logical image names to TGA
files. The font is a ProsperoEden `.pefont`; native language fallback fonts are attached
by the frontend. `sounds` names a directory of WAV recordings. The example in
`examples/grid-launcher` uses only the upstream licensed font and neutral presentation.

## Script and data API

`main.lua` returns a table with `init(event)`, `draw(event)` and optional `input(event)`,
`update(event)` and `shutdown(event)`. `init` and `draw` are required. `init` receives
`state`, `first_start`, `api_major` and `api_minor`. `update` receives `dt` and `time`;
`draw` receives `time`. `input.key` is one of `cross`, `circle`, `square`, `triangle`,
`options`, `l1`, `r1`, `up`, `down`, `left`, `right`.

Input keys name physical buttons, preserving the `launcher-v1.0` contract. Packs may
read `host.query("preferences").menu_bind` to follow the user's native menu bindings:
the four array entries assign Select, Back, Secondary and Game settings respectively
to face buttons 0 (Cross), 1 (Circle), 2 (Square) and 3 (Triangle). A pack that offers
its own bindings should keep its hints consistent with its input handling. The native
launcher picker follows the native menu bindings, including its Select and Back hints.
`preferences.set` accepts a `menu_bind` array containing each of those four integers
exactly once. Partial preference patches retain the current menu bindings.

`status.update` holds the newer application release the start-up check found, whatever
the user chose. `preferences.update_notices` is **Settings > Diagnostics > Update
notices** (true by default): when it is false, a pack should not offer the release when
it opens, only name it where it shows the running version. The built-in launcher that a
pack falls back to follows it.

`choices.speed_limit` lists the speed-limit steps as percentages, `0` meaning no limit.
`preferences.speed_limit` and a game setting's `speed_limit` are indexes into that list;
the game setting is `-1` while the game follows Settings.

Three functions cross the host boundary:

```lua
local model = host.query("library")
local result = host.action("launch.request", {game=model.games[1].handle})
host.draw("text", {text="Library", x=120, y=120, size=36, color=0xffffff})
```

Arguments and results are data tables; native pointers do not cross the boundary.
Actions return `{ok, message}` or `{ok=false, error}`. Queries return their model or an
error table. Render errors return an error table. Check these results when an operation
can fail. Services and serialized fields are defined in
`headless/prosperoeden/pe/ui/services.hpp` and `runtime_models.hpp`.

| Queries | Purpose |
| --- | --- |
| `library`, `home`, `profiles` | Library snapshots, recents, setup status and current profile |
| `preferences`, `choices`, `game_settings` | Global and per-game settings and supported choices |
| `status` | Clock, real controller mask, multiplayer phase, launch/sync/update/keyboard state |
| `game_files`, `mods`, `cheats`, `setup` | Folder information, a game's mods (each with `cheats_only` when it holds cheats alone), its cheats with the Cheats switch, and setup details |
| `sources`, `downloads` | Remote-source catalog and native download queue |
| `save_sync`, `save_sync_setup`, `pair_servers`, `pairing` | Native save synchronization and pairing |
| `room_form`, `public_rooms` | Room form without a saved password, cached public directory |
| `state`, `launchers`, `translate`, `measure` | UI state, installed launchers, localization and font measurement |

| Actions | Purpose |
| --- | --- |
| `launcher.select` (`id`), `launcher.picker`, `state.save` | Selection and pack-specific UI state |
| `library.refresh` | Rescan through the existing game service |
| `profile.choose` (`handle` or `index`), `profile.add`, `profile.rename` (`index`, `step`), `profile.remove` (`index`) | Native profiles |
| `preferences.set` (field patch), `game_settings.set` (`game`, `patch`), `game.docked` (`game`, `value`) | Existing settings storage |
| `game_files.set` (`path`) | Existing game-files folder selection |
| `mods.enabled`, `mod.enabled`, `cheats.enabled`, `cheat.enabled` (`mod`, `name`), `mods.folder` | Native mod and cheat operations, using a library `game` handle |
| `save.import`, `save.export` (`game`) | Native save transfer |
| `sources.refresh`, `download.start` (`game`, `source`, `first`), `download.cancel` (`key`), `game.delete` (`game`, `confirmed`) | Native remote and file operations |
| `room.refresh`, `room.leave`, `room.join` (connection fields), `room.join_public` (`index`, `nickname`, `password`) | Existing multiplayer services |
| `keyboard.open` (`field`, `text`), `keyboard.seed` (`game`), `keyboard.cancel` | Native asynchronous text/seed entry |
| `pair.start` (`profile`, `server`), `pair.cancel`, `pair.unlink` (`index`) | Native save-server pairing |
| `sync.answer` (`choice`), `sync.finish`, `sync.cancel` | Conflict, retry and failure decisions |
| `launch.request` (`game`), `launch.cancel`, `launch.commit` | Validated game launch lifecycle |
| `update.start`, `update.cancel`, `update.apply` | Existing application updater |
| `sound` (`event`, optional `variation`, `pitch`, `gain`) | Named recordings on the existing UI mixer bus |

`game` is a handle from the current library snapshot, never an arbitrary executable
path. `source` and public-room/pairing indices are zero based. Library generation
changes require the pack to refresh its displayed model. Profiles include a stable
native `handle`; use it instead of a display name.

`launch.request` captures the current profile and enters `ready` or `syncing`. A pack
must wait for `ready` before its animation finishes and calls `launch.commit`. The host
rechecks game existence, the captured profile and room readiness, resolves the native
path and calls `will_play` exactly once. A script cannot commit while a conflict or
failure needs an answer. `sync.answer.choice` accepts `console`, `server`, `neither`,
`retry` or `play_anyway` when that answer is available. Do not silently choose a side.

## Rendering, recordings and loading screens

The drawing canvas is 1920 by 1080, scaled by the existing frontend. Coordinates are
pixels; colors are `0xRRGGBB`, with separate alpha. Commands include `rect`, `border`,
`gradient`, `shadow`, `circle`, `ring`, `line`, `triangle`, `text`, `image`, `cover`, `qr`,
`clip`/`unclip`, `transform`/`untransform` and `opacity`/`unopacity`. Common rectangles
use `x,y,w,h`; images use a logical `asset` and normalized `uv={x,y,w,h}`. Covers use
the current library `game` handle and an optional `fallback` color for missing art.
Fonts are measured, fitted and shaped natively.

Recordings are named `<event>_01.wav`, with optional variations `_02` to `_08`.
Playback defaults to the recording's original pitch, uses the native WAV decoder and
obeys menu volume. Retired audio banks are released only after the mixer's stop fence
acknowledges that old clip references are no longer in use.

A loading description uses `schema:1`, `atlas`, `background` and up to 16 `layers`.
Each layer has `kind` (`image` or `border`), `rect`, optional `uv`, `color`, `alpha`,
`radius`, `width`, `period`, `y_from`, `y_to`, `move_start`, `move_end`, `fade_in`,
`fade_out_start`, `fade_out_end`. Negative rectangle x/y anchor to the right/bottom.
Animation tracks use seconds. Reduce motion displays their settled state. The atlas
must be at least 512 pixels wide; its first row is reserved for validated metadata.

The frontend copies the prepared loading scene into owned CPU data before releasing
the Lua VM and menu textures. OpenGL and Vulkan upload independent copies and use the
same generic shader. Gameplay and background workers never invoke Lua.

## Limits and updates

The VM allows base, math, table, string and UTF-8 functions. It has no file, process,
network, debug or native-module library, and cannot load code dynamically. Scripts are
text only, at most 2 MiB. Each callback has 300,000 Lua instructions and a 16 MiB VM
budget. Images, fonts, WAV data, loading layers, API tables and draw-command counts
also have explicit limits. Install packs from authors you trust: packs can request
application operations exposed by Services, including settings changes and confirmed
file deletion. The VM is a presentation boundary, not a replacement for reviewing a
pack's behavior.

App packages include `launcher-runtime.json`. The helper checks its `supports` contract
list both after staging and immediately before application. An incompatible release
is refused while an external pack is selected. The user may explicitly select the
built-in launcher to proceed. Packs and UI state live outside the app folder, so a
compatible app update replaces engine files without replacing them.

This does **not** make an unmodified official release support runtime packs. Automatic
updates preserve a selected pack only when the destination release includes this
runtime and declares its compatible contract, API and capabilities. Optional
[launcher pack updates](LAUNCHER_UPDATES.md) provide separate HTTPS feeds, automatic
checks and confirmed installation without replacing the application. Maintainers must preserve an existing contract or introduce
a new contract with a migration; adding unrelated engine features does not require
recompiling packs.

## Desktop testing

### Authoring and maintenance

Copy `examples/grid-launcher` into a new pack folder and change its manifest ID to match
that folder. Implement `input`, `update` and `draw` in Lua, using the host queries and
actions above. Editing the pack does not require compiling the PS5 app; reselect it to
reload its files. The desktop renderer provides a faster development loop with fake
games and services. Real game launch, audio and console lifecycle still need PS5 testing.

This is a programmable launcher API, not a visual editor or HTML/CSS renderer. A complete
replacement must implement navigation, accessible presentation and the workflows it
exposes. The built-in Theme list remains a compiled color-theme table. A runtime pack
may provide its own palettes and asset variants, as part of its Lua presentation.

For a new upstream version, maintainers build the app once with the runtime integration;
they do not compile each installed launcher. Check changes to the Services models,
frontend lifecycle and updater, adapt the bridge, and run the renderer and workflow
checks. Existing packs can remain unchanged when `launcher-v1.0` keeps its behavior.
New host settings do not automatically appear in old packs. Breaking changes need a new
contract or an adapter, rather than declaring an incompatible implementation compatible.
The application updater does not download launcher updates; the separate native pack
updater handles packs that opt into the [update-feed format](LAUNCHER_UPDATES.md).

The host build needs a C++20 compiler, EGL/OpenGL development libraries and
nlohmann/json development headers (3.11 or newer), OpenSSL and libcurl development files. The existing dependency tool fetches
the pinned HarfBuzz and UI kit; Lua is vendored in the source tree.

`tools/launcher/runtime-preview.sh <output-dir> <width> <height>` builds an interactive
stdin-driven renderer with fake services. Set `PE_LAUNCHER_DATA` to a test data root.
It accepts `select <id>`, `key <name> [frames]`, `tick <frames>`, `capture <absolute.png>`,
`picker`, `recreate`, `status` and `exit`. Additional commands exercise loading,
profiles, save-sync, disappearing games and updater state. This is a renderer/API test;
console pairing, actual game execution, resolution changes and real update installation
still require PS5 testing.

Run the reproducible VM and launch-workflow checks against that preview binary:

```sh
python3 tools/launcher/test-runtime-faults.py /path/to/pe_preview
python3 tools/launcher/test-runtime-workflows.py /path/to/pe_preview
python3 tools/launcher/test-runtime-filesystem.py
```

The tests use temporary neutral packs and simulated native services, and leave the
console's data directory untouched.
