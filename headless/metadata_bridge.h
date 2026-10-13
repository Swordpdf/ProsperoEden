#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EDEN_METADATA_TITLE = 1,
    EDEN_METADATA_COVER = 2,
};

// Cached for this process; restart after replacing setup files. Empty means ready.
const char* eden_startup_error(void);
uint64_t eden_game_title_id(const char* rom_path);

int eden_extract_game_metadata(const char* rom_path, const char* keys_dir,
                               const char* cover_tga_path, char* title,
                               size_t title_capacity);

// A cover in a picture file's bytes (PNG or JPEG, as a download source sends it) as the TGA the
// launcher draws, at most 512 pixels a side. Nonzero when it was written.
int eden_write_cover_tga(const unsigned char* encoded, size_t size, const char* cover_tga_path);

// The languages the game declares in its own control data (NACP flags: bit n is NS
// ApplicationLanguage n), or 0 when they cannot be read.
uint32_t eden_game_supported_languages(const char* rom_path, const char* keys_dir);
// The language the game will use when the player chose `chosen` (an index of Eden's
// Settings::Language): that language or a variant of it when the game has one, else English,
// else the next one in the console's fallback order. The game's languages are read from
// its update's control data when the last eden_scan_addons found one, else from its own; `chosen`
// itself when the game's languages are unknown.
int eden_game_language(const char* rom_path, const char* keys_dir, uint64_t title_id, int chosen);

// Update and DLC files (NSP or XCI, any depth) in updates_dir, read with the provider that also
// applies them to a running game. Replaces the previous scan; eden_game_addons queries it.
void eden_scan_addons(const char* updates_dir, const char* keys_dir);
// The update and DLC files of a game in updates_dir (NSP or XCI, any depth), by the title IDs
// they hold: each one's full path is handed to found. Returns how many there were.
typedef void (*eden_found_file)(void* user, const char* path);
int eden_game_addon_files(uint64_t title_id, const char* updates_dir, const char* keys_dir, eden_found_file found,
                          void* user);
// For a base game: the newest update's display version (empty without one) and its DLC count.
// Returns nonzero when either exists.
int eden_game_addons(uint64_t title_id, char* update_version, size_t capacity, unsigned* dlc_count);

// Save transfer for one game (ryujinx_saves.h). Sources, in the game files folder next to roms/:
// save-import/<title ID>/ (a save folder copied by hand) or ryujinx/ (a Ryujinx data folder).
enum {
    EDEN_SAVE_NONE = 0,      // nothing to import for the game
    EDEN_SAVE_FOLDER = 1,    // save-import/<title ID>/
    EDEN_SAVE_RYUJINX = 2,   // ryujinx/
};
int eden_save_import_source(uint64_t title_id);
enum {
    EDEN_SAVE_DONE = 0,
    EDEN_SAVE_NOTHING = 1,   // no source (import), or the game has no save yet (export)
    EDEN_SAVE_NO_USER = 2,   // ProsperoEden has not created its user: start any game once
    EDEN_SAVE_FAILED = 3,    // the copy failed; the current save is unchanged
};
// Copies the game's saves from the source into ProsperoEden's, after moving the current ones to
// /data/prosperoeden/backup/save-import (path receives that folder, or stays empty when the game
// had no save).
int eden_save_import(uint64_t title_id, char* path, size_t capacity);
// Copies the game's saves to save-export/<title ID>-<date>-<time>/ (account/ and device/) in the
// game files folder; path receives that folder.
int eden_save_export(uint64_t title_id, char* path, size_t capacity);

#ifdef __cplusplus
}
#endif
