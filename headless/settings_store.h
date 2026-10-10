// SPDX-License-Identifier: GPL-3.0-or-later
// Everything the launcher remembers, in one JSON file (config/prosperoeden.json):
//
//   {
//     "version": 1,
//     "video": { "renderer": "vulkan", "fps_overlay": true, "resolution": "1x",
//                "upscaling_filter": "bilinear", "refresh_rate": "60",
//                "output_resolution": "1080p", "frame_gen": false,
//                "frame_gen_target": "auto", "frame_gen_multiplier": "2" },
//     "audio": { "volume": 100, "mute": false, "menu_volume": 70 },
//     "controls": { "vibration": true, "mapping": { "a": "cross", "b": "circle" } },
//     "system": { "language": "en-US" },
//     "accessibility": { "large_text": false, "high_contrast": false, "reduce_motion": false },
//     "diagnostics": { "detailed_logging": false, "immediate_logs": false },
//     "performance": { "block_list": false, "async_shaders": false, "fast_gpu": false,
//                      "unsafe_cpu": false, "unsafe_dma": false, "reactive_flushing": true,
//                      "skip_invalidation": false },
//     "game_files": "/mnt/ext1/eden",
//     "library": { "last_game": "Game [id].nsp", "recent": ["Game [id].nsp"] },
//     "games": { "0100000000010000": { "console_mode": "handheld", "renderer": "opengl",
//                                      "resolution": "0.75x", "upscaling_filter": "fsr",
//                                      "refresh_rate": "120", "fps_overlay": false,
//                                      "frame_gen": true,
//                                      "frame_gen_target": "120", "frame_gen_multiplier": "3",
//                                      "volume": 80, "mute": false, "vibration": false,
//                                      "language": "ja", "mapping": { "a": "cross" },
//                                      "controller": "handheld",
//                                      "mods": false,
//                                      "mods_off": ["A mod's folder name"],
//                                      "cheats_on": ["A mod's folder name#A cheat's name"],
//                                      "performance": { "fast_gpu": true } } }
//   }
//
// Missing or mistyped values read as their defaults. Writes replace the file atomically. The
// text files of earlier versions (settings.txt, last-game.txt, recent-games.txt, assets-dir.txt,
// game-<title>-mode.txt) are read once into the JSON file and left in place.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
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
// Settings > Video: the refresh rate of the output while a game runs. 120 Hz is asked of the
// display (display_refresh.h); one that cannot show it stays at 60 Hz.
inline constexpr const char* kRefreshKeys[] = {"60", "120"};
inline constexpr int kRefreshHz[] = {60, 120};
// Lossless Scaling frame generation (Vulkan only): the multiplier is how many frames the driver
// makes out of each one the game draws, and the target is the rate it aims for instead. Eden
// ignores the multiplier once a target rate is set (FrameGenMaxGenerations in settings.cpp lets
// the pacer add up to its own maximum then), so "auto" passes 0 and leaves the ratio in charge.
// The target's limits are Eden's own: 0 to 240 Hz.
inline constexpr const char* kFrameGenTargetKeys[] = {"auto", "60", "90", "120", "144", "240"};
inline constexpr int kFrameGenTargetHz[] = {0, 60, 90, 120, 144, 240};
static_assert(std::size(kFrameGenTargetKeys) == std::size(kFrameGenTargetHz));
inline constexpr const char* kFrameGenMultiplierKeys[] = {"2", "3", "4"};
inline constexpr int kFrameGenMultiplierValue[] = {2, 3, 4};
static_assert(std::size(kFrameGenMultiplierKeys) == std::size(kFrameGenMultiplierValue));
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
    int refresh = 0;                     // index into kRefreshKeys
    int output = 0;                      // index into kOutputKeys
    bool frame_gen = false;              // Lossless Scaling frame generation (Vulkan only)
    int frame_gen_target = 0;            // index into kFrameGenTargetKeys (0 is "auto")
    int frame_gen_multiplier = 0;        // index into kFrameGenMultiplierKeys (0 is 2x)
    bool vibration = true;
    int language = 0;                    // index into kLanguageKeys (English (US), Eden's default)
    int menu_volume = 70;                // the launcher's own sounds, 0 (off) to 100
    bool large_text = false;             // Settings > Accessibility: the launcher's look
    bool high_contrast = false;
    bool reduce_motion = false;
    ButtonMapping mapping = kDefaultMapping;  // Settings > Controls > Button mapping
    int controller = -1;                 // a game's own Controller type (kControllerKeys); -1: automatic
    bool immediate_logs = false;         // Settings > Diagnostics: log lines written as they come
};

