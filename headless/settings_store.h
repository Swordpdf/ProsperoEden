// SPDX-License-Identifier: GPL-3.0-or-later
// Everything the launcher remembers, in one JSON file (config/prosperoeden.json):
//
//   {
//     "version": 1,
//     "video": { "renderer": "vulkan", "fps_overlay": true, "resolution": "1x",
//                "upscaling_filter": "bilinear", "astc_textures": "compressed",
//                "refresh_rate": "60",
//                "output_resolution": "1080p", "frame_gen": false, "frame_gen_method": "lsfg",
//                "frame_gen_target": "auto", "frame_gen_multiplier": "2", "post_processing": "Cinematic" },
//     "audio": { "volume": 100, "mute": false, "menu_volume": 70 },
//     "controls": { "vibration": true, "vibration_strength": 100,
//                   "mapping": { "a": "cross", "b": "circle" } },
//     "system": { "language": "en-US", "update_notices": true },
//     "accessibility": { "large_text": false, "high_contrast": false, "reduce_motion": false },
//     "launcher": { "theme": "canopy" },
//     "diagnostics": { "detailed_logging": false, "immediate_logs": false },
//     "performance": { "block_list": false, "async_shaders": false, "fast_gpu": false,
//                      "unsafe_cpu": false, "unsafe_dma": false, "reactive_flushing": true,
//                      "skip_invalidation": false, "compact_memory": false, "memory_layout": "4Gb",
//                      "speed_limit": "100" },
//     "game_files": "/mnt/ext1/eden",
//     "library": { "last_game": "Game [id].nsp", "recent": ["Game [id].nsp"] },
//     "games": { "0100000000010000": { "console_mode": "handheld", "renderer": "opengl",
//                                      "resolution": "0.75x", "upscaling_filter": "fsr",
//                                      "astc_textures": "full",
//                                      "refresh_rate": "120", "fps_overlay": false,
//                                      "frame_gen": true, "frame_gen_method": "mihawkgen",
//                                      "frame_gen_target": "120", "frame_gen_multiplier": "3",
//                                      "volume": 80, "mute": false, "vibration": false,
//                                      "language": "ja", "mapping": { "a": "cross" },
//                                      "controller": "handheld",
//                                      "mods": false, "cheats": false,
//                                      "mods_off": ["A mod's folder name"],
//                                      "cheats_on": ["A mod's folder name#A cheat's name"],
//                                      "cheats_off": ["A mod's folder name#Its one cheat's name"],
//                                      "performance": { "fast_gpu": true } } }
//   }
//
// Missing or mistyped values read as their defaults. Writes replace the file atomically. The
// text files of earlier versions (settings.txt, last-game.txt, recent-games.txt, assets-dir.txt,
// game-<title>-mode.txt) are read once into the JSON file and left in place.
#pragma once
#include "hud.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "button_mapping.h"
#include "storage_paths.h"
#include "multiplayer.h"

