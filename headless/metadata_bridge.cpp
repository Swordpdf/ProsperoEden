#include "game_name.h"
#include "metadata_bridge.h"
#include "assets_dir.h"
#include "diagnostics.h"
#include "profiles.h"
#include "ryujinx_saves.h"
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Eden builds stb_image for JPEG only (src/common/stb.h), the format of a game's own icon. A cover
// from a download source (a RomM server's) is a PNG as often: this file has a reader of its own
// for both, private to it (STB_IMAGE_STATIC), beside Eden's.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG 1
#define STBI_ONLY_PNG 1
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb_image.h>
#pragma GCC diagnostic pop

#include "common/fs/path_util.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/set/settings_types.h"
#include "core/loader/loader.h"

namespace {
constexpr std::array<const char*, 18> language_names{
    "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
    "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
    "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese",
    "BrazilianPortuguese", "Polish", "Thai",
};

FileSys::VirtualDir OpenControlRomFs(const FileSys::VirtualFile& file, bool xci) {
    std::shared_ptr<FileSys::NCA> control;
    if (xci) {
        FileSys::XCI image(file);
        if (image.GetStatus() != Loader::ResultStatus::Success) return {};
        control = image.GetNCAByType(FileSys::NCAContentType::Control);
    } else {
        FileSys::NSP package(file);
        if (package.GetStatus() != Loader::ResultStatus::Success) return {};
        control = package.GetNCA(FileSys::GetBaseTitleID(package.GetProgramTitleID()),
                                 FileSys::ContentRecordType::Control);
    }
    if (!control || control->GetStatus() != Loader::ResultStatus::Success) return {};
    const auto romfs = control->GetRomFS();
    return romfs ? FileSys::ExtractRomFS(romfs) : FileSys::VirtualDir{};
}

std::string ReadTitle(const FileSys::VirtualDir& romfs) {
    auto nacp = romfs->GetFile("control.nacp");
    if (!nacp) nacp = romfs->GetFile("Control.nacp");
    if (!nacp || nacp->GetSize() < 0x3000) return {};
    // The first of the game's names (one per language, English first) that is text: a name can be
    // empty or hold something else (game_name.h), and the next language's is then the one to show.
    std::array<char, 0x200> name{};
    for (std::size_t language = 0; language < 16; ++language) {
        if (nacp->Read(reinterpret_cast<unsigned char*>(name.data()), name.size(),
                       language * 0x300) != name.size()) return {};
        name.back() = '\0';
        if (!name.front()) continue;
        if (Eden::UsableName(name.data())) return Eden::PlainName(name.data());
        std::fprintf(stderr, "[ProsperoEden] library: name %zu of a game is not text; trying its next one\n", language);
    }
    return {};
}

// The supported-language flags of a control RomFS's NACP (bit n is NS ApplicationLanguage n); 0
// when they cannot be read.
uint32_t ReadLanguages(const FileSys::VirtualDir& romfs) {
    auto nacp = romfs->GetFile("control.nacp");
    if (!nacp) nacp = romfs->GetFile("Control.nacp");
    FileSys::RawNACP raw{};
    if (!nacp || nacp->ReadObject(&raw) != sizeof(raw)) return 0;
    return static_cast<uint32_t>(raw.supported_languages);
}

// Game file -> its language flags, kept from the Library's metadata pass over the same data.
std::map<std::string, uint32_t>& GameLanguages() {
    static std::map<std::string, uint32_t> languages;
    return languages;
}

FileSys::VirtualFile FindIcon(const FileSys::VirtualDir& romfs) {
    for (const char* language : language_names) {
        if (auto icon = romfs->GetFile(std::string{"icon_"} + language + ".dat")) return icon;
    }
    return {};
}

// A picture's bytes as a 32-bit TGA, made smaller by whole steps (box filter) until it is at most
// `largest` pixels a side. A picture of more than 4096 pixels a side is not read at all (a cover
// from a server could take a lot of memory).
bool WriteTgaBytes(const unsigned char* encoded, std::size_t size, const char* output, int largest) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* rgba = stbi_load_from_memory(encoded, static_cast<int>(size), &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        stbi_image_free(rgba);
        return false;
    }
    int step = 1;
    while (std::max(width, height) / step > largest) ++step;
    const int out_width = std::max(1, width / step);
    const int out_height = std::max(1, height / step);
    std::FILE* file = std::fopen(output, "wb");
    if (!file) {
        stbi_image_free(rgba);
        return false;
    }
    unsigned char header[18]{};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(out_width);
    header[13] = static_cast<unsigned char>(out_width >> 8);
    header[14] = static_cast<unsigned char>(out_height);
    header[15] = static_cast<unsigned char>(out_height >> 8);
    header[16] = 32;
    header[17] = 0x28;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> row(static_cast<std::size_t>(out_width) * 4);
    for (int y = 0; ok && y < out_height; ++y) {
        for (int x = 0; x < out_width; ++x) {
            unsigned sum[4]{};
            for (int dy = 0; dy < step; ++dy)
                for (int dx = 0; dx < step; ++dx) {
                    const unsigned char* source =
                        rgba + (static_cast<std::size_t>(y * step + dy) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x * step + dx)) * 4;
                    for (int k = 0; k < 4; ++k) sum[k] += source[k];
                }
            const unsigned count = static_cast<unsigned>(step * step);
            row[4 * x + 0] = static_cast<unsigned char>(sum[2] / count);
            row[4 * x + 1] = static_cast<unsigned char>(sum[1] / count);
            row[4 * x + 2] = static_cast<unsigned char>(sum[0] / count);
            row[4 * x + 3] = static_cast<unsigned char>(sum[3] / count);
        }
        ok = std::fwrite(row.data(), 1, row.size(), file) == row.size();
    }
    ok = std::fclose(file) == 0 && ok;
    stbi_image_free(rgba);
    return ok;
}

