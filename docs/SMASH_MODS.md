# Smash Ultimate 13.0.4 mods

A user completed a full battle with the Infernape moveset on PS5 using the
compatibility fixes in this change and the versions below. Other characters and
combinations need their own tests. The upstream rebase has not been tested on PS5.

## Tested framework versions

| Component | Version |
| --- | --- |
| Super Smash Bros. Ultimate | 13.0.4 |
| Skyline | Official beta |
| ARCropolis | [4.0.8](https://github.com/Raytwo/ARCropolis/releases/tag/v4.0.8) |
| Smashline | [1.6.6](https://github.com/HDR-Development/smashline/releases/tag/v1.6.6) |
| Param Hook | [13.0.4](https://github.com/ultimate-research/params-hook-plugin/releases/tag/v13.0.4) |
| CSK Collection | 5.0.4 |

Use [Skyline's official releases](https://github.com/skyline-dev/skyline/releases)
for `skyline.zip`. ARCropolis 4.0.8 embeds its menu resources. ARCropolis 4.1.1
and Smashline 1.6.7/1.6.8 target Smash 13.0.5 and should not replace these pins
for a 13.0.4 installation. API exports alone do not prove game-version compatibility.

## Install

Close the emulator before copying files. In the selected **Settings > Game files**
folder (default `/data/prosperoeden`), use:

```text
mods/01006A800016E000/Skyline/
  exefs/main.npdm
  exefs/subsdk9
  romfs/skyline/plugins/
    libarcropolis.nro
    libsmashline_plugin.nro
    libparam_hook.nro
    libthe_csk_collection.nro
```

Enable **Skyline** in the game's Mods screen. Keep one active installation of
each framework. Select only the CSK core plugin for an initial test; its ZIP also
contains optional mods and flags.

Character packs belong on the emulated SD card:

```text
/data/prosperoeden/user/sdmc/ultimate/mods/(Moveset) Infernape/
  config.json
  plugin.nro
  fighter/...
  effect/...
  ui/...
```

Keep the pack's own `plugin.nro` inside the pack folder. The emulated SD root
stays under `/data/prosperoeden/user` when Game files is moved to another drive.
Infernape uses Lucario's added `c80` slot and requires the companion plugins above;
a basic costume replacement may need only Skyline and ARCropolis.

## Optional staging helper

With Python 3.11+, prepare a new transfer directory without changing an installation:

```bash
python3 tools/stage-smash-mods.py \
  --skyline /path/to/skyline.zip \
  --arcropolis /path/to/arcropolis-4.0.8-release.zip \
  --plugin /path/to/libsmashline_plugin.nro \
  --plugin /path/to/libparam_hook.nro \
  --plugin /path/to/libthe_csk_collection.nro \
  --character '/path/to/(Moveset) Infernape' \
  --output results/smash-upload/data/prosperoeden
```

Copy the staged `mods/` to the selected Game files folder and `user/sdmc/` to
`/data/prosperoeden/user/sdmc/`. The manifest records SHA-256 hashes. The helper
checks archive paths and file headers; it does not download mods, choose dependency
versions, verify licensing, or establish gameplay compatibility.

Add packs one at a time and complete a match before combining them. Two packs
replacing the same `ui_chara_db.prc` need an author's supported merged setup.

## Compatibility fixes and regression checks

- Propagate guest instruction-byte cache invalidations across cores so Skyline's
  executable-memory hooks cannot recompile stale instructions.
- Request the timing thread's stop before waking it, preventing a lost-wake
  deadlock during shutdown.
- Build shader catalog keys lexically, avoiding PS5's denied canonicalization.

`tools/check-guest-hooks.py` exercises four guest cores and writable/executable
aliases. `tools/check-timing-stop.py` forces the original lost-wake schedule and
checks clean joins. `python3 tools/test-stage-smash-mods.py` validates staging.
