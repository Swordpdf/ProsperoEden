# Starting a game from a home screen forwarder

A forwarder is a separate small app with its own home screen tile (its own title ID, icon and
name). When it is opened, it launches ProsperoEden (`PPSA99008`) with launch arguments, and
ProsperoEden starts that game directly instead of opening its launcher.

## Arguments

| Argument | Meaning |
| --- | --- |
| `--rom <file>` or `--rom=<file>` | The game to start. An absolute path (`/mnt/usb0/eden/roms/Game.nsp`), or a path inside the game files folder's `roms/` (`Game.nsp`, `rpg/Game.xci`). A relative path may not contain `..`. |
| `--exit-after-game` | When that game ends normally, close ProsperoEden so the console returns to the home screen. Without it, the library opens. |

Unknown arguments are ignored. Parsing is in `headless/forwarded_launch.h`; `headless/main.cpp`
uses it after filesystem access is set up, so `roms/` resolves against the folder chosen in
**Settings > Game files**.

## Behaviour

- The forwarded game starts once. Quitting it (Touchpad + L1) opens the library, or, with
  `--exit-after-game`, ends ProsperoEden.
- If the file is missing (or filesystem access was not granted, so the path cannot be read), the
  launcher opens and shows `Forwarded game not found: <path>`.
- After a crash, ProsperoEden restarts without arguments and shows the crash notice; the
  forwarded game is not started again.
- Arguments only reach a new ProsperoEden process. If ProsperoEden is already running, the
  system brings it to the front and `main` does not run again; a forwarder should close it
  first.

## The Library's games: library.json

A forwarder maker can list the games ProsperoEden has, with their names and covers, from
`/data/prosperoeden/library.json`. ProsperoEden writes it each time its Library is read (when the
launcher opens, once keys and firmware are set up, and when games are added or deleted), in one
step, so a reader never sees half of it. It
holds the games on the console, not those that can only be downloaded from a source.

```json
{
  "version": 1,
  "app": "PPSA99008",
  "game_files": "/mnt/usb0/eden",
  "games": [
    {
      "file": "Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp",
      "path": "/mnt/usb0/eden/roms/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp",
      "title": "Mario Kart 8 Deluxe",
      "title_id": "0100152000022000",
      "cover": "/data/prosperoeden/covers/1a2b3c4d.tga"
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `version` | The format. Later versions only add fields; a reader of version 1 reads them as well. |
| `game_files` | The game files folder in use (**Settings > Game files**), which holds `roms/`. |
| `file` | The game's path in `roms/`: what to pass to `--rom`. |
| `path` | Its full path. |
| `title` | The name the Library shows. |
| `title_id` | 16 hexadecimal digits, or `null` when the game's data could not be read (keys missing, for one). |
| `cover` | Its cover, a TGA image, or `null` when it has none yet. |

Without filesystem access ProsperoEden cannot write it, and the file stays as it last was.