inline int KeyIndex(const std::string& value, const char* const* keys, int count, int fallback) {
    for (int i = 0; i < count; ++i)
        if (value == keys[i]) return i;
    return fallback;
}

inline bool ValidRomFilename(std::string_view name) {
    if (name.size() < 5 || name.size() > 255) return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
    std::string extension(name.substr(name.size() - 4));
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".nsp" || extension == ".xci";
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
    if (ReadFile(folder + "/last-game.txt", text) && ValidRomFilename(text))
        document["library"]["last_game"] = text;
    if (ReadFile(folder + "/recent-games.txt", text)) {
        Json recent = Json::array();
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line) && recent.size() < 4;)
            if (ValidRomFilename(line)) recent.push_back(line);
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

inline Preferences LoadPreferences(const std::string& file = SettingsFile()) {
    using Settings::Json;
    const Json document = Settings::Load(file);
    Preferences result;
    result.hud = Settings::Bool(document, Json::json_pointer("/video/fps_overlay"), result.hud);
    result.backend = Settings::String(document, Json::json_pointer("/video/renderer")) == "opengl" ?
        GraphicsBackend::OpenGL : GraphicsBackend::Vulkan;
    const int volume = Settings::Int(document, Json::json_pointer("/audio/volume"), result.volume);
    if (volume >= 0 && volume <= 100) result.volume = volume;
    result.mute = Settings::Bool(document, Json::json_pointer("/audio/mute"), result.mute);
    const int menu_volume = Settings::Int(document, Json::json_pointer("/audio/menu_volume"), result.menu_volume);
    if (menu_volume >= 0 && menu_volume <= 100) result.menu_volume = menu_volume;
    result.detailed_logging = Settings::Bool(document, Json::json_pointer("/diagnostics/detailed_logging"),
                                             result.detailed_logging);
    result.immediate_logs = Settings::Bool(document, Json::json_pointer("/diagnostics/immediate_logs"),
                                           result.immediate_logs);
    result.resolution = KeyIndex(Settings::String(document, Json::json_pointer("/video/resolution")),
                                 kResolutionKeys, int(std::size(kResolutionKeys)), result.resolution);
    result.upscaling_filter = KeyIndex(Settings::String(document, Json::json_pointer("/video/upscaling_filter")),
                                       kUpscalingFilterKeys, int(std::size(kUpscalingFilterKeys)),
                                       result.upscaling_filter);
    result.refresh = KeyIndex(Settings::String(document, Json::json_pointer("/video/refresh_rate")),
                              kRefreshKeys, int(std::size(kRefreshKeys)), result.refresh);
    result.output = KeyIndex(Settings::String(document, Json::json_pointer("/video/output_resolution")),
                             kOutputKeys, int(std::size(kOutputKeys)), result.output);
    result.frame_gen = Settings::Bool(document, Json::json_pointer("/video/frame_gen"), result.frame_gen);
    result.frame_gen_target = KeyIndex(
        Settings::String(document, Json::json_pointer("/video/frame_gen_target")), kFrameGenTargetKeys,
        int(std::size(kFrameGenTargetKeys)), result.frame_gen_target);
    result.frame_gen_multiplier = KeyIndex(
        Settings::String(document, Json::json_pointer("/video/frame_gen_multiplier")),
        kFrameGenMultiplierKeys, int(std::size(kFrameGenMultiplierKeys)), result.frame_gen_multiplier);
    result.vibration = Settings::Bool(document, Json::json_pointer("/controls/vibration"), result.vibration);
    result.language = KeyIndex(Settings::String(document, Json::json_pointer("/system/language")),
                               kLanguageKeys, int(std::size(kLanguageKeys)), result.language);
    result.large_text = Settings::Bool(document, Json::json_pointer("/accessibility/large_text"), false);
    result.high_contrast = Settings::Bool(document, Json::json_pointer("/accessibility/high_contrast"), false);
    result.reduce_motion = Settings::Bool(document, Json::json_pointer("/accessibility/reduce_motion"), false);
    result.mapping = Settings::Mapping(document, Json::json_pointer("/controls/mapping"), kDefaultMapping);
    return result;
}