namespace Eden {
enum class GraphicsBackend { OpenGL, Vulkan };
inline const char* BackendName(GraphicsBackend backend) {
    return backend == GraphicsBackend::OpenGL ? "OpenGL" : "Vulkan";
}
// Settings > Video: the internal rendering resolution (a scale of the game's own 720p handheld
// or 1080p docked output) and the filter that scales the result to the TV output. 3x and 4x draw
// nine and sixteen times the game's own pixels: they need the graphics memory for it.
inline constexpr const char* kResolutionKeys[] = {"0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"};
inline constexpr const char* kResolutionLabels[] = {"0.5x (faster, softer)", "0.75x (faster)", "1x (native)",
                                                    "1.5x (sharper)", "2x (sharpest)", "3x (slower)",
                                                    "4x (slowest)"};
static_assert(std::size(kResolutionLabels) == std::size(kResolutionKeys));
inline constexpr int kNativeResolution = 2;
inline constexpr const char* kUpscalingFilterKeys[] = {"bilinear", "fsr", "bicubic", "nearest"};
inline constexpr const char* kUpscalingFilterLabels[] = {"Bilinear", "AMD FSR", "Bicubic", "Nearest"};
// Settings > Video: how a texture in ASTC, a format the console's GPU cannot read, is held.
// Compressed keeps it as BC3; full quality keeps it decoded, at four times the graphics memory
// (headless/main.cpp has the numbers).
inline constexpr const char* kAstcTextureKeys[] = {"compressed", "full"};
inline constexpr const char* kAstcTextureLabels[] = {"Compressed", "Full quality"};
static_assert(std::size(kAstcTextureLabels) == std::size(kAstcTextureKeys));
// Settings > Video: the refresh rate of the output while a game runs. 120 Hz is asked of the
// display (display_refresh.h); one that cannot show it stays at 60 Hz.
inline constexpr const char* kRefreshKeys[] = {"60", "120"};
inline constexpr int kRefreshHz[] = {60, 120};
// Lossless Scaling frame generation (Vulkan only): the multiplier is how many frames the driver
// makes out of each one the game draws, and the target is the rate it aims for instead. Eden
// ignores the multiplier once a target rate is set (FrameGenMaxGenerations in settings.cpp lets
// the pacer add up to its own maximum then), so "auto" passes 0 and leaves the ratio in charge.
// The target's limits are Eden's own: 0 to 240 Hz.
// Frame generation method: which generator to use (Vulkan only). LSFG needs the user's own
// Lossless.dll; MihawkGen uses its own neural network weights shipped in the build.
inline constexpr const char* kFrameGenMethodKeys[] = {"lsfg", "mihawkgen"};
inline constexpr const char* kFrameGenMethodLabels[] = {"Lossless Scaling", "MihawkGen"};
static_assert(std::size(kFrameGenMethodKeys) == std::size(kFrameGenMethodLabels));
inline constexpr const char* kFrameGenTargetKeys[] = {"auto", "60", "90", "120", "144", "240"};
inline constexpr int kFrameGenTargetHz[] = {0, 60, 90, 120, 144, 240};
static_assert(std::size(kFrameGenTargetKeys) == std::size(kFrameGenTargetHz));
inline constexpr const char* kFrameGenMultiplierKeys[] = {"2", "3", "4"};
inline constexpr int kFrameGenMultiplierValue[] = {2, 3, 4};
static_assert(std::size(kFrameGenMultiplierKeys) == std::size(kFrameGenMultiplierValue));
// Settings > Video > Overlay size: how large the overlay's chip is drawn. "regular" is the size
// every earlier build drew; "medium" is one and a half times it; "large" is twice it, for reading
// the line from across a room. The corner it stands in is a switch of its own
// (Preferences::hud_corner), and the order here is the one the launchers walk
// (hud.h's kHudCornerCount). The value is the chip's scale times two, as hud.h's field carries it.
inline constexpr const char* kHudSizeKeys[] = {"regular", "medium", "large"};
inline constexpr int kHudSizeValue[] = {2, 3, 4};
static_assert(std::size(kHudSizeKeys) == std::size(kHudSizeValue));
// Settings > Video > Overlay corner: which corner of the picture the chip is drawn against.
inline constexpr const char* kHudCornerKeys[] = {"top_left", "top_right", "bottom_left", "bottom_right"};
static_assert(std::size(kHudCornerKeys) == 4);
// Settings > Video: the size of the picture the app puts out, for the menu and for a game (its
// frame after the upscaling filter). The console scales it to what the TV shows.
inline constexpr const char* kOutputKeys[] = {"1080p", "1440p", "2160p"};
inline constexpr int kOutputWidth[] = {1920, 2560, 3840};
inline constexpr int kOutputHeight[] = {1080, 1440, 2160};
// Settings > Language: the system language games see, in launcher order. Each entry maps to Eden's
// Settings::Language and to the Settings::Region consoles sold with that language have (indices in
// Eden's enum order; headless/main.cpp checks them). Eden's older "Chinese" and "Taiwanese" codes
// are left out: games use Chinese (Simplified) and Chinese (Traditional) instead.
inline constexpr const char* kLanguageKeys[] = {"en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "pt",
                                                "pt-BR", "nl", "ru", "pl", "ja", "ko", "zh-Hans", "zh-Hant", "th"};
inline constexpr const char* kLanguageLabels[] = {"English (US)", "English (UK)", "French", "French (Canada)",
    "German", "Italian", "Spanish", "Spanish (Latin America)", "Portuguese", "Portuguese (Brazil)", "Dutch",
    "Russian", "Polish", "Japanese", "Korean", "Chinese (Simplified)", "Chinese (Traditional)", "Thai"};
inline constexpr int kLanguageSettings[] = {1, 12, 2, 13, 3, 4, 5, 14, 9, 17, 8, 10, 18, 0, 7, 15, 16, 19};
inline constexpr int kLanguageRegions[] = {1, 2, 2, 1, 2, 2, 2, 1, 2, 1, 2, 2, 2, 0, 5, 4, 6, 1};
static_assert(std::size(kLanguageLabels) == std::size(kLanguageKeys) &&
              std::size(kLanguageSettings) == std::size(kLanguageKeys) &&
              std::size(kLanguageRegions) == std::size(kLanguageKeys));
struct Preferences {
    bool hud = true;
    int volume = 100;
    bool mute = false;
    bool detailed_logging = false;
    GraphicsBackend backend = GraphicsBackend::Vulkan;
    int resolution = kNativeResolution;  // index into kResolutionKeys
    int upscaling_filter = 0;            // index into kUpscalingFilterKeys
    int astc_textures = 0;               // index into kAstcTextureKeys (0 is compressed)
    int refresh = 0;                     // index into kRefreshKeys
    int output = 0;                      // index into kOutputKeys
    bool frame_gen = false;              // frame generation (Vulkan only)
    int frame_gen_method = 0;            // index into kFrameGenMethodKeys (0 is LSFG)
    int frame_gen_target = 0;            // index into kFrameGenTargetKeys (0 is "auto")
    int frame_gen_multiplier = 0;        // index into kFrameGenMultiplierKeys (0 is 2x)
    std::string post_processing;         // Settings > Video > Post-processing: a preset's name ("": off)
    bool vibration = true;
    int vibration_strength = 100;        // percent of what a game asks for: 25, 50, 75 or 100
    int language = 0;                    // index into kLanguageKeys (English (US), Eden's default)
    int menu_volume = 70;                // the launcher's own sounds, 0 (off) to 100
    bool large_text = false;             // Settings > Accessibility: the launcher's look
    bool high_contrast = false;
    bool reduce_motion = false;
    std::string theme = "canopy";        // Settings > Theme: the launcher's look, by its id
    std::string accent;                   // Settings > Theme > Colour: accent preset id ("": theme's own)
    ButtonMapping mapping = kDefaultMapping;  // Settings > Controls > Button mapping
    int controller = -1;                 // a game's own Controller type (kControllerKeys); -1: automatic
    bool immediate_logs = false;         // Settings > Diagnostics: log lines written as they come
    // Settings > Video: the overlay's chip, which draws over whatever the game shows. Kept at the
    // end so the aggregate initialisations the checks use still mean what they did.
    int hud_size = 0;        // index into kHudSizeKeys: 0 regular, 1 medium, 2 large
    int hud_corner = 0;      // index into kHudCornerKeys: which corner of the picture it stands in
    bool hud_colors = true;  // the line in its inks rather than one (bake-loading-text.py)
    // What the overlay's line may say, bit by bit (hud.h's kHudField*): the pieces the user has
    // chosen to show. All of them by default, the game's own resolution (bit 9) with the rest.
    int hud_fields = 0x2ff;
    // Which face button presses each core menu action (Settings > Controls > Menu keybinds).
    MenuBinding menu_bind = kDefaultMenuBinding;
    // Settings > Appearance > Wallpaper: the image file's name in the wallpapers folder ("" when
    // the theme's own art shows).
    std::string wallpaper;
    // Settings > Diagnostics > Update notices: off, the launcher keeps a newer release to itself at
    // its start (About still names it).
    bool update_notices = true;
};

inline int KeyIndex(const std::string& value, const char* const* keys, int count, int fallback) {
    for (int i = 0; i < count; ++i)
        if (value == keys[i]) return i;
    return fallback;
}

// A game's file as the Library names it: its path in the game files folder's roms/, either its
// name ("Game.nsp") or a game folder and its name ("Game/Game.nsp", the game's update and DLC
// beside it). Readable characters, one folder at most, never a hidden folder, "." or "..".
inline bool ValidRomPath(std::string_view name) {
    if (name.size() < 5 || name.size() > 511) return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '\\') return false;
    if (const auto slash = name.find('/'); slash != std::string_view::npos) {
        if (slash == 0 || name[0] == '.' || name.find('/', slash + 1) != std::string_view::npos) return false;
        if (slash > 255 || name.size() - slash - 1 < 5 || name.size() - slash - 1 > 255) return false;
    } else if (name.size() > 255) {
        return false;
    }
    std::string extension(name.substr(name.size() - 4));
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".nsp" || extension == ".xci";
}

// A wallpaper's file name: the name alone (the image lives in the wallpapers folder), readable
// characters, at most 128 bytes. Empty means no wallpaper; "." and ".." are never a file.
inline bool ValidWallpaperName(std::string_view name) {
    if (name.empty()) return true;
    if (name.size() > 128 || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
    return true;
}

inline std::string SettingsFile() { return ConfigFile("prosperoeden.json"); }

namespace Settings {
using Json = nlohmann::json;

inline std::string Folder(const std::string& file) {
    const auto slash = file.find_last_of('/');
    return slash == std::string::npos ? std::string{"."} : file.substr(0, slash);
}

// The whole file, or empty when it cannot be read completely.
inline bool ReadFile(const std::string& path, std::string& text) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    text.clear();
    char buffer[4096];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, count);
    const bool ok = !std::ferror(file);
    std::fclose(file);
    return ok;
}

inline bool WriteFile(const std::string& path, std::string_view contents) {
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (ok && std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    std::remove(temporary.c_str());
    return false;
}

// The whole file, every profile's part included: for the code that keeps the profiles (profiles.h).
inline bool WriteWhole(const Json& document, const std::string& file) {
    return WriteFile(file, document.dump(2, ' ', false, Json::error_handler_t::replace) + "\n");
}

inline std::string TitleKey(uint64_t title_id) {
    char key[17];
    std::snprintf(key, sizeof(key), "%016" PRIX64, title_id);
    return key;
}

// The earlier text files, when they are still in the settings folder.
inline Json Legacy(const std::string& folder) {
    Json document = Json::object();
    std::string text;
    if (ReadFile(folder + "/settings.txt", text)) {
        int version, hud, volume, mute, logging, backend = 1;
        char extra;
        std::istringstream input(text);
        if ((input >> version >> hud >> volume >> mute >> logging) &&
            (version == 1 || (version == 2 && (input >> backend))) && !(input >> extra) &&
            (backend == 0 || backend == 1) && (hud == 0 || hud == 1) && volume >= 0 && volume <= 100 &&
            (mute == 0 || mute == 1) && (logging == 0 || logging == 1)) {
            document["video"] = {{"renderer", backend ? "vulkan" : "opengl"}, {"fps_overlay", hud != 0}};
            document["audio"] = {{"volume", volume}, {"mute", mute != 0}};
            document["diagnostics"] = {{"detailed_logging", logging != 0}};
        }
    }
    if (ReadFile(folder + "/last-game.txt", text) && ValidRomPath(text))
        document["library"]["last_game"] = text;
    if (ReadFile(folder + "/recent-games.txt", text)) {
        Json recent = Json::array();
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line) && recent.size() < 4;)
            if (ValidRomPath(line)) recent.push_back(line);
        if (!recent.empty()) document["library"]["recent"] = recent;
    }
    if (ReadFile(folder + "/assets-dir.txt", text)) {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (ValidAssetsDir(text)) document["game_files"] = text;
    }
    return document;
}

