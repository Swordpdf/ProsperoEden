// ProsperoEden - Sample data for the launcher preview on a PC (no console, no game files).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_services.hpp"

#include "pe/core/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace pe::host
{

namespace
{

struct Sample
{
    const char *name;
    const char *format;
    const char *size;
    const char *update;   // its version, or "" without one
    int dlc;
    const char *language; // the language the game will use
    const char *missing;  // the chosen language when the game lacks it, else ""
    std::uint32_t sky;  // cover colours
    std::uint32_t land;
    std::uint32_t mark;
    bool mods = false; // has the sample mods (FakeServices::mods)
};

// Invented titles: nothing here names a real game.
constexpr Sample kSamples[] = {
    {"Starfall Odyssey", "NSP", "6.4 GB", "1.2.0", 2, "English (US)", "", 0x1b2a6b, 0x40b3c8, 0xffd166},
    {"Moss & Lantern", "XCI", "2.1 GB", "", 0, "English (US)", "", 0x16402f, 0x7bc86c, 0xf4e285},
    {"Kart Carnival Deluxe", "NSP", "7.8 GB", "3.0.1", 48, "English (US)", "", 0xb3261e, 0xffb238, 0xffffff},
    {"Tiny Harbor", "NSP", "512.0 MB", "", 0, "English (US)", "", 0x256d8f, 0x9bd8e6, 0xfff3d6},
    {"Echoes of the Valley", "XCI", "14.2 GB", "1.1.0", 0, "Spanish",
     "Portuguese (Brazil)", 0x3b1f5e, 0xc77dff, 0xffe0f5, true},
    {"Pocket Rally Turbo", "NSP", "1.9 GB", "", 0, "English (US)", "", 0x202020, 0xe85d04, 0xf8f9fa},
    {"Cloudline", "NSP", "3.3 GB", "", 1, "English (US)", "", 0x5fa8d3, 0xcae9ff, 0x1b4965},
    {"Ember Knights II", "XCI", "9.6 GB", "2.4.0", 5, "English (US)", "", 0x3d0c02, 0xd62828, 0xfcbf49},
    {"Paper Garden and the Clockwork Orchard", "NSP", "840.5 MB", "", 0, "English (US)", "", 0xf1e3c6, 0x90be6d, 0x386641},
    {"Neon Drifters", "NSP", "5.2 GB", "1.0.3", 0, "English (US)", "", 0x10002b, 0x7b2cbf, 0x5ef2ff},
    {"Caf\xC3\xA9 Nocturne", "NSP", "2.7 GB", "", 0, "French", "", 0x2b1d0e, 0xa9713c, 0xf6e7cb},
    {"Sky Shepherds", "XCI", "4.4 GB", "", 3, "English (US)", "", 0x457b9d, 0xa8dadc, 0xf1faee, true},
};

void put_pixel(std::vector<std::uint8_t> &pixels, int size, int x, int y, float r, float g, float b)
{
    std::uint8_t *out = pixels.data() + (static_cast<std::size_t>(y) * size + x) * 4;
    out[0] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    out[1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    out[2] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    out[3] = 255;
}

// A simple poster: sky gradient, a sun or moon, two ranges of hills.
bool write_cover(const std::string &path, const Sample &sample, int index)
{
    constexpr int kSize = 256;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
    const auto channel = [](std::uint32_t color, int shift)
    { return static_cast<float>((color >> shift) & 0xff) / 255.0f; };
    const float sky[3] = {channel(sample.sky, 16), channel(sample.sky, 8), channel(sample.sky, 0)};
    const float land[3] = {channel(sample.land, 16), channel(sample.land, 8), channel(sample.land, 0)};
    const float mark[3] = {channel(sample.mark, 16), channel(sample.mark, 8), channel(sample.mark, 0)};
    const float sun_x = 60.0f + static_cast<float>((index * 53) % 140);
    const float sun_y = 70.0f + static_cast<float>((index * 31) % 50);
    const float sun_r = 26.0f + static_cast<float>((index * 7) % 14);
    for (int y = 0; y < kSize; ++y)
    {
        for (int x = 0; x < kSize; ++x)
        {
            const float v = static_cast<float>(y) / (kSize - 1);
            float color[3];
            for (int k = 0; k < 3; ++k)
                color[k] = sky[k] * (1.0f - v * 0.55f) + land[k] * v * 0.35f;
            const float distance = std::hypot(static_cast<float>(x) - sun_x, static_cast<float>(y) - sun_y);
            const float disc = std::clamp(sun_r - distance + 0.5f, 0.0f, 1.0f);
            const float halo = std::exp(-distance / (sun_r * 1.6f)) * 0.35f;
            for (int k = 0; k < 3; ++k)
                color[k] = color[k] * (1.0f - disc) + mark[k] * disc + mark[k] * halo;
            const float fx = static_cast<float>(x);
            const float far_hill = 168.0f + 18.0f * std::sin(fx * 0.031f + index) +
                                   9.0f * std::sin(fx * 0.083f + index * 2.0f);
            const float near_hill = 204.0f + 22.0f * std::sin(fx * 0.024f + index * 1.7f + 2.0f) +
                                    7.0f * std::sin(fx * 0.11f + index);
            if (static_cast<float>(y) > far_hill)
                for (int k = 0; k < 3; ++k)
                    color[k] = land[k] * 0.72f + sky[k] * 0.18f;
            if (static_cast<float>(y) > near_hill)
                for (int k = 0; k < 3; ++k)
                    color[k] = land[k] * 0.38f + sky[k] * 0.10f;
            put_pixel(pixels, kSize, x, y, color[0], color[1], color[2]);
        }
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    const unsigned char header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      kSize & 0xff, kSize >> 8, kSize & 0xff, kSize >> 8, 32, 0x28};
    const bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
                    std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    std::fclose(file);
    return ok;
}

// Invented games on the sample download sources.
constexpr Sample kServerSamples[] = {
    {"Lighthouse Keeper", "NSP", "3.2 GB", "", 0, "English (US)", "", 0x0b1d3a, 0x2a6f97, 0xffe66d},
    {"Rune Gardens", "XCI", "11.6 GB", "", 0, "English (US)", "", 0x1f3d1a, 0x9bc53d, 0xe55934},
    {"Orbit Postman", "NSP", "780.4 MB", "", 0, "English (US)", "", 0x14051f, 0x6a4c93, 0x8ac926},
};
constexpr std::uint64_t kServerSizes[] = {3435973837ull, 12455405158ull, 818311987ull};
// What of them is in .remote-downloads/ already (in percent), from a download stopped before:
// Orbit Postman's goes on, read from the drive first for the check of its contents.
constexpr std::uint64_t kServerHad[] = {0, 0, 40};
// Which sources have them: Rune Gardens is on both.
const std::vector<std::string> kServerSources[] = {{"Home"}, {"Home", "Office"}, {"Office"}};

// The same labels as the console's settings (headless/settings_store.h), translated like them.
constexpr const char *kResolutionLabels[] = {"0.5x (faster, softer)", "0.75x (faster)",
                                             "1x (native)", "1.5x (sharper)", "2x (sharpest)",
                                             "3x (slower)", "4x (slowest)"};
const std::vector<std::string> kResolutionKeys = {"0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"};
constexpr const char *kFilterLabels[] = {"Bilinear", "AMD FSR", "Bicubic", "Nearest"};
constexpr const char *kAstcLabels[] = {"Compressed", "Full quality"};
constexpr const char *kLanguageLabels[] = {
    "English (US)", "English (UK)", "French", "French (Canada)", "German", "Italian", "Spanish",
    "Spanish (Latin America)", "Portuguese", "Portuguese (Brazil)", "Dutch", "Russian", "Polish",
    "Japanese", "Korean", "Chinese (Simplified)", "Chinese (Traditional)", "Thai"};

template <std::size_t N> std::vector<std::string> translated(const char *const (&labels)[N])
{
    std::vector<std::string> out;
    for (const char *label : labels)
        out.emplace_back(tr(label));
    return out;
}
constexpr int kLanguageRegions[] = {1, 2, 2, 1, 2, 2, 2, 1, 2, 1, 2, 2, 2, 0, 5, 4, 6, 1};

} // namespace

FakeServices::FakeServices(const std::string &covers_directory)
{
    wallpapers_directory_ = covers_directory + "/wallpapers";
    int index = 0;
    for (const Sample &sample : kSamples)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "0100%04X0000%04X", 0xA000 + index, 0x1000 * (index % 8));
        ui::Game game;
        game.name = sample.name;
        game.format = sample.format;
        game.size = sample.size;
        game.file = std::string(sample.name) + " [" + id + "]." +
                    (std::string(sample.format) == "NSP" ? "nsp" : "xci");
        game.title_id = 0x0100A00000001000ull + static_cast<std::uint64_t>(index) * 0x10000;
        if (sample.update[0] != 0)
        {
            game.addons = fill(tr("Update {0}"), {sample.update});
            game.addons_short = std::string("v") + sample.update;
        }
        if (sample.dlc > 0)
        {
            const std::string dlc = fill(tr("{0} DLC"), {std::to_string(sample.dlc)});
            game.addons += (game.addons.empty() ? "" : ", ") + dlc;
            game.addons_short += (game.addons_short.empty() ? "" : ", ") + dlc;
        }
        if (sample.mods)
            modded_.push_back(game.title_id);
        game.language = tr(sample.language);
        if (sample.missing[0] != 0)
            game.language_note = fill(tr("{0} not available"), {tr(sample.missing)});
        // One game has no cover art, to show the placeholder.
        if (index != 8)
        {
            game.cover = covers_directory + "/cover-" + std::to_string(index) + ".tga";
            if (!write_cover(game.cover, sample, index))
                game.cover.clear();
        }
        games_.push_back(game);
        ++index;
    }
    for (const Sample &sample : kServerSamples)
    {
        ui::Game game;
        game.name = sample.name;
        game.format = sample.format;
        game.size = sample.size;
        game.file = std::string(sample.name) + "." + (std::string(sample.format) == "NSP" ? "nsp" : "xci");
        game.sources = kServerSources[remote_.size() % 3];
        game.key = "name:" + game.file;
        game.remote = true;
        game.cover = covers_directory + "/server-" + std::to_string(remote_.size()) + ".tga";
        if (!write_cover(game.cover, sample, index++))
            game.cover.clear();
        remote_.push_back(game);
    }
    // Runtime preview: exercise the complete scrolling library without shipping game payloads.
    for(int extra=static_cast<int>(games_.size());extra<64;++extra) {
        ui::Game game=games_[static_cast<std::size_t>(extra)%games_.size()];
        game.name="Sample Software "+std::to_string(extra+1); game.file=game.name+".nsp";
        game.title_id=0x0100C00000000000ull+static_cast<std::uint64_t>(extra); games_.push_back(std::move(game));
    }
    std::sort(games_.begin(), games_.end(),
              [](const ui::Game &a, const ui::Game &b) { return a.name < b.name; });
}