inline bool SavePreferences(const Preferences& value, const std::string& file = SettingsFile()) {
    if (value.volume < 0 || value.volume > 100 || value.menu_volume < 0 || value.menu_volume > 100 ||
        (value.backend != GraphicsBackend::OpenGL && value.backend != GraphicsBackend::Vulkan) ||
        value.resolution < 0 || value.resolution >= int(std::size(kResolutionKeys)) ||
        value.upscaling_filter < 0 || value.upscaling_filter >= int(std::size(kUpscalingFilterKeys)) ||
        value.refresh < 0 || value.refresh >= int(std::size(kRefreshKeys)) ||
        value.output < 0 || value.output >= int(std::size(kOutputKeys)) ||
        value.language < 0 || value.language >= int(std::size(kLanguageKeys)) ||
        !ValidMapping(value.mapping)) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    document["video"]["renderer"] = value.backend == GraphicsBackend::Vulkan ? "vulkan" : "opengl";
    document["video"]["fps_overlay"] = value.hud;
    document["video"]["resolution"] = kResolutionKeys[value.resolution];
    document["video"]["upscaling_filter"] = kUpscalingFilterKeys[value.upscaling_filter];
    document["video"]["refresh_rate"] = kRefreshKeys[value.refresh];
    document["video"]["output_resolution"] = kOutputKeys[value.output];
    document["video"]["frame_gen"] = value.frame_gen;
    document["video"]["frame_gen_target"] = kFrameGenTargetKeys[value.frame_gen_target];
    document["video"]["frame_gen_multiplier"] = kFrameGenMultiplierKeys[value.frame_gen_multiplier];
    document["audio"]["volume"] = value.volume;
    document["audio"]["mute"] = value.mute;
    document["audio"]["menu_volume"] = value.menu_volume;
    document["controls"]["vibration"] = value.vibration;
    if (value.mapping == kDefaultMapping) document["controls"].erase("mapping");
    else document["controls"]["mapping"] = Settings::MappingJson(value.mapping);
    document["system"]["language"] = kLanguageKeys[value.language];
    document["diagnostics"]["detailed_logging"] = value.detailed_logging;
    document["diagnostics"]["immediate_logs"] = value.immediate_logs;
    document["accessibility"]["large_text"] = value.large_text;
    document["accessibility"]["high_contrast"] = value.high_contrast;
    document["accessibility"]["reduce_motion"] = value.reduce_motion;
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
                                                   "unsafe_dma", "reactive_flushing", "skip_invalidation"};
inline constexpr int kPerformanceSwitches = int(std::size(kPerformanceKeys));

// Library > Game settings: what one game does differently from Settings. Each value is -1 (absent
// from the file) while the game follows Settings; switches are 0 off, 1 on.
struct GameSettings {
    int renderer = -1;          // 0 OpenGL, 1 Vulkan
    int resolution = -1;        // index into kResolutionKeys
    int upscaling_filter = -1;  // index into kUpscalingFilterKeys
    int refresh = -1;           // index into kRefreshKeys
    int hud = -1;               // FPS overlay
    int frame_gen = -1;         // Lossless Scaling frame generation (Vulkan only)
    int frame_gen_target = -1;  // index into kFrameGenTargetKeys
    int frame_gen_multiplier = -1;  // index into kFrameGenMultiplierKeys
    int volume = -1;            // game volume, 0 to 100
    int mute = -1;
    int vibration = -1;
    int language = -1;          // index into kLanguageKeys
    int controller = -1;        // index into kControllerKeys
    bool own_mapping = false;   // the game has a button mapping of its own: mapping
    ButtonMapping mapping = kDefaultMapping;
    std::array<int, kPerformanceSwitches> performance{-1, -1, -1, -1, -1, -1, -1};  // kPerformanceKeys
};
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
    result.refresh = KeyIndex(key("refresh_rate"), kRefreshKeys, int(std::size(kRefreshKeys)), -1);
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
    const int volume = Settings::Int(document, Json::json_pointer(base + "/volume"), -1);
    result.volume = volume >= 0 && volume <= 100 ? volume : -1;
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
        value.refresh >= int(std::size(kRefreshKeys)) || value.language >= int(std::size(kLanguageKeys)) ||
        value.controller >= int(std::size(kControllerKeys)) ||
        value.frame_gen_target >= int(std::size(kFrameGenTargetKeys)) ||
        value.frame_gen_multiplier >= int(std::size(kFrameGenMultiplierKeys)) ||
        value.volume > 100 || !flag_ok(value.hud) || !flag_ok(value.frame_gen) || !flag_ok(value.mute) ||
        !flag_ok(value.vibration) ||
        !std::all_of(value.performance.begin(), value.performance.end(), flag_ok) ||
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
    store("refresh_rate", value.refresh, kRefreshKeys);
    store("frame_gen_target", value.frame_gen_target, kFrameGenTargetKeys);
    store("frame_gen_multiplier", value.frame_gen_multiplier, kFrameGenMultiplierKeys);
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
    if (value.own_mapping) game["mapping"] = Settings::MappingJson(value.mapping);
    else game.erase("mapping");
    auto& performance = game["performance"];
    if (!performance.is_object()) performance = Settings::Json::object();
    for (int i = 0; i < kPerformanceSwitches; ++i) flag(performance, kPerformanceKeys[i], value.performance[i]);
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
    if (game.refresh >= 0) result.refresh = game.refresh;
    if (game.hud >= 0) result.hud = game.hud == 1;
    if (game.frame_gen >= 0) result.frame_gen = game.frame_gen == 1;
    if (game.frame_gen_target >= 0) result.frame_gen_target = game.frame_gen_target;
    if (game.frame_gen_multiplier >= 0) result.frame_gen_multiplier = game.frame_gen_multiplier;
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
    };
    read("");
    if (title_id) read("/games/" + Settings::TitleKey(title_id));
    return result;
}