// The whole settings file; the first read after an update builds it from the earlier text files.
inline Json LoadWhole(const std::string& file) {
    std::string text;
    if (ReadFile(file, text)) {
        Json document = Json::parse(text, nullptr, false);
        return document.is_object() ? document : Json::object();
    }
    Json document = Legacy(Folder(file));
    if (!document.empty()) {
        document["version"] = 1;
        (void)WriteWhole(document, file);
    }
    return document;
}

// Every profile has its own settings (profiles.h): the ones under Settings, each game's own, and
// the recently played games. The profile that was there before profiles could be chosen keeps
// them where they always were, at the top of the file; every other profile has the same layout
// under "profile_settings"/<its ID>. What belongs to the console and not to a person stays at the
// top for everyone: the game files folder and the profiles themselves.
inline constexpr const char* kSharedKeys[] = {"version", "game_files", "profiles", "profile_settings"};

// Where the chosen profile's settings are in the whole file; empty for the top of the file.
inline std::string ProfileAt(const Json& whole) {
    const Json::json_pointer current("/profiles/current"), first("/profiles/first");
    if (!whole.contains(current) || !whole.at(current).is_string()) return {};
    const std::string key = whole.at(current).get<std::string>();
    const std::string owner = whole.contains(first) && whole.at(first).is_string() ?
        whole.at(first).get<std::string>() : std::string{};
    const bool own = key.size() == 32 && key != owner &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    return own ? "/profile_settings/" + key : std::string{};
}

// The settings of the profile that is playing, as one document: what every reader and writer of
// settings below works on. For the first profile that is the file itself.
inline Json Load(const std::string& file) {
    Json whole = LoadWhole(file);
    const std::string at = ProfileAt(whole);
    if (at.empty()) return whole;
    const Json::json_pointer pointer(at);
    Json view = whole.contains(pointer) && whole.at(pointer).is_object() ? whole.at(pointer) : Json::object();
    for (const char* key : kSharedKeys)
        if (whole.contains(key)) view[key] = whole[key];
    return view;
}

// Writes the playing profile's settings back into the file, and with them what is shared.
inline bool Write(const Json& document, const std::string& file) {
    Json whole = LoadWhole(file);
    const std::string at = ProfileAt(whole);
    if (at.empty()) {
        // The first profile's settings are the top of the file: the other profiles' parts, which
        // this document may be older than, are kept as the file has them.
        Json next = document;
        if (whole.contains("profile_settings")) next["profile_settings"] = whole["profile_settings"];
        return WriteWhole(next, file);
    }
    Json own = document;
    for (const char* key : kSharedKeys) own.erase(key);
    whole[Json::json_pointer(at)] = std::move(own);
    whole["version"] = 1;
    // The game files folder is the console's: a profile that changes it changes it for everyone.
    if (document.contains("game_files")) whole["game_files"] = document["game_files"];
    return WriteWhole(whole, file);
}

// A button mapping as the file names it: the default with the buttons it names (button_mapping.h).
// One that names a DualSense button twice, or none it knows, is the fallback.
inline ButtonMapping Mapping(const Json& document, const Json::json_pointer& at, const ButtonMapping& fallback) {
    if (!document.contains(at) || !document.at(at).is_object()) return fallback;
    ButtonMapping result = kDefaultMapping;
    for (const auto& [name, value] : document.at(at).items()) {
        const int game = KeyIndex(name, kGameButtonKeys, kGameButtons, -1);
        const int pad = value.is_string() ? KeyIndex(value.get<std::string>(), kPadButtonKeys, kPadButtons, -1) : -1;
        if (game < 0 || pad < 0) return fallback;
        result[game] = pad;
    }
    return ValidMapping(result) ? result : fallback;
}
// What differs from the default, by name.
inline Json MappingJson(const ButtonMapping& mapping) {
    Json result = Json::object();
    for (int game = 0; game < kGameButtons; ++game)
        if (mapping[game] != kDefaultMapping[game]) result[kGameButtonKeys[game]] = kPadButtonKeys[mapping[game]];
    return result;
}

// A menu button binding as the file names it: the default with the actions it names. One that names
// a face button twice, or none of the four, is the fallback.
inline MenuBinding MenuMapping(const Json& document, const Json::json_pointer& at, const MenuBinding& fallback) {
    if (!document.contains(at) || !document.at(at).is_object()) return fallback;
    MenuBinding result = kDefaultMenuBinding;
    for (const auto& [name, value] : document.at(at).items()) {
        const int action = KeyIndex(name, kMenuActionKeys, kMenuActions, -1);
        const int pad = value.is_string() ? KeyIndex(value.get<std::string>(), kPadButtonKeys, kMenuActions, -1) : -1;
        if (action < 0 || pad < 0) return fallback;
        result[action] = pad;
    }
    return ValidMenuBinding(result) ? result : fallback;
}
// What differs from the default, by name.
inline Json MenuMappingJson(const MenuBinding& binding) {
    Json result = Json::object();
    for (int action = 0; action < kMenuActions; ++action)
        if (binding[action] != kDefaultMenuBinding[action])
            result[kMenuActionKeys[action]] = kPadButtonKeys[binding[action]];
    return result;
}

inline bool Bool(const Json& document, const Json::json_pointer& at, bool fallback) {
    return document.contains(at) && document.at(at).is_boolean() ? document.at(at).get<bool>() : fallback;
}
inline int Int(const Json& document, const Json::json_pointer& at, int fallback) {
    return document.contains(at) && document.at(at).is_number_integer() ? document.at(at).get<int>() : fallback;
}
inline std::string String(const Json& document, const Json::json_pointer& at) {
    return document.contains(at) && document.at(at).is_string() ? document.at(at).get<std::string>() : std::string{};
}
} // namespace Settings

// Post-processing: the name of one of Eden's ReShade FX presets (the ones it bundles and the
// .fxp files in post_presets), as Eden names them; "" is off. Kept by name because the presets
// a console has can change. A name that is not a preset any more leaves the picture as it is.
inline bool ValidPostProcessing(std::string_view name) {
    return name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return static_cast<unsigned char>(c) >= 32 && c != 127 && c != '/' && c != '\\';
    });
}