bool WriteTga(const FileSys::VirtualFile& icon, const char* output) {
    const auto encoded = icon->ReadAllBytes();
    return WriteTgaBytes(encoded.data(), encoded.size(), output, 4096);
}
}

int eden_write_cover_tga(const unsigned char* encoded, size_t size, const char* cover_tga_path) {
    if (!encoded || !size || size > (64u << 20) || !cover_tga_path) return 0;
    // Written beside, then moved into place: the menu may be drawing the old one.
    const std::string staged = std::string{cover_tga_path} + ".new";
    if (!WriteTgaBytes(encoded, size, staged.c_str(), 512) || std::rename(staged.c_str(), cover_tga_path) != 0) {
        (void)std::remove(staged.c_str());
        return 0;
    }
    return 1;
}

uint64_t eden_game_title_id(const char* rom_path) {
    if (!rom_path) return 0;
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, Eden::AssetsPath("keys"));
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) return 0;
    std::string path = rom_path;
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path.ends_with(".xci")) {
        FileSys::XCI image(file);
        return image.GetStatus() == Loader::ResultStatus::Success ? image.GetProgramTitleID() : 0;
    }
    if (path.ends_with(".nsp")) {
        FileSys::NSP package(file);
        return package.GetStatus() == Loader::ResultStatus::Success ? package.GetProgramTitleID() : 0;
    }
    return 0;
}

int eden_extract_game_metadata(const char* rom_path, const char* keys_dir,
                               const char* cover_tga_path, char* title,
                               size_t title_capacity) {
    if (!rom_path || !keys_dir || !cover_tga_path || !title || title_capacity == 0) return 0;
    title[0] = '\0';
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) return 0;
    std::string path = rom_path;
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool xci = path.size() >= 4 && path.substr(path.size() - 4) == ".xci";
    const auto romfs = OpenControlRomFs(file, xci);
    if (!romfs) return 0;
    GameLanguages()[rom_path] = ReadLanguages(romfs);

    int result = 0;
    const auto extracted_title = ReadTitle(romfs);
    if (!extracted_title.empty()) {
        std::snprintf(title, title_capacity, "%s", extracted_title.c_str());
        result |= EDEN_METADATA_TITLE;
    }
    if (const auto icon = FindIcon(romfs); icon && WriteTga(icon, cover_tga_path))
        result |= EDEN_METADATA_COVER;
    return result;
}

uint32_t eden_game_supported_languages(const char* rom_path, const char* keys_dir) {
    if (!rom_path || !keys_dir) return 0;
    if (const auto known = GameLanguages().find(rom_path); known != GameLanguages().end()) return known->second;
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
        if (!file) return 0;
        std::string path = rom_path;
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto romfs = OpenControlRomFs(file, path.ends_with(".xci"));
        if (!romfs) return 0;
        return GameLanguages()[rom_path] = ReadLanguages(romfs);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ProsperoEden] languages: %s\n", error.what());
        return 0;
    }
}

namespace {
struct AddOns {
    std::string update;
    unsigned dlc = 0;
    uint32_t languages = 0;  // the update's own language flags (its control data replaces the game's)
};
// Base title ID -> its update and DLC files, from the last eden_scan_addons.
std::map<uint64_t, AddOns>& ScannedAddOns() {
    static std::map<uint64_t, AddOns> scanned;
    return scanned;
}
}