// Title 0 saves the general values.
inline bool SavePerformance(uint64_t title_id, const PerformanceSettings& value,
                            const std::string& file = SettingsFile()) {
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& owner = title_id ? document["games"][Settings::TitleKey(title_id)] : document;
    if (!owner.is_object()) owner = Settings::Json::object();
    owner["performance"] = {{"block_list", value.block_list}, {"async_shaders", value.async_shaders},
                            {"fast_gpu", value.fast_gpu}, {"unsafe_cpu", value.unsafe_cpu},
                            {"unsafe_dma", value.unsafe_dma}, {"reactive_flushing", value.reactive_flushing},
                            {"skip_invalidation", value.skip_invalidation}};
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

// The Library's Mods switch: one switch for all of a game's mods, on unless the player turned it
// off ("mods": false). The mods' own switches (mods_off) keep their state behind it.
inline bool LoadModsEnabled(uint64_t title_id, const std::string& file = SettingsFile()) {
    if (!title_id) return true;
    using Settings::Json;
    return Settings::Bool(Settings::Load(file),
                          Json::json_pointer("/games/" + Settings::TitleKey(title_id) + "/mods"), true);
}

inline bool SaveModsEnabled(uint64_t title_id, bool enabled, const std::string& file = SettingsFile()) {
    if (!title_id) return false;
    Settings::Json document = Settings::Load(file);
    document["version"] = 1;
    auto& game = document["games"][Settings::TitleKey(title_id)];
    if (!game.is_object()) game = Settings::Json::object();
    if (enabled) game.erase("mods");
    else game["mods"] = false;
    return Settings::Write(document, file);
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

inline std::string LoadLastGame(const std::string& file = SettingsFile()) {
    const Settings::Json document = Settings::Load(file);
    const std::string name = Settings::String(document, Settings::Json::json_pointer("/library/last_game"));
    return ValidRomFilename(name) ? name : std::string{};
}

inline bool SaveLastGame(std::string_view name, const std::string& file = SettingsFile()) {
    if (!ValidRomFilename(name)) return false;
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
        if (recent.size() < 4 && ValidRomFilename(name) && std::find(recent.begin(), recent.end(), name) == recent.end())
            recent.push_back(name);
    }
    return recent;
}

inline bool SaveRecentGame(std::string_view name, const std::string& file = SettingsFile()) {
    if (!ValidRomFilename(name)) return false;
    auto recent = LoadRecentGames(file);
    recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
    recent.insert(recent.begin(), std::string(name));
    if (recent.size() > 4) recent.resize(4);
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
    const int port = Settings::Int(document, Settings::Json::json_pointer("/multiplayer/port"), 24872);
    result.port = port > 0 && port <= 65535 ? port : 24872;
    if (result.host.size() > 253) result.host.clear();
    if (result.nickname.size() > 20) result.nickname.clear();
    return result;
}

inline bool SaveRoomConnection(const Multiplayer::Connection& value,
                               const std::string& file = SettingsFile()) {
    auto document = Settings::Load(file);
    document["multiplayer"] = {{"host", value.host}, {"port", value.port}, {"nickname", value.nickname}};
    return Settings::Write(document, file);
}

inline int AudioVolume(const Preferences& value) {
    return value.mute ? 0 : 0x8000 * value.volume / 100;
}
} // namespace Eden