inline Preferences LoadPreferences(const std::string& file = SettingsFile()) {
    using Settings::Json;
    const Json document = Settings::Load(file);
    Preferences result;
    result.hud = Settings::Bool(document, Json::json_pointer("/video/fps_overlay"), result.hud);
    result.hud_size = KeyIndex(Settings::String(document, Json::json_pointer("/video/fps_overlay_size")),
                               kHudSizeKeys, int(std::size(kHudSizeKeys)), result.hud_size);
    result.hud_corner = KeyIndex(Settings::String(document, Json::json_pointer("/video/fps_overlay_corner")),
                                 kHudCornerKeys, int(std::size(kHudCornerKeys)), result.hud_corner);
    result.hud_colors = Settings::Bool(document, Json::json_pointer("/video/fps_overlay_colors"),
                                       result.hud_colors);
    const int hud_fields =
        Settings::Int(document, Json::json_pointer("/video/fps_overlay_fields"), result.hud_fields);
    if (ValidHudFields(hud_fields)) result.hud_fields = hud_fields;
    result.backend = Settings::String(document, Json::json_pointer("/video/renderer")) == "opengl" ?
        GraphicsBackend::OpenGL : GraphicsBackend::Vulkan;
    const int volume = Settings::Int(document, Json::json_pointer("/audio/volume"), result.volume);
    if (volume >= 0 && volume <= 100) result.volume = volume;
    result.mute = Settings::Bool(document, Json::json_pointer("/audio/mute"), result.mute);
    const int menu_volume = Settings::Int(document, Json::json_pointer("/audio/menu_volume"), result.menu_volume);
    if (menu_volume >= 0 && menu_volume <= 100) result.menu_volume = menu_volume;
    // A theme's id is a short plain word; anything else reads as the default.
    const std::string theme = Settings::String(document, Json::json_pointer("/launcher/theme"));
    if (!theme.empty() && theme.size() <= 32 &&
        std::all_of(theme.begin(), theme.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
        result.theme = theme;
    const std::string accent = Settings::String(document, Json::json_pointer("/launcher/accent"));
    if (accent.size() <= 32 &&
        std::all_of(accent.begin(), accent.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
        result.accent = accent;
    const std::string wallpaper = Settings::String(document, Json::json_pointer("/launcher/wallpaper"));
    if (ValidWallpaperName(wallpaper)) result.wallpaper = wallpaper;
    result.detailed_logging = Settings::Bool(document, Json::json_pointer("/diagnostics/detailed_logging"),
                                             result.detailed_logging);
    result.immediate_logs = Settings::Bool(document, Json::json_pointer("/diagnostics/immediate_logs"),
                                           result.immediate_logs);
    result.resolution = KeyIndex(Settings::String(document, Json::json_pointer("/video/resolution")),
                                 kResolutionKeys, int(std::size(kResolutionKeys)), result.resolution);
    result.upscaling_filter = KeyIndex(Settings::String(document, Json::json_pointer("/video/upscaling_filter")),
                                       kUpscalingFilterKeys, int(std::size(kUpscalingFilterKeys)),
                                       result.upscaling_filter);
    result.astc_textures = KeyIndex(Settings::String(document, Json::json_pointer("/video/astc_textures")),
                                    kAstcTextureKeys, int(std::size(kAstcTextureKeys)), result.astc_textures);
    result.refresh = KeyIndex(Settings::String(document, Json::json_pointer("/video/refresh_rate")),
                              kRefreshKeys, int(std::size(kRefreshKeys)), result.refresh);
    result.output = KeyIndex(Settings::String(document, Json::json_pointer("/video/output_resolution")),
                             kOutputKeys, int(std::size(kOutputKeys)), result.output);
    result.frame_gen = Settings::Bool(document, Json::json_pointer("/video/frame_gen"), result.frame_gen);
    result.frame_gen_method = KeyIndex(
        Settings::String(document, Json::json_pointer("/video/frame_gen_method")), kFrameGenMethodKeys,
        int(std::size(kFrameGenMethodKeys)), result.frame_gen_method);
    result.frame_gen_target = KeyIndex(
        Settings::String(document, Json::json_pointer("/video/frame_gen_target")), kFrameGenTargetKeys,
        int(std::size(kFrameGenTargetKeys)), result.frame_gen_target);
    result.frame_gen_multiplier = KeyIndex(
        Settings::String(document, Json::json_pointer("/video/frame_gen_multiplier")),
        kFrameGenMultiplierKeys, int(std::size(kFrameGenMultiplierKeys)), result.frame_gen_multiplier);
    if (const auto preset = Settings::String(document, Json::json_pointer("/video/post_processing"));
        ValidPostProcessing(preset))
        result.post_processing = preset;
    result.vibration = Settings::Bool(document, Json::json_pointer("/controls/vibration"), result.vibration);
    // A strength between the steps is read as the step below it; out of range as the nearest end.
    result.vibration_strength = std::clamp(
        Settings::Int(document, Json::json_pointer("/controls/vibration_strength"), result.vibration_strength), 25,
        100) / 25 * 25;
    result.language = KeyIndex(Settings::String(document, Json::json_pointer("/system/language")),
                               kLanguageKeys, int(std::size(kLanguageKeys)), result.language);
    result.update_notices = Settings::Bool(document, Json::json_pointer("/system/update_notices"),
                                           result.update_notices);
    result.large_text = Settings::Bool(document, Json::json_pointer("/accessibility/large_text"), false);
    result.high_contrast = Settings::Bool(document, Json::json_pointer("/accessibility/high_contrast"), false);
    result.reduce_motion = Settings::Bool(document, Json::json_pointer("/accessibility/reduce_motion"), false);
    result.mapping = Settings::Mapping(document, Json::json_pointer("/controls/mapping"), kDefaultMapping);
    result.menu_bind =
        Settings::MenuMapping(document, Json::json_pointer("/controls/menu_mapping"), kDefaultMenuBinding);
    return result;
}

inline bool SavePreferences(const Preferences& value, const std::string& file = SettingsFile()) {
    if (value.volume < 0 || value.volume > 100 || value.menu_volume < 0 || value.menu_volume > 100 ||
        (value.backend != GraphicsBackend::OpenGL && value.backend != GraphicsBackend::Vulkan) ||
        value.resolution < 0 || value.resolution >= int(std::size(kResolutionKeys)) ||
        value.upscaling_filter < 0 || value.upscaling_filter >= int(std::size(kUpscalingFilterKeys)) ||
        value.astc_textures < 0 || value.astc_textures >= int(std::size(kAstcTextureKeys)) ||
        value.refresh < 0 || value.refresh >= int(std::size(kRefreshKeys)) ||
        value.output < 0 || value.output >= int(std::size(kOutputKeys)) ||
        value.language < 0 || value.language >= int(std::size(kLanguageKeys)) ||
        value.hud_size < 0 || value.hud_size >= int(std::size(kHudSizeKeys)) ||
        value.hud_corner < 0 || value.hud_corner >= int(std::size(kHudCornerKeys)) ||
        !ValidHudFields(value.hud_fields) || !ValidMapping(value.mapping) ||
        !ValidMenuBinding(value.menu_bind) || !ValidWallpaperName(value.wallpaper) ||
        !ValidPostProcessing(value.post_processing)) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["video"]["renderer"] = value.backend == GraphicsBackend::Vulkan ? "vulkan" : "opengl";
    document["video"]["fps_overlay"] = value.hud;
    document["video"]["fps_overlay_size"] = kHudSizeKeys[value.hud_size];
    document["video"]["fps_overlay_corner"] = kHudCornerKeys[value.hud_corner];
    document["video"]["fps_overlay_colors"] = value.hud_colors;
    document["video"]["fps_overlay_fields"] = value.hud_fields;
    document["video"]["resolution"] = kResolutionKeys[value.resolution];
    document["video"]["upscaling_filter"] = kUpscalingFilterKeys[value.upscaling_filter];
    document["video"]["astc_textures"] = kAstcTextureKeys[value.astc_textures];
    document["video"]["refresh_rate"] = kRefreshKeys[value.refresh];
    document["video"]["output_resolution"] = kOutputKeys[value.output];
    document["video"]["frame_gen"] = value.frame_gen;
    document["video"]["frame_gen_method"] = kFrameGenMethodKeys[value.frame_gen_method];
    document["video"]["frame_gen_target"] = kFrameGenTargetKeys[value.frame_gen_target];
    document["video"]["frame_gen_multiplier"] = kFrameGenMultiplierKeys[value.frame_gen_multiplier];
    if (value.post_processing.empty()) document["video"].erase("post_processing");
    else document["video"]["post_processing"] = value.post_processing;
    document["audio"]["volume"] = value.volume;
    document["audio"]["mute"] = value.mute;
    document["audio"]["menu_volume"] = value.menu_volume;
    document["controls"]["vibration"] = value.vibration;
    document["controls"]["vibration_strength"] = value.vibration_strength;
    if (value.mapping == kDefaultMapping) document["controls"].erase("mapping");
    else document["controls"]["mapping"] = Settings::MappingJson(value.mapping);
    if (value.menu_bind == kDefaultMenuBinding) document["controls"].erase("menu_mapping");
    else document["controls"]["menu_mapping"] = Settings::MenuMappingJson(value.menu_bind);
    document["system"]["language"] = kLanguageKeys[value.language];
    document["system"]["update_notices"] = value.update_notices;
    document["diagnostics"]["detailed_logging"] = value.detailed_logging;
    document["diagnostics"]["immediate_logs"] = value.immediate_logs;
    document["accessibility"]["large_text"] = value.large_text;
    document["accessibility"]["high_contrast"] = value.high_contrast;
    document["accessibility"]["reduce_motion"] = value.reduce_motion;
    document["launcher"]["theme"] = value.theme;
    document["launcher"]["wallpaper"] = value.wallpaper;
    if (document["launcher"].contains("wallpaper_blur"))
        document["launcher"].erase("wallpaper_blur"); // a switch this build no longer has
    if (!value.accent.empty())
        document["launcher"]["accent"] = value.accent;
    else if (document["launcher"].contains("accent"))
        document["launcher"].erase("accent");
    return Settings::Write(document, file);
}

// Games run docked unless the player saved "Handheld" for that title in the launcher.
inline bool LoadGameDocked(uint64_t title_id, const std::string& file = SettingsFile()) {
    if (!title_id) return true;
    using Settings::Json;
    const Json document = Settings::Load(file);
    const Json::json_pointer at("/games/" + Settings::TitleKey(title_id) + "/console_mode");
    if (document.contains(at)) return Settings::String(document, at) != "handheld";
    // A mode saved by an earlier version, in its own file.
    char name[48];
    std::snprintf(name, sizeof(name), "/game-%016llx-mode.txt", static_cast<unsigned long long>(title_id));
    std::string text;
    return !(Settings::ReadFile(Settings::Folder(file) + name, text) && text == "1 handheld\n");
}

inline bool SaveGameDocked(uint64_t title_id, bool docked, const std::string& file = SettingsFile()) {
    if (!title_id) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["games"][Settings::TitleKey(title_id)]["console_mode"] = docked ? "docked" : "handheld";
    return Settings::Write(document, file);
}

// The Performance switches by name, in the order of PerformanceSettings and of the launcher.
inline constexpr const char* kPerformanceKeys[] = {"block_list", "async_shaders", "fast_gpu", "unsafe_cpu",
                                                   "unsafe_dma", "reactive_flushing", "skip_invalidation",
                                                   "compact_memory"};
inline constexpr int kPerformanceSwitches = int(std::size(kPerformanceKeys));
// Settings > Performance > Memory: how much memory the guest's console is told it has. Eden calls
// this the memory layout, and it is what a mod that keeps a heap of its own (an HD camera pack)
// asks for: the standard layout is the Switch's 4 GiB, and a mod written against a larger one
// reads the size from the kernel and reserves what it does not find, which stops the game before
// its first picture rather than the mod. Above 4 GiB the guest's memory is a block backed where
// the game's kernel uses it, so a 6 GiB layout holds only what a session touches; the pool the CPU
// shares with the GPU still has to take that much, so a demanding game can run out of it instead
// of running out of graphics memory. The menu shows the three as plain sizes (settings.cpp's
// kMemoryLayouts); the line at launch in main.cpp says which one a session took.
inline constexpr const char* kMemoryLayoutKeys[] = {"4Gb", "6Gb", "8Gb"};
inline constexpr int kMemoryLayoutGib[] = {4, 6, 8};
static_assert(std::size(kMemoryLayoutKeys) == std::size(kMemoryLayoutGib));
// Settings > Performance > Speed limit: how fast a game may run, as a percentage of the console it
// was made for (Eden's speed limit). Above 100% the emulated console's vsync comes that much
// sooner, so a game locked to 30 FPS can reach 60 at 200% where the console has the time to spare,
// and a game that counts its frames to keep time plays faster with it. 0 is Eden's limit switched
// off: as fast as the console can. The menu writes the percentages (settings.cpp).
inline constexpr const char* kSpeedLimitKeys[] = {"100", "150", "200", "300", "unlimited"};
inline constexpr int kSpeedLimitPercent[] = {100, 150, 200, 300, 0};
static_assert(std::size(kSpeedLimitKeys) == std::size(kSpeedLimitPercent));

// Library > Game settings: what one game does differently from Settings. Each value is -1 (absent
// from the file) while the game follows Settings; switches are 0 off, 1 on.
struct GameSettings {
    int renderer = -1;          // 0 OpenGL, 1 Vulkan
    int resolution = -1;        // index into kResolutionKeys
    int upscaling_filter = -1;  // index into kUpscalingFilterKeys
    int refresh = -1;           // index into kRefreshKeys
    int hud = -1;               // FPS overlay
    int hud_size = -1;          // index into kHudSizeKeys
    int hud_corner = -1;        // index into kHudCornerKeys
    int frame_gen = -1;         // frame generation (Vulkan only)
    int frame_gen_method = -1;  // index into kFrameGenMethodKeys
    int frame_gen_target = -1;  // index into kFrameGenTargetKeys
    int frame_gen_multiplier = -1;  // index into kFrameGenMultiplierKeys
    std::optional<std::string> post_processing;  // a preset's name, "" off; none: as Settings
    int astc_textures = -1;     // index into kAstcTextureKeys
    int volume = -1;            // game volume, 0 to 100
    int mute = -1;
    int vibration = -1;
    int language = -1;          // index into kLanguageKeys
    int controller = -1;        // index into kControllerKeys
    bool own_mapping = false;   // the game has a button mapping of its own: mapping
    ButtonMapping mapping = kDefaultMapping;
    std::array<int, kPerformanceSwitches> performance{-1, -1, -1, -1, -1, -1, -1, -1};  // kPerformanceKeys
    int64_t rng_seed = -1;      // 0 to 4294967295, or -1: no seed (load_address.h)
    int speed_limit = -1;       // index into kSpeedLimitKeys ("performance" / "speed_limit")
};
// The seed a typed text names: digits and nothing else, 0 to 4294967295. -1 for any other text.
inline int64_t RngSeedFromText(std::string_view text) {
    if (text.empty() || text.size() > 10) return -1;
    uint64_t seed = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return -1;
        seed = seed * 10 + static_cast<uint64_t>(digit - '0');
    }
    return seed <= 0xffffffffULL ? static_cast<int64_t>(seed) : -1;
}
inline constexpr const char* kRendererKeys[] = {"opengl", "vulkan"};
// Library > Game settings > Controls > Controller type: the controller every player of the game
// gets (the handheld: player 1 only). Without one a game gets what it takes, a Pro Controller
// first (controller_applet.h).
inline constexpr const char* kControllerKeys[] = {"pro", "handheld", "dual_joycons", "left_joycon", "right_joycon"};

inline GameSettings LoadGameSettings(uint64_t title_id, const std::string& file = SettingsFile()) {
    GameSettings result;
    if (!title_id) return result;
    using Settings::Json;
    const Json document = Settings::Load(file);
    const std::string base = "/games/" + Settings::TitleKey(title_id);
    const auto key = [&](const char* name) { return Settings::String(document, Json::json_pointer(base + "/" + name)); };
    result.renderer = KeyIndex(key("renderer"), kRendererKeys, int(std::size(kRendererKeys)), -1);
    result.resolution = KeyIndex(key("resolution"), kResolutionKeys, int(std::size(kResolutionKeys)), -1);
    result.upscaling_filter = KeyIndex(key("upscaling_filter"), kUpscalingFilterKeys,
                                       int(std::size(kUpscalingFilterKeys)), -1);
    result.astc_textures = KeyIndex(key("astc_textures"), kAstcTextureKeys, int(std::size(kAstcTextureKeys)), -1);
    if (const Json::json_pointer at(base + "/post_processing");
        document.contains(at) && document.at(at).is_string() && ValidPostProcessing(document.at(at).get<std::string>()))
        result.post_processing = document.at(at).get<std::string>();
    result.refresh = KeyIndex(key("refresh_rate"), kRefreshKeys, int(std::size(kRefreshKeys)), -1);
    result.hud_size = KeyIndex(key("fps_overlay_size"), kHudSizeKeys, int(std::size(kHudSizeKeys)), -1);
    result.hud_corner = KeyIndex(key("fps_overlay_corner"), kHudCornerKeys,
                                 int(std::size(kHudCornerKeys)), -1);
    result.frame_gen_method = KeyIndex(key("frame_gen_method"), kFrameGenMethodKeys,
                                       int(std::size(kFrameGenMethodKeys)), -1);
    result.frame_gen_target = KeyIndex(key("frame_gen_target"), kFrameGenTargetKeys,
                                       int(std::size(kFrameGenTargetKeys)), -1);
    result.frame_gen_multiplier = KeyIndex(key("frame_gen_multiplier"), kFrameGenMultiplierKeys,
                                           int(std::size(kFrameGenMultiplierKeys)), -1);
    result.language = KeyIndex(key("language"), kLanguageKeys, int(std::size(kLanguageKeys)), -1);
    result.controller = KeyIndex(key("controller"), kControllerKeys, int(std::size(kControllerKeys)), -1);
    const auto flag = [&](const std::string& path) {
        const Json::json_pointer at(base + path);
        return document.contains(at) && document.at(at).is_boolean() ? int(document.at(at).get<bool>()) : -1;
    };
    result.hud = flag("/fps_overlay");
    result.frame_gen = flag("/frame_gen");
    result.mute = flag("/mute");
    result.vibration = flag("/vibration");
    for (int i = 0; i < kPerformanceSwitches; ++i)
        result.performance[i] = flag(std::string("/performance/") + kPerformanceKeys[i]);
    result.speed_limit = KeyIndex(key("performance/speed_limit"), kSpeedLimitKeys,
                                  int(std::size(kSpeedLimitKeys)), -1);
    const int volume = Settings::Int(document, Json::json_pointer(base + "/volume"), -1);
    result.volume = volume >= 0 && volume <= 100 ? volume : -1;
    const Json::json_pointer seed_at(base + "/rng_seed");
    if (document.contains(seed_at) && (document.at(seed_at).is_number_integer() ||
                                       document.at(seed_at).is_number_unsigned())) {
        const auto& seed_value = document.at(seed_at);
        const auto seed = seed_value.is_number_unsigned() ? seed_value.get<uint64_t>() :
                                                             static_cast<uint64_t>(seed_value.get<int64_t>());
        result.rng_seed = seed <= 0xffffffffULL ? static_cast<int64_t>(seed) : -1;
    }
    const Json::json_pointer mapping(base + "/mapping");
    result.own_mapping = document.contains(mapping);
    if (result.own_mapping) result.mapping = Settings::Mapping(document, mapping, kDefaultMapping);
    return result;
}

inline bool SaveGameSettings(uint64_t title_id, const GameSettings& value, const std::string& file = SettingsFile()) {
    const auto flag_ok = [](int flag) { return flag >= -1 && flag <= 1; };
    if (!title_id || value.renderer >= int(std::size(kRendererKeys)) ||
        value.resolution >= int(std::size(kResolutionKeys)) ||
        value.upscaling_filter >= int(std::size(kUpscalingFilterKeys)) ||
        value.astc_textures >= int(std::size(kAstcTextureKeys)) ||
        value.refresh >= int(std::size(kRefreshKeys)) || value.language >= int(std::size(kLanguageKeys)) ||
        value.controller >= int(std::size(kControllerKeys)) ||
        value.frame_gen_method >= int(std::size(kFrameGenMethodKeys)) ||
        value.frame_gen_target >= int(std::size(kFrameGenTargetKeys)) ||
        value.frame_gen_multiplier >= int(std::size(kFrameGenMultiplierKeys)) ||
        value.volume > 100 || value.rng_seed < -1 || value.rng_seed > 0xffffffffLL ||
        value.hud_size >= int(std::size(kHudSizeKeys)) ||
        value.hud_corner >= int(std::size(kHudCornerKeys)) ||
        value.speed_limit >= int(std::size(kSpeedLimitKeys)) ||
        !flag_ok(value.hud) || !flag_ok(value.frame_gen) ||
        !flag_ok(value.mute) ||
        !flag_ok(value.vibration) ||
        !std::all_of(value.performance.begin(), value.performance.end(), flag_ok) ||
        (value.post_processing && !ValidPostProcessing(*value.post_processing)) ||
        (value.own_mapping && !ValidMapping(value.mapping))) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& game = document["games"][Settings::TitleKey(title_id)];
    if (!game.is_object()) game = Settings::Json::object();
    const auto store = [&](const char* name, int index, const char* const* keys) {
        if (index < 0) game.erase(name);
        else game[name] = keys[index];
    };
    store("renderer", value.renderer, kRendererKeys);
    store("resolution", value.resolution, kResolutionKeys);
    store("upscaling_filter", value.upscaling_filter, kUpscalingFilterKeys);
    store("astc_textures", value.astc_textures, kAstcTextureKeys);
    store("refresh_rate", value.refresh, kRefreshKeys);
    store("fps_overlay_size", value.hud_size, kHudSizeKeys);
    store("fps_overlay_corner", value.hud_corner, kHudCornerKeys);
    store("frame_gen_method", value.frame_gen_method, kFrameGenMethodKeys);
    store("frame_gen_target", value.frame_gen_target, kFrameGenTargetKeys);
    store("frame_gen_multiplier", value.frame_gen_multiplier, kFrameGenMultiplierKeys);
    if (value.post_processing) game["post_processing"] = *value.post_processing;
    else game.erase("post_processing");
    store("language", value.language, kLanguageKeys);
    store("controller", value.controller, kControllerKeys);
    const auto flag = [](Settings::Json& owner, const char* name, int value) {
        if (value < 0) owner.erase(name);
        else owner[name] = value == 1;
    };
    flag(game, "fps_overlay", value.hud);
    flag(game, "frame_gen", value.frame_gen);
    flag(game, "mute", value.mute);
    flag(game, "vibration", value.vibration);
    if (value.volume < 0) game.erase("volume");
    else game["volume"] = value.volume;
    if (value.rng_seed < 0) game.erase("rng_seed");
    else game["rng_seed"] = value.rng_seed;
    if (value.own_mapping) game["mapping"] = Settings::MappingJson(value.mapping);
    else game.erase("mapping");
    auto& performance = game["performance"];
    if (!performance.is_object()) performance = Settings::Json::object();
    for (int i = 0; i < kPerformanceSwitches; ++i) flag(performance, kPerformanceKeys[i], value.performance[i]);
    if (value.speed_limit < 0) performance.erase("speed_limit");
    else performance["speed_limit"] = kSpeedLimitKeys[value.speed_limit];
    if (performance.empty()) game.erase("performance");
    return Settings::Write(document, file);
}

// The settings a session of a game uses: Settings, with what the game does differently.
inline Preferences PreferencesFor(uint64_t title_id, const std::string& file = SettingsFile()) {
    Preferences result = LoadPreferences(file);
    if (!title_id) return result;
    const GameSettings game = LoadGameSettings(title_id, file);
    if (game.renderer >= 0) result.backend = game.renderer == 0 ? GraphicsBackend::OpenGL : GraphicsBackend::Vulkan;
    if (game.resolution >= 0) result.resolution = game.resolution;
    if (game.upscaling_filter >= 0) result.upscaling_filter = game.upscaling_filter;
    if (game.astc_textures >= 0) result.astc_textures = game.astc_textures;
    if (game.refresh >= 0) result.refresh = game.refresh;
    if (game.hud >= 0) result.hud = game.hud == 1;
    if (game.hud_size >= 0) result.hud_size = game.hud_size;
    if (game.hud_corner >= 0) result.hud_corner = game.hud_corner;
    if (game.frame_gen >= 0) result.frame_gen = game.frame_gen == 1;
    if (game.frame_gen_method >= 0) result.frame_gen_method = game.frame_gen_method;
    if (game.frame_gen_target >= 0) result.frame_gen_target = game.frame_gen_target;
    if (game.frame_gen_multiplier >= 0) result.frame_gen_multiplier = game.frame_gen_multiplier;
    if (game.post_processing) result.post_processing = *game.post_processing;
    if (game.volume >= 0) result.volume = game.volume;
    if (game.mute >= 0) result.mute = game.mute == 1;
    if (game.vibration >= 0) result.vibration = game.vibration == 1;
    if (game.language >= 0) result.language = game.language;
    if (game.own_mapping) result.mapping = game.mapping;
    result.controller = game.controller;
    return result;
}

// The game the running session belongs to (0 at the launcher): what the audio and the renderer
// read their game's settings for.
inline std::atomic<uint64_t> session_title{0};

// Advanced, file-only workaround, deliberately per game: reducing the guest's tick rate helps
// some dynamic frame-rate games but also slows games that pace themselves by that counter.
// Keep it outside "performance", whose visible switches SavePerformance replaces as a group.
inline bool LoadAdaptiveClock(uint64_t title_id, const std::string& file = SettingsFile()) {
    if (!title_id) return false;
    using Settings::Json;
    return Settings::Bool(Settings::Load(file),
                          Json::json_pointer("/games/" + Settings::TitleKey(title_id) + "/adaptive_clock"), false);
}

// Speed against accuracy, for every game ("performance") and for one ("games/<title>/performance",
// whose values go before the general ones, one value at a time). Settings > Performance has
// a switch for each general value, and Library > Game settings > Performance one for each of a
// game's own.
struct PerformanceSettings {
    bool block_list = false;     // compile the blocks of earlier sessions ahead (jit_list.h)
    bool async_shaders = false;  // draw before a new shader is ready: no pause, things missing meanwhile
    bool fast_gpu = false;       // Eden's lowest GPU accuracy
    bool unsafe_cpu = false;     // dynarmic's inexact floating-point shortcuts
    bool unsafe_dma = false;     // Eden's unsafe DMA accuracy
    bool reactive_flushing = true;   // Eden's reactive flushing: off is faster, some effects break
    bool skip_invalidation = false;  // Eden's skip_cpu_inner_invalidation: fewer cache invalidations
    bool compact_memory = false;     // the guest's memory in 16 KiB pages where it is mostly zeroes
    int memory_layout = 0;           // index into kMemoryLayoutKeys (0 is the Switch's own 4 GiB)
    int speed_limit = 0;             // index into kSpeedLimitKeys (0 is 100%, the console's own pace)
};

inline PerformanceSettings LoadPerformance(uint64_t title_id, const std::string& file = SettingsFile()) {
    using Settings::Json;
    const Json document = Settings::Load(file);
    PerformanceSettings result;
    const auto read = [&](const std::string& base) {
        const auto value = [&](const char* name, bool& out) {
            out = Settings::Bool(document, Json::json_pointer(base + "/performance/" + name), out);
        };
        value("block_list", result.block_list);
        value("async_shaders", result.async_shaders);
        value("fast_gpu", result.fast_gpu);
        value("unsafe_cpu", result.unsafe_cpu);
        value("unsafe_dma", result.unsafe_dma);
        value("reactive_flushing", result.reactive_flushing);
        value("skip_invalidation", result.skip_invalidation);
        value("compact_memory", result.compact_memory);
        // A name that is not one of the layouts leaves the value the general settings gave.
        result.memory_layout = KeyIndex(Settings::String(document,
                                                         Json::json_pointer(base + "/performance/memory_layout")),
                                        kMemoryLayoutKeys, int(std::size(kMemoryLayoutKeys)), result.memory_layout);
        result.speed_limit = KeyIndex(Settings::String(document,
                                                       Json::json_pointer(base + "/performance/speed_limit")),
                                      kSpeedLimitKeys, int(std::size(kSpeedLimitKeys)), result.speed_limit);
    };
    read("");
    if (title_id) read("/games/" + Settings::TitleKey(title_id));
    return result;
}

// Title 0 saves the general values.
inline bool SavePerformance(uint64_t title_id, const PerformanceSettings& value,
                            const std::string& file = SettingsFile()) {
    if (value.memory_layout < 0 || value.memory_layout >= int(std::size(kMemoryLayoutKeys)) ||
        value.speed_limit < 0 || value.speed_limit >= int(std::size(kSpeedLimitKeys))) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& owner = title_id ? document["games"][Settings::TitleKey(title_id)] : document;
    if (!owner.is_object()) owner = Settings::Json::object();
    owner["performance"] = {{"block_list", value.block_list}, {"async_shaders", value.async_shaders},
                            {"fast_gpu", value.fast_gpu}, {"unsafe_cpu", value.unsafe_cpu},
                            {"unsafe_dma", value.unsafe_dma}, {"reactive_flushing", value.reactive_flushing},
                            {"skip_invalidation", value.skip_invalidation},
                            {"compact_memory", value.compact_memory},
                            {"memory_layout", kMemoryLayoutKeys[value.memory_layout]},
                            {"speed_limit", kSpeedLimitKeys[value.speed_limit]}};
    return Settings::Write(document, file);
}

// Library > Game settings > Mods: the names of the game's mods that are switched off (mods.h). A
// mod is on unless it is listed, so one added later is used without a visit to the launcher.
inline std::vector<std::string> LoadDisabledMods(uint64_t title_id, const std::string& file = SettingsFile()) {
    std::vector<std::string> names;
    if (!title_id) return names;
    using Settings::Json;
    const Json document = Settings::Load(file);
    const Json::json_pointer at("/games/" + Settings::TitleKey(title_id) + "/mods_off");
    if (!document.contains(at) || !document.at(at).is_array()) return names;
    for (const auto& entry : document.at(at))
        if (entry.is_string() && std::find(names.begin(), names.end(), entry.get<std::string>()) == names.end())
            names.push_back(entry.get<std::string>());
    return names;
}

inline bool SaveModEnabled(uint64_t title_id, std::string_view name, bool enabled,
                           const std::string& file = SettingsFile()) {
    if (!title_id || name.empty() || name.size() > 255) return false;
    auto names = LoadDisabledMods(title_id, file);
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    if (!enabled) names.emplace_back(name);
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& game = document["games"][Settings::TitleKey(title_id)];
    if (!game.is_object()) game = Settings::Json::object();
    if (names.empty()) game.erase("mods_off");
    else game["mods_off"] = names;
    return Settings::Write(document, file);
}

namespace Settings {
// A game's switch that is on unless the player turned it off (`key`: false).
inline bool LoadGameSwitch(uint64_t title_id, const char* key, const std::string& file) {
    if (!title_id) return true;
    return Bool(Load(file), Json::json_pointer("/games/" + TitleKey(title_id) + "/" + key), true);
}

inline bool SaveGameSwitch(uint64_t title_id, const char* key, bool enabled, const std::string& file) {
    if (!title_id) return false;
    Json document = Load(file);
    document["version"] = 1;
    auto& game = document["games"][TitleKey(title_id)];
    if (!game.is_object()) game = Json::object();
    if (enabled) game.erase(key);
    else game[key] = false;
    return Write(document, file);
}
} // namespace Settings

// The Library's Mods switch: one switch for all of a game's mods, on unless the player turned it
// off ("mods": false). The mods' own switches (mods_off) keep their state behind it.
inline bool LoadModsEnabled(uint64_t title_id, const std::string& file = SettingsFile()) {
    return Settings::LoadGameSwitch(title_id, "mods", file);
}

inline bool SaveModsEnabled(uint64_t title_id, bool enabled, const std::string& file = SettingsFile()) {
    return Settings::SaveGameSwitch(title_id, "mods", enabled, file);
}

// Game settings > Cheats: one switch for all of a game's cheats, apart from the Mods switch, on
// unless the player turned it off ("cheats": false). The cheats' own switches keep their state
// behind it.
inline bool LoadCheatsEnabled(uint64_t title_id, const std::string& file = SettingsFile()) {
    return Settings::LoadGameSwitch(title_id, "cheats", file);
}

inline bool SaveCheatsEnabled(uint64_t title_id, bool enabled, const std::string& file = SettingsFile()) {
    return Settings::SaveGameSwitch(title_id, "cheats", enabled, file);
}

// The cheats chosen for a game, each as "<mod>#<cheat>" (mods.h). A mod that lists several cheats
// has them chosen one by one, and none runs until it is chosen.
inline std::vector<std::string> LoadChosenCheats(uint64_t title_id, const std::string& file = SettingsFile()) {
    std::vector<std::string> ids;
    if (!title_id) return ids;
    using Settings::Json;
    const Json document = Settings::Load(file);
    const Json::json_pointer at("/games/" + Settings::TitleKey(title_id) + "/cheats_on");
    if (!document.contains(at) || !document.at(at).is_array()) return ids;
    for (const auto& entry : document.at(at))
        if (entry.is_string() && std::find(ids.begin(), ids.end(), entry.get<std::string>()) == ids.end())
            ids.push_back(entry.get<std::string>());
    return ids;
}

inline bool SaveChosenCheats(uint64_t title_id, const std::vector<std::string>& ids,
                             const std::string& file = SettingsFile()) {
    if (!title_id) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& game = document["games"][Settings::TitleKey(title_id)];
    if (!game.is_object()) game = Settings::Json::object();
    if (ids.empty()) game.erase("cheats_on");
    else game["cheats_on"] = ids;
    return Settings::Write(document, file);
}

// The single cheats of mods that also patch the code or replace files, switched off on their own
// in the Cheats list, each as "<mod>#<cheat>" (cheats.h). They run unless listed, as before the
// Cheats list had them.
inline std::vector<std::string> LoadCheatsOff(uint64_t title_id, const std::string& file = SettingsFile()) {
    std::vector<std::string> keys;
    if (!title_id) return keys;
    using Settings::Json;
    const Json document = Settings::Load(file);
    const Json::json_pointer at("/games/" + Settings::TitleKey(title_id) + "/cheats_off");
    if (!document.contains(at) || !document.at(at).is_array()) return keys;
    for (const auto& entry : document.at(at))
        if (entry.is_string() && std::find(keys.begin(), keys.end(), entry.get<std::string>()) == keys.end())
            keys.push_back(entry.get<std::string>());
    return keys;
}

inline bool SaveCheatOff(uint64_t title_id, std::string_view key, bool off, const std::string& file = SettingsFile()) {
    if (!title_id || key.empty() || key.size() > 512) return false;
    auto keys = LoadCheatsOff(title_id, file);
    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    if (off) keys.emplace_back(key);
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& game = document["games"][Settings::TitleKey(title_id)];
    if (!game.is_object()) game = Settings::Json::object();
    if (keys.empty()) game.erase("cheats_off");
    else game["cheats_off"] = keys;
    return Settings::Write(document, file);
}

inline std::string LoadLastGame(const std::string& file = SettingsFile()) {
    const Settings::Json document = Settings::Load(file);
    const std::string name = Settings::String(document, Settings::Json::json_pointer("/library/last_game"));
    return ValidRomPath(name) ? name : std::string{};
}

inline bool SaveLastGame(std::string_view name, const std::string& file = SettingsFile()) {
    if (!ValidRomPath(name)) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["library"]["last_game"] = std::string(name);
    return Settings::Write(document, file);
}

inline std::vector<std::string> LoadRecentGames(const std::string& file = SettingsFile()) {
    using Settings::Json;
    const Json document = Settings::Load(file);
    const Json::json_pointer at("/library/recent");
    std::vector<std::string> recent;
    if (!document.contains(at) || !document.at(at).is_array()) return recent;
    for (const auto& entry : document.at(at)) {
        if (!entry.is_string()) continue;
        const std::string name = entry.get<std::string>();
        if (recent.size() < 8 && ValidRomPath(name) && std::find(recent.begin(), recent.end(), name) == recent.end())
            recent.push_back(name);
    }
    return recent;
}

inline bool SaveRecentGame(std::string_view name, const std::string& file = SettingsFile()) {
    if (!ValidRomPath(name)) return false;
    auto recent = LoadRecentGames(file);
    recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
    recent.insert(recent.begin(), std::string(name));
    if (recent.size() > 8) recent.resize(8);
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["library"]["recent"] = recent;
    return Settings::Write(document, file);
}

// The saved game files folder, or empty when none is saved.
inline std::string LoadSavedAssetsDir(const std::string& file = SettingsFile()) {
    const std::string value = Settings::String(Settings::Load(file), Settings::Json::json_pointer("/game_files"));
    return ValidAssetsDir(value) ? value : std::string{};
}

inline bool SaveAssetsDir(std::string_view directory, const std::string& file = SettingsFile()) {
    if (!ValidAssetsDir(directory)) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["game_files"] = std::string(directory);
    return Settings::Write(document, file);
}

inline Multiplayer::Connection LoadRoomConnection(const std::string& file = SettingsFile()) {
    const auto document = Settings::Load(file);
    Multiplayer::Connection result;
    result.host = Settings::String(document, Settings::Json::json_pointer("/multiplayer/host"));
    result.nickname = Settings::String(document, Settings::Json::json_pointer("/multiplayer/nickname"));
    result.password = Settings::String(document, Settings::Json::json_pointer("/multiplayer/password"));
    const int port = Settings::Int(document, Settings::Json::json_pointer("/multiplayer/port"), 24872);
    result.port = port > 0 && port <= 65535 ? port : 24872;
    if (result.host.size() > 253) result.host.clear();
    if (result.nickname.size() > 20) result.nickname.clear();
    if (result.password.size() > 128) result.password.clear();
    return result;
}

inline bool SaveRoomConnection(const Multiplayer::Connection& value,
                               const std::string& file = SettingsFile()) {
    if (value.password.size() > 128) return false;
    auto document = Settings::Load(file);
    document["multiplayer"] = {{"host", value.host}, {"port", value.port},
                               {"nickname", value.nickname}, {"password", value.password}};
    return Settings::Write(document, file);
}

inline int AudioVolume(const Preferences& value) {
    return value.mute ? 0 : 0x8000 * value.volume / 100;
}
} // namespace Eden