void eden_scan_addons(const char* updates_dir, const char* keys_dir) {
    auto& scanned = ScannedAddOns();
    scanned.clear();
    if (!updates_dir || !keys_dir) return;
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        auto directory = vfs.OpenDirectory(updates_dir, FileSys::OpenMode::Read);
        if (!directory) return;
        const FileSys::ExternalContentProvider provider({std::move(directory)});
        for (const auto& entry : provider.ListEntriesFilter(FileSys::TitleType::Update, std::nullopt, std::nullopt)) {
            auto& addons = scanned[FileSys::GetBaseTitleID(entry.title_id)];
            if (!addons.update.empty()) continue;
            // Newest first; the display version comes from the update's own control data.
            if (const auto versions = provider.ListUpdateVersions(entry.title_id); !versions.empty()) {
                addons.update = versions.front().version_string.empty()
                    ? "v" + std::to_string(versions.front().version) : versions.front().version_string;
            } else if (const auto version = provider.GetEntryVersion(entry.title_id)) {
                addons.update = "v" + std::to_string(*version);
            }
            if (const auto control = provider.GetEntry(entry.title_id, FileSys::ContentRecordType::Control);
                control && control->GetStatus() == Loader::ResultStatus::Success)
                if (const auto romfs = control->GetRomFS())
                    if (const auto files = FileSys::ExtractRomFS(romfs)) addons.languages = ReadLanguages(files);
        }
        std::set<uint64_t> dlc;
        for (const auto& entry : provider.ListEntriesFilter(FileSys::TitleType::AOC, std::nullopt, std::nullopt))
            if (dlc.insert(entry.title_id).second) ++scanned[FileSys::GetBaseTitleID(entry.title_id)].dlc;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ProsperoEden] updates: %s\n", error.what());
        scanned.clear();
    }
}

namespace {
// Whether an NSP or XCI holds content of the game: its update (base | 0x800) or one of its DLC
// (base + 0x1000 + n) have the game's base title ID.
bool HoldsContentOf(const FileSys::VirtualFile& file, uint64_t base) {
    std::string name = file->GetName();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::shared_ptr<FileSys::NSP> package;
    if (name.ends_with(".nsp")) {
        package = std::make_shared<FileSys::NSP>(file);
    } else if (name.ends_with(".xci")) {
        FileSys::XCI image(file);
        if (image.GetStatus() == Loader::ResultStatus::Success) package = image.GetSecurePartitionNSP();
    }
    if (!package || package->GetStatus() != Loader::ResultStatus::Success) return false;
    for (const auto& [title_id, contents] : package->GetNCAs())
        if (FileSys::GetBaseTitleID(title_id) == base) return true;
    return false;
}

int FindAddOnFiles(const FileSys::VirtualDir& directory, uint64_t base, eden_found_file found, void* user, int depth) {
    if (!directory || depth > 8) return 0;
    int count = 0;
    for (const auto& file : directory->GetFiles())
        if (HoldsContentOf(file, base)) {
            found(user, file->GetFullPath().c_str());
            ++count;
        }
    for (const auto& folder : directory->GetSubdirectories()) count += FindAddOnFiles(folder, base, found, user, depth + 1);
    return count;
}
} // namespace

int eden_game_addon_files(uint64_t title_id, const char* updates_dir, const char* keys_dir, eden_found_file found,
                          void* user) {
    if (!title_id || !updates_dir || !keys_dir || !found) return 0;
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        return FindAddOnFiles(vfs.OpenDirectory(updates_dir, FileSys::OpenMode::Read),
                              FileSys::GetBaseTitleID(title_id), found, user, 0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ProsperoEden] updates: %s\n", error.what());
        return 0;
    }
}