ui::Home FakeServices::home()
{
    ui::Home home;
    home.setup_ready = setup_ready;
    if (!setup_ready)
        home.status = fill(tr("Setup required: {0} Open Settings, Game files to choose the folder that "
                              "holds your keys, firmware and roms folders (or add the files to {1}), "
                              "then reopen ProsperoEden."),
                           {fill(tr("Missing or empty keys/prod.keys in {0}."), {"/data/prosperoeden"}),
                            "/data/prosperoeden"});
    else if (!crash_report.empty())
    {
        home.status = fill(tr("ProsperoEden stopped because of an error. A report was saved to {0}."), {crash_report});
        home.launch_failed = true;
    }
    else if (!launch_error.empty())
    {
        home.status = fill(tr("Game could not start: {0} Details: {1}"),
                           {launch_error, "/data/prosperoeden/logs/stderr.log"});
        home.launch_failed = true;
    }
    if (has_history && setup_ready)
    {
        const auto find = [&](const char *name) -> const ui::Game &
        {
            for (const ui::Game &game : games_)
                if (game.name == name)
                    return game;
            return games_.front();
        };
        // As the console does: a game taken away is not offered, the next recent one is.
        const char *const recent[] = {"Echoes of the Valley", "Kart Carnival Deluxe", "Starfall Odyssey",
                                      "Caf\xC3\xA9 Nocturne", "Neon Drifters", "Pocket Rally Turbo",
                                      "Cloudline", "Ember Knights II"};
        const char *first = recent[0];
        for (const char *name : recent)
            if (game_exists(find(name).file))
            {
                first = name;
                break;
            }
        const ui::Game &last = find(first);
        home.last_file = last.file;
        home.last_exists = true;
        home.last_title = last.name;
        home.last_caption = last.language_note;
        home.last_caption_warning = true;
        home.last_cover = last.cover;
        home.last_title_id = last.title_id;
        home.last_addons = last.addons;
        home.last_language = last.language;
        for (const char *name : recent)
        {
            const ui::Game &game = find(name);
            if (game_exists(game.file))
                home.recents.push_back({game.file, game.name, game.cover});
        }
    }
    home.system_status = fill(tr("{0} games installed"), {std::to_string(games_.size())}) + "  /  " +
                         (setup_ready ? tr("Firmware ready") : tr("Setup required"));
    return home;
}

