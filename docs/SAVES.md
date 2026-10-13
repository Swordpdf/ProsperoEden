# Save data

Moving a game's save data in and out of ProsperoEden, and keeping it in step with a server.

## Moving save data

Emulators like Eden keep a save as the files the game wrote, so nothing is converted: folders are copied. In the Library, press Triangle on a game and pick **Save data**. The folders below are next to `roms/` in the game files folder.

- **Import from a folder.** Copy the contents of the game's save folder (what an emulator opens as the game's save directory) into `save-import/<title ID>/`. The title ID is the 16-character code in the game's file name; Save data also shows it when there is nothing to import.
- **Import from Ryujinx.** Copy Ryujinx's data folder (the one that holds `bis/`, or a portable folder around it) to `ryujinx/`. ProsperoEden finds the game's save in it: the first user's, and the device save when there is one.
- **Export.** Square writes `save-export/<title ID>-<date>-<time>/`, with `account/` and `device/` inside. An exported folder can be imported again: put it in `save-import/` under the title ID.

Cross imports, and asks before it replaces a save. The save it replaces is first moved to `/data/prosperoeden/backup/save-import`, so nothing is lost.

## Save sync

Each profile can keep its save data on a server, such as a [RomM](https://github.com/rommapp/romm) server (5.0.0 or newer), the way Steam keeps it in the cloud: before a game starts, its save data is put in step with the server's, and after the game ended the new save data goes to the server. Played on another device in between (Eden on Android with [Argosy](https://github.com/rommapp/argosy-launcher), or another console), the newer save data comes to the console; changed on both sides, ProsperoEden asks which one stays. Save sync is set up for each profile and does not depend on the download sources: games copied to the console by hand sync too, as long as the server has the game.

- **Pair a profile, without typing.** **Settings > Save sync** lists the profiles and the server each one keeps its save data on. Cross on a profile pairs it with one of the download sources' servers (asked first when several can): the dialog shows a QR code and a short code. Scan it with your phone (or open the address under it), sign in to the server as the user this profile's save data belongs to, check that the code is the same and approve. The console gets a sign-in of its own for that user and writes it into the profile's entry; the code lasts ten minutes. Square, pressed twice, unlinks a profile: its save data stays on the console and on the server. Each profile can be paired with a user of its own, so two people's save data stay apart. Pairing needs the server among the download sources (`sources.json`); without one, set it up over FTP as below.
- **Or set it up over FTP.** ProsperoEden writes `/data/prosperoeden/config/remote/save-sync.json` with an entry for every profile, and keeps it in step with the profiles: a new profile gets an empty entry, a removed one's entry goes, and a renamed one's `"__profile_name"` changes. Fill in the entry of each profile that should sync (over FTP, like the keys), with the server's `"type"`, `"url"` and sign-in (see [RomM](#romm) for a RomM server):

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

### RomM

`"type": "romm"` keeps the save data on a [RomM](https://github.com/rommapp/romm) server with save sync, RomM 5.0.0 or newer (checked with 5.0.0 to 5.3.1). With an older server nothing is synced, and every sync says so in a dialog to confirm, with the server's version and the one it needs. Paired from Settings > Save sync, RomM's device authorization does the signing in: the code is approved on the server's page `/pair/device`, and RomM makes a device of the console with a client API token bound to it. Set up over FTP instead, its entry in `save-sync.json` has the server's `"url"` and a `"token"` of the profile's own RomM user: in RomM, create a client API token (your profile, **Client API tokens**) with the scopes `platforms.read`, `roms.read`, `assets.read`, `assets.write`, `devices.read` and `devices.write` (and `me.read`, for the log to name the user); a scope that is missing is named when a sync fails. `"username"` and `"password"` work instead of `"token"`, and `"platform"` names the platform's slug when the server does not call it `switch`. The console is registered once as a device of that user, **ProsperoEden (PS5)**, and RomM knows from it which version of each game's save data the console last had. A game's save data is in its slot `autosave`, for the emulator `eden`, as Argosy keeps Eden's; the game's other slots and other emulators' save data are left alone. Which side changed is told by the save data itself, not by the times RomM compares: the console keeps what it last had in step (`config/remote/save-sync/<profile>/synced.json`). Save data deleted in RomM is uploaded again by the console that still has it.