int eden_game_language(const char* rom_path, const char* keys_dir, uint64_t title_id, int chosen) {
    const auto& codes = Service::Set::available_language_codes;
    if (chosen < 0 || chosen >= static_cast<int>(codes.size())) return chosen;
    uint32_t supported = 0;
    const auto& scanned = ScannedAddOns();
    if (const auto found = scanned.find(FileSys::GetBaseTitleID(title_id));
        title_id && found != scanned.end() && !found->second.update.empty())
        supported = found->second.languages;
    if (!supported) supported = eden_game_supported_languages(rom_path, keys_dir);
    namespace NS = Service::NS;
    const auto application = NS::ConvertToApplicationLanguage(codes[static_cast<std::size_t>(chosen)]);
    const auto* priorities = application ? NS::GetApplicationLanguagePriorityList(*application) : nullptr;
    if (!supported || !priorities) return chosen;
    // The console's order for a language goes on to its neighbours before English: Brazilian
    // Portuguese to Portuguese, then to Latin American Spanish. A player who chose Portuguese
    // does not expect Spanish. So: the chosen language and its own variants in the console's
    // order, then English, and only then the rest of that order. A language code starts with
    // its two letters ("pt-BR", "pt"), which tells the variants of one language.
    const auto letters = [](Service::Set::LanguageCode code) { return static_cast<u64>(code) & 0xffff; };
    const u64 own = letters(codes[static_cast<std::size_t>(chosen)]);
    const u64 english = u64{'e'} | u64{'n'} << 8;
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto language : *priorities) {
            if ((supported & NS::GetSupportedLanguageFlag(language)) == 0) continue;
            const auto match = NS::ConvertToLanguageCode(language);
            if (!match) continue;
            if ((pass == 0 && letters(*match) != own) || (pass == 1 && letters(*match) != english)) continue;
            for (std::size_t i = 0; i < codes.size(); ++i)
                if (codes[i] == *match) return static_cast<int>(i);
        }
    }
    return chosen;
}

int eden_game_addons(uint64_t title_id, char* update_version, size_t capacity, unsigned* dlc_count) {
    if (update_version && capacity) update_version[0] = '\0';
    if (dlc_count) *dlc_count = 0;
    const auto& scanned = ScannedAddOns();
    const auto found = scanned.find(FileSys::GetBaseTitleID(title_id));
    if (!title_id || found == scanned.end()) return 0;
    if (update_version && capacity) std::snprintf(update_version, capacity, "%s", found->second.update.c_str());
    if (dlc_count) *dlc_count = found->second.dlc;
    return !found->second.update.empty() || found->second.dlc != 0;
}

namespace {
// The save folder name of the profile games use (profiles.h), as Eden's save paths spell it.
// Without a profile file (no game has run yet) the one existing user save folder is used.
std::string EdenUserFolder(const std::filesystem::path& saves, std::string& error) {
    // The chosen profile's (profiles.h): a save goes in and comes out for whoever is playing.
    if (const auto profiles = Eden::Profiles::Read(); !profiles.empty())
        return profiles[static_cast<std::size_t>(Eden::Profiles::CurrentIndex())].Key();
    std::error_code list_error;
    std::vector<std::string> users;
    for (const auto& entry : Eden::RyujinxSaves::ListFolder(saves, list_error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() == 32 && name != std::string(32, '0')) users.push_back(name);
    }
    if (users.size() == 1) return users.front();
    error = "start any game once first, so ProsperoEden creates its user";
    return {};
}
} // namespace

namespace {
struct SaveSource {
    int kind = EDEN_SAVE_NONE;
    std::vector<Eden::RyujinxSaves::Save> saves;
};
// A save folder copied by hand comes before a Ryujinx data folder: it names the game itself.
SaveSource FindSaveSource(uint64_t title_id) {
    std::string error;
    SaveSource source;
    source.saves = Eden::RyujinxSaves::FindFolderSaves(Eden::AssetsPath("save-import"), title_id, error);
    if (!source.saves.empty()) {
        source.kind = EDEN_SAVE_FOLDER;
        return source;
    }
    source.saves = Eden::RyujinxSaves::FindSaves(Eden::AssetsPath("ryujinx"), title_id, error);
    if (!source.saves.empty()) source.kind = EDEN_SAVE_RYUJINX;
    return source;
}
std::string TitleName(uint64_t title_id) {
    char title[17];
    std::snprintf(title, sizeof(title), "%016llX", static_cast<unsigned long long>(title_id));
    return title;
}
std::string TimeStamp() {
    char stamp[32] = "now";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
    return stamp;
}
void PutPath(char* path, size_t capacity, const std::string& value) {
    if (path && capacity) std::snprintf(path, capacity, "%s", value.c_str());
}
} // namespace

int eden_save_import_source(uint64_t title_id) {
    return title_id ? FindSaveSource(title_id).kind : EDEN_SAVE_NONE;
}