bool FakeServices::docked(std::uint64_t title_id)
{
    return std::find(handheld_.begin(), handheld_.end(), title_id) == handheld_.end();
}

bool FakeServices::set_docked(std::uint64_t title_id, bool docked)
{
    handheld_.erase(std::remove(handheld_.begin(), handheld_.end(), title_id), handheld_.end());
    if (!docked)
        handheld_.push_back(title_id);
    return true;
}

const std::vector<std::string> &FakeServices::resolution_labels()
{
    static const std::vector<std::string> labels = translated(kResolutionLabels);
    return labels;
}
const std::vector<std::string> &FakeServices::resolution_keys()
{
    return kResolutionKeys;
}
const std::vector<std::string> &FakeServices::filter_labels()
{
    static const std::vector<std::string> labels = translated(kFilterLabels);
    return labels;
}
const std::vector<std::string> &FakeServices::astc_labels()
{
    static const std::vector<std::string> labels = translated(kAstcLabels);
    return labels;
}
// The bundled presets the preview shows, as a console with no presets of its own lists them.
const std::vector<std::string> &FakeServices::post_processing_labels()
{
    static const std::vector<std::string> labels = {
        tr("Off"), "Anime", "Cinematic", "Clean", "Dreamy", "Retro", "Vivid"};
    return labels;
}
const std::vector<std::string> &FakeServices::language_labels()
{
    static const std::vector<std::string> labels = translated(kLanguageLabels);
    return labels;
}