int eden_save_import(uint64_t title_id, char* path, size_t capacity) {
    namespace fs = std::filesystem;
    PutPath(path, capacity, "");
    const SaveSource source = title_id ? FindSaveSource(title_id) : SaveSource{};
    if (source.saves.empty()) return EDEN_SAVE_NOTHING;
    std::string error;
    const fs::path root = fs::path{Eden::UserDir()} / "nand" / "user" / "save" / "0000000000000000";
    const std::string user = EdenUserFolder(root, error);
    if (user.empty()) {
        Eden::Report("save import", error.c_str());
        return EDEN_SAVE_NO_USER;
    }
    const std::string title = TitleName(title_id);
    const fs::path backups = fs::path{Eden::kDataDir} / "backup" / "save-import";
    bool replaced = false;
    if (!Eden::RyujinxSaves::Import(source.saves, root / user / title, root / std::string(32, '0') / title, backups,
                                    title + "-" + TimeStamp(), error, &replaced)) {
        Eden::Report("save import", error.c_str());
        return EDEN_SAVE_FAILED;
    }
    if (replaced) PutPath(path, capacity, backups.string());
    Eden::Report("save import", (title + (source.kind == EDEN_SAVE_FOLDER ? " from save-import" : " from ryujinx")).c_str());
    return EDEN_SAVE_DONE;
}

int eden_save_export(uint64_t title_id, char* path, size_t capacity) {
    namespace fs = std::filesystem;
    PutPath(path, capacity, "");
    if (!title_id) return EDEN_SAVE_NOTHING;
    std::string error;
    const fs::path root = fs::path{Eden::UserDir()} / "nand" / "user" / "save" / "0000000000000000";
    const std::string user = EdenUserFolder(root, error);
    if (user.empty()) return EDEN_SAVE_NOTHING;
    const std::string title = TitleName(title_id);
    const fs::path target = fs::path{Eden::AssetsPath("save-export")} / (title + "-" + TimeStamp());
    if (!Eden::RyujinxSaves::Export(root / user / title, root / std::string(32, '0') / title, target, error)) {
        Eden::Report("save export", error.c_str());
        return error == "no save yet" ? EDEN_SAVE_NOTHING : EDEN_SAVE_FAILED;
    }
    PutPath(path, capacity, target.string());
    Eden::Report("save export", target.string().c_str());
    return EDEN_SAVE_DONE;
}

const char* eden_startup_error() {
    static const std::string error = []() -> std::string {
        try {
            const std::string keys = Eden::AssetsPath("keys");
            const std::string firmware = Eden::AssetsPath("firmware");
            Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys);
            FileSys::RealVfsFilesystem vfs;
            const auto prod = vfs.OpenFile(std::string{keys} + "/prod.keys", FileSys::OpenMode::Read);
            if (!prod || !prod->GetSize())
                return "Missing or empty keys/prod.keys in " + Eden::AssetsDir() + ".";
            auto& manager = Core::Crypto::KeyManager::Instance();
            if (!manager.HasKey(Core::Crypto::S256KeyType::Header))
                return "prod.keys could not supply an NCA header key. Replace it with a valid key dump.";
            std::error_code ec;
#if defined(__PROSPERO__)
            const auto entries = Eden::ReadNativeDirectory(firmware, ec);
#else
            std::vector<std::filesystem::directory_entry> entries;
            for (std::filesystem::directory_iterator it{firmware, ec}, end; !ec && it != end; it.increment(ec))
                entries.push_back(*it);
#endif
            if (ec) return "Cannot read firmware/ in " + Eden::AssetsDir() + ". Install extracted firmware NCAs.";
            unsigned count = 0;
            bool version = false;
            for (const auto& entry : entries) {
                if (entry.path().extension() != ".nca") continue;
                ++count;
                const auto file = vfs.OpenFile(entry.path().string(), FileSys::OpenMode::Read);
                if (!file) return "A firmware NCA cannot be read. Reinstall the firmware dump.";
                FileSys::NCA nca(file);
                if (nca.GetStatus() != Loader::ResultStatus::Success)
                    return "Firmware NCA validation failed (code " +
                        std::to_string(static_cast<unsigned>(nca.GetStatus())) +
                        "). Check that firmware and prod.keys are compatible.";
                if (nca.GetTitleId() == 0x0100000000000809ULL && nca.GetRomFS()) {
                    const auto romfs = FileSys::ExtractRomFS(nca.GetRomFS());
                    const auto file = romfs ? romfs->GetFile("file") : FileSys::VirtualFile{};
                    version |= file && file->GetSize() >= 0x100 && file->ReadBytes(0x100).size() == 0x100;
                }
            }
            if (!count) return "No firmware NCAs found in " + Eden::AssetsPath("firmware") + ".";
            if (!version) return "Firmware SystemVersion data is missing or unreadable. Install a complete firmware dump.";
            return {};
        } catch (...) {
            return "Setup validation failed. Check that firmware and key files are readable and valid.";
        }
    }();
    return error.c_str();
}