std::string FakeServices::language_region(int language)
{
    static constexpr const char *kRegions[] = {"Japan", "USA", "Europe", "Australia",
                                               "China", "Korea", "Taiwan"};
    return language >= 0 && language < 18 ? tr(kRegions[kLanguageRegions[language]]) : "";
}

std::string FakeServices::setup_details()
{
    return setup_ready ?
               tr("Keys and firmware: startup checks passed. Game-specific compatibility is checked at "
                  "launch.") :
               fill(tr("Missing or empty keys/prod.keys in {0}."), {"/data/prosperoeden"});
}

std::vector<std::string> FakeServices::wallpapers()
{
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it{wallpapers_directory_, error}, end; !error && it != end;
         it.increment(error))
        if (it->is_regular_file())
            names.push_back(it->path().filename().string());
    // Sorted without regard to case, as the console lists a folder (eden_services.cpp).
    const auto lower = [](std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::sort(names.begin(), names.end(),
              [&](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    return names;
}

bool FakeServices::folders(const std::string &directory, std::vector<std::string> *names)
{
    names->clear();
    if (directory == "/mnt/ext1/eden")
        *names = {"firmware", "keys", "roms", "updates"};
    else if (directory == "/mnt/ext1")
        *names = {"backups", "eden", "media", "music", "photos", "retro", "saves", "videos"};
    else if (directory == "/mnt")
        *names = {"ext0", "ext1", "usb0"};
    else if (directory == "/")
        *names = {"data", "mnt", "user"};
    else if (directory == "/data")
        *names = {"homebrew", "prosperoeden"};
    else if (directory == "/data/prosperoeden")
        *names = {"config", "covers", "logs"};
    return true;
}

std::vector<ui::Mod> FakeServices::mods(std::uint64_t title_id)
{
    if (!has_mods || std::find(modded_.begin(), modded_.end(), title_id) == modded_.end())
        return {};
    std::vector<ui::Mod> mods = {
        {"60 FPS", tr("Patch"), true, {}},
        {"Sharper textures", tr("Files"), true, {}},
        {"Starter pack", std::string(tr("Files")) + ", " + tr("Cheats"), true, {}},
    };
    for (ui::Mod &mod : mods)
        mod.enabled = std::find(mods_off_.begin(), mods_off_.end(), mod.name) == mods_off_.end();
    for (const char *cheat : {"60 FPS", "30 FPS", "Infinite health", "All items", "Moon jump"})
        mods.back().cheats.push_back(
            {cheat, std::find(cheats_on_.begin(), cheats_on_.end(), cheat) != cheats_on_.end()});
    return mods;
}

bool FakeServices::set_cheat_enabled(std::uint64_t, const std::string &, const std::string &cheat,
                                     bool enabled)
{
    const auto drop = [this](const std::string &name)
    { cheats_on_.erase(std::remove(cheats_on_.begin(), cheats_on_.end(), name), cheats_on_.end()); };
    drop(cheat);
    if (!enabled)
        return true;
    if (cheat == "60 FPS" || cheat == "30 FPS")
    {
        drop("60 FPS");
        drop("30 FPS");
    }
    cheats_on_.push_back(cheat);
    return true;
}

bool FakeServices::set_mod_enabled(std::uint64_t, const std::string &name, bool enabled)
{
    mods_off_.erase(std::remove(mods_off_.begin(), mods_off_.end(), name), mods_off_.end());
    if (!enabled)
        mods_off_.push_back(name);
    return true;
}

ui::FolderInfo FakeServices::folder_info(const std::string &directory)
{
    ui::FolderInfo info;
    if (directory == "/mnt/ext1/eden")
    {
        info.keys = true;
        info.firmware = 236;
        info.games = static_cast<int>(games_.size());
    }
    return info;
}

bool FakeServices::delete_game(const ui::Game &game, std::string *message)
{
    const auto local = std::find_if(games_.begin(), games_.end(),
                                    [&](const ui::Game &g) { return g.file == game.file && !g.sources.empty(); });
    if (local == games_.end())
        return false;
    // On its sources only again.
    ui::Game remote = *local;
    remote.remote = true;
    remote.title_id = 0;
    remote_.push_back(remote);
    games_.erase(local);
    ++generation_;
    *message = fill(tr("Deleted {1} files ({0}). Save data and settings are kept."), {game.size, "2"});
    return true;
}

} // namespace pe::host

namespace pe::host
{

ui::Sources FakeServices::sources()
{
    ui::Sources sources;
    sources.setup_file = "/data/prosperoeden/config/remote/sources.json";
    sources.generation = generation_;
    if (!has_server)
        return sources;
    sources.configured = true;
    sources.list.push_back({"Home", "http://192.168.1.20:3000", false, true, "", 12});
    sources.list.push_back({"Office", "https://games.example.org", false, !server_fails,
                            server_fails ? "The server did not answer" : "", server_fails ? 0 : 5});
    return sources;
}

bool FakeServices::download(const ui::Game &game, int source, bool first)
{
    const auto found = std::find_if(remote_.begin(), remote_.end(), [&](const ui::Game &g) { return g.key == game.key; });
    if (found == remote_.end() || source < 0 || source >= static_cast<int>(found->sources.size()))
        return false;
    for (ui::Download &download : queue_)
        if (download.key == game.key)
        {
            if (download.state == ui::DownloadState::failed)
            {
                download.state = ui::DownloadState::queued;
                download.error.clear();
                download_fails = false;
            }
            return true;
        }
    ui::Download download;
    download.key = found->key;
    download.file = found->file;
    download.name = found->name;
    download.source = found->sources[static_cast<std::size_t>(source)];
    download.cover = found->cover;
    for (std::size_t i = 0; i < std::size(kServerSamples); ++i)
        if (found->name == kServerSamples[i].name)
        {
            download.total = kServerSizes[i];
            download.done = kServerSizes[i] * kServerHad[i] / 100;
        }
    if (first)
    {
        // After the one downloading.
        auto at = queue_.begin();
        while (at != queue_.end() &&
               (at->state == ui::DownloadState::downloading || at->state == ui::DownloadState::verifying))
            ++at;
        queue_.insert(at, download);
    }
    else
    {
        queue_.push_back(download);
    }
    return true;
}

bool FakeServices::cancel_download(const std::string &key)
{
    const auto at = std::find_if(queue_.begin(), queue_.end(), [&](const ui::Download &d) { return d.key == key; });
    if (at == queue_.end())
        return false;
    going_on_.erase(std::remove_if(going_on_.begin(), going_on_.end(), [&](const auto &g) { return g.first == key; }),
                    going_on_.end());
    queue_.erase(at);
    return true;
}

std::vector<ui::Download> FakeServices::downloads()
{
    // One step of the first game that can move: 2.5% of it.
    for (ui::Download &download : queue_)
    {
        if (download.state == ui::DownloadState::failed)
            continue;
        // One that goes on: what it had is read first, for the check of its contents, a step of 10%
        // a look, up to where it was (verifying), then it downloads from there.
        auto going_on = std::find_if(going_on_.begin(), going_on_.end(), [&](const auto &g) { return g.first == download.key; });
        if (download.state == ui::DownloadState::queued && download.done > 0 && going_on == going_on_.end())
        {
            going_on_.emplace_back(download.key, download.done);
            download.state = ui::DownloadState::verifying;
            download.done = 0;
            download.rate = 0;
            break;
        }
        if (download.state == ui::DownloadState::verifying)
        {
            download.done = std::min(going_on->second, download.done + download.total / 10);
            if (download.done >= going_on->second)
                download.state = ui::DownloadState::downloading;
            break;
        }
        download.state = ui::DownloadState::downloading;
        download.done = std::min(download.total, download.done + download.total / 40);
        download.rate = 48ull << 20;
        if (download_fails && download.done * 10 >= download.total * 3)
        {
            download.state = ui::DownloadState::failed;
            download.error = "The connection timed out";
        }
        else if (download.done >= download.total)
        {
            // On the console now: it joins the library.
            const auto game = std::find_if(remote_.begin(), remote_.end(),
                                           [&](const ui::Game &g) { return g.file == download.file; });
            if (game != remote_.end())
            {
                ui::Game local = *game;
                local.remote = false;
                local.title_id = 0x0100B00000001000ull + static_cast<std::uint64_t>(game - remote_.begin()) * 0x10000;
                local.language = tr("English (US)");
                games_.push_back(local);
                remote_.erase(game);
            }
            const std::string file = download.file;
            queue_.erase(std::find_if(queue_.begin(), queue_.end(), [&](const ui::Download &d) { return d.file == file; }));
            ++generation_;
        }
        break;
    }
    return queue_;
}

// ---- save sync ----

void FakeServices::start_save_sync(const std::string &file, bool before)
{
    sync_ = {};
    sync_.stage = ui::SaveSyncStage::working;
    sync_.before = before;
    sync_.profile = "Player 1";
    for (const ui::Game &game : games_)
        if (game.file == file)
            sync_.game = game.name;
    sync_looks_ = 0;
}

ui::SaveSync FakeServices::save_sync()
{
    // A few looks while it works, then what came of it.
    if (sync_.stage == ui::SaveSyncStage::working && ++sync_looks_ >= 8)
    {
        if (save_sync_too_old)
        {
            sync_.stage = ui::SaveSyncStage::failed;
            sync_.error = "RomM 4.9.2 is older than the save sync takes: it needs RomM 5.0.0 or newer";
            sync_.too_old = true;
            sync_.server_version = "4.9.2";
            sync_.needed_version = "5.0.0";
        }
        else if (save_sync_fails)
        {
            sync_.stage = ui::SaveSyncStage::failed;
            sync_.error = "The server did not answer";
        }
        else if (save_sync_conflict)
        {
            sync_.stage = ui::SaveSyncStage::conflict;
            sync_.console_time = 1791457200; // 2026-10-08, 11:00 UTC
            sync_.server_time = 1791468000;  // and 14:00
            sync_.server_device = "Pixel 8";
            save_sync_conflict = false;
        }
        else
        {
            sync_.stage = ui::SaveSyncStage::done;
            sync_.outcome = sync_.before ? ui::SaveSyncOutcome::same : ui::SaveSyncOutcome::uploaded;
        }
    }
    return sync_;
}

void FakeServices::choose_save_data(ui::SaveChoice choice)
{
    if (sync_.stage != ui::SaveSyncStage::conflict)
        return;
    sync_.stage = ui::SaveSyncStage::working;
    sync_looks_ = 0;
    sync_.outcome = choice == ui::SaveChoice::console ? ui::SaveSyncOutcome::uploaded :
                    choice == ui::SaveChoice::server  ? ui::SaveSyncOutcome::downloaded :
                                                        ui::SaveSyncOutcome::kept;
}

void FakeServices::played(const std::string &name)
{
    sync_ = {};
    sync_.stage = ui::SaveSyncStage::working;
    sync_.before = false;
    sync_.game = name;
    sync_.profile = "Player 1";
    sync_looks_ = 0;
}

// ---- Settings > Save sync ----

ui::SaveSyncSetup FakeServices::save_sync_setup()
{
    ui::SaveSyncSetup setup;
    setup.file = "/data/prosperoeden/config/remote/save-sync.json";
    const char *names[] = {"Player 1", "Kids"};
    for (int i = 0; i < 2; ++i)
    {
        ui::SaveSyncProfile profile;
        profile.name = names[i];
        profile.current = i == 0;
        profile.linked = !linked_[static_cast<std::size_t>(i)].empty();
        profile.server = linked_[static_cast<std::size_t>(i)];
        setup.profiles.push_back(profile);
    }
    return setup;
}

bool FakeServices::start_pairing(int profile, int server)
{
    pair_ = {};
    pair_.stage = ui::PairingStage::asking;
    pair_.profile = profile == 0 ? "Player 1" : "Kids";
    pair_.server = pair_servers()[static_cast<std::size_t>(server)].name;
    pair_profile_ = profile;
    pair_looks_ = 0;
    return true;
}

ui::PairingStatus FakeServices::pairing()
{
    ++pair_looks_;
    if (pair_.stage == ui::PairingStage::asking && pair_looks_ > 2)
    {
        pair_.stage = ui::PairingStage::waiting;
        pair_.code = "K7QM2XWP";
        pair_.address = "http://192.168.1.20:3000/pair/device?user_code=K7QM2XWP";
        pair_.seconds_left = 598;
    }
    else if (pair_.stage == ui::PairingStage::waiting && pair_looks_ > 40)
    {
        pair_.stage = ui::PairingStage::done;
        pair_.user = "alex";
        linked_[static_cast<std::size_t>(pair_profile_)] = "alex @ http://192.168.1.20:3000";
    }
    return pair_;
}

bool FakeServices::unlink_profile(int profile)
{
    linked_[static_cast<std::size_t>(profile)].clear();
    return true;
}

} // namespace pe::host
