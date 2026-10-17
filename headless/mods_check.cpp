// SPDX-License-Identifier: GPL-3.0-or-later
// Host check for mods: Eden's patch code as this build derives it (.ips and .pchtxt patches, which
// the pinned source could not apply: headless/CMakeLists.txt), what the launcher lists from a
// game's mods folder (mods.h), the names it switches off (settings_store.h), and the cheats chosen
// one by one (cheats.h) as Eden's own parser reads them.
#include "mods.h"
#include "settings_store.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "core/file_sys/ips_layer.h"
#include "core/memory/cheat_engine.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
namespace fs = std::filesystem;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "Mods FAIL: %s\n", what);
        std::exit(1);
    }
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

FileSys::VirtualFile file(std::vector<u8> bytes, const char* name) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), name);
}

// A stand-in for a game's code: 0x800 bytes that count up.
std::vector<u8> code() {
    std::vector<u8> bytes(0x800);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i * 7 + 3);
    return bytes;
}

void append(std::vector<u8>& out, std::initializer_list<int> bytes) {
    for (const int byte : bytes) out.push_back(static_cast<u8>(byte));
}

void check_ips() {
    const std::vector<u8> original = code();
    // IPS: a record of three bytes at 0x10, then a run of eight 0xAA at 0x100.
    std::vector<u8> ips{'P', 'A', 'T', 'C', 'H'};
    append(ips, {0x00, 0x00, 0x10, 0x00, 0x03, 0x11, 0x22, 0x33});
    append(ips, {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0xAA});
    append(ips, {'E', 'O', 'F'});
    auto patched = FileSys::PatchIPS(file(original, "main"), file(ips, "patch.ips"));
    require(patched != nullptr, "an IPS patch applies");
    auto bytes = patched->ReadAllBytes();
    require(bytes.size() == original.size() && bytes[0x10] == 0x11 && bytes[0x11] == 0x22 && bytes[0x12] == 0x33,
            "IPS record bytes");
    for (std::size_t i = 0x100; i < 0x108; ++i) require(bytes[i] == 0xAA, "IPS run bytes");
    require(bytes[0x0F] == original[0x0F] && bytes[0x13] == original[0x13] && bytes[0x108] == original[0x108],
            "IPS leaves the rest alone");
    // IPS32: offsets of four bytes, end marker "EEOF".
    std::vector<u8> ips32{'I', 'P', 'S', '3', '2'};
    append(ips32, {0x00, 0x00, 0x07, 0x00, 0x00, 0x02, 0xC0, 0xDE});
    append(ips32, {'E', 'E', 'O', 'F'});
    patched = FileSys::PatchIPS(file(original, "main"), file(ips32, "patch.ips"));
    require(patched != nullptr, "an IPS32 patch applies");
    bytes = patched->ReadAllBytes();
    require(bytes[0x700] == 0xC0 && bytes[0x701] == 0xDE && bytes[0x702] == original[0x702], "IPS32 record bytes");
    // Not a patch, a patch without its end, and a patch past the end of the code: nothing applies.
    require(FileSys::PatchIPS(file(original, "main"), file(original, "not.ips")) == nullptr, "a file that is no patch");
    std::vector<u8> cut(ips.begin(), ips.end() - 3);
    require(FileSys::PatchIPS(file(original, "main"), file(cut, "cut.ips")) == nullptr, "a patch without its end");
    std::vector<u8> beyond{'P', 'A', 'T', 'C', 'H'};
    append(beyond, {0x00, 0x09, 0x00, 0x00, 0x01, 0x55, 'E', 'O', 'F'});
    require(FileSys::PatchIPS(file(original, "main"), file(beyond, "beyond.ips")) == nullptr,
            "a patch past the end of the code");
}

void check_pchtxt() {
    // Offsets count from the start of the NSO with the shift a patch file may name; a patch's
    // values are the bytes as written, a quoted value is text.
    std::string text =
        "@nsobid-0123456789ABCDEF0123456789abcdef01234567\n"
        "# 60 FPS\n"
        "@flag offset_shift 0x100\n"
        "@enabled\n"
        "00000010 DEADBEEF\n"
        "00000020 \"Hi\\n!\"\n"
        "00000040 01 // one byte\n"
        "@disabled\n"
        "00000030 FFFFFFFF\n"
        "@enabled\n"
        // What patch files carry besides records: their authors' names and links. None of it is a
        // record or a flag (read as such, it wrote over the start of the code and the build ID).
        "@somebody\r\n"
        "@nobody-here\r\n"
        "@blender\r\n"
        "https://example.invalid/somebody\r\n"
        "Special thanks to the team!\r\n"
        "Contributors (thank you)\r\n"
        "Somebody#1234, Another One#5678\r\n";
    // A value longer than the pinned code's 248-byte record.
    std::string wide = "00000200 ";
    for (int i = 0; i < 300; ++i) wide += "5A";
    text += wide + "\n@stop\n00000050 EE\n";
    const FileSys::IPSwitchCompiler compiler{
        file(std::vector<u8>(text.begin(), text.end()), "60fps.pchtxt")};
    const auto id = compiler.GetBuildID();
    static constexpr u8 kId[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67,
                                 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67};
    for (std::size_t i = 0; i < id.size(); ++i) require(id[i] == (i < sizeof(kId) ? kId[i] : 0), "the build ID");
    const std::vector<u8> original = code();
    const auto patched = compiler.Apply(file(original, "main"));
    require(patched != nullptr, "a .pchtxt patch applies");
    const auto bytes = patched->ReadAllBytes();
    require(bytes.size() == original.size(), "the code keeps its size");
    require(bytes[0x110] == 0xDE && bytes[0x111] == 0xAD && bytes[0x112] == 0xBE && bytes[0x113] == 0xEF,
            "hex values, in the order written");
    require(bytes[0x120] == 'H' && bytes[0x121] == 'i' && bytes[0x122] == '\n' && bytes[0x123] == '!' &&
            bytes[0x124] == original[0x124], "text values");
    require(bytes[0x140] == 0x01 && bytes[0x141] == original[0x141], "a value before a comment");
    require(bytes[0x130] == original[0x130], "a disabled patch is skipped");
    for (std::size_t i = 0x100; i < 0x110; ++i) require(bytes[i] == original[i], "credits are not records");
    for (std::size_t i = 0x300; i < 0x300 + 300; ++i) require(bytes[i] == 0x5A, "a long value");
    require(bytes[0x300 + 300] == original[0x300 + 300], "a long value ends where it should");
    require(bytes[0x150] == original[0x150], "nothing after @stop");
}

void check_listing(const fs::path& base) {
    using namespace Eden::Mods;
    const std::uint64_t game = 0x0100ABCD00001000ULL, other = 0x0100FFFF00002000ULL;
    const std::string root = (base / "mods").string();
    require(List(root, game).empty() && TitleFolder(root, game).empty(), "no mods folder");
    require(TitleFolderToCreate(root, game) == root + "/0100ABCD00001000", "the folder to make");
    // The title's folder in lower case, as a file manager may have it.
    const fs::path title = base / "mods" / "0100abcd00001000";
    write_text(title / "60 FPS" / "exefs" / "0123456789ABCDEF.pchtxt", "@enabled\n");
    write_text(title / "Texture pack" / "romfs" / "model" / "hero.bin", "x");
    write_text(title / "Everything" / "ExeFS" / "main", "x");
    write_text(title / "Everything" / "romfs_ext" / "a.stub", "");
    write_text(title / "Everything" / "cheats" / "0123456789ABCDEF.txt", "[cheat]\n");
    write_text(title / "cheat_infinite.txt", "[cheat]\n");
    write_text(title / "notes.txt", "not a mod");
    fs::create_directories(title / "Empty" / "exefs");
    write_text(title / "Readme only" / "readme.txt", "nothing Eden uses");
    write_text(title / "Wrong exefs" / "exefs" / "notes.txt", "nothing Eden uses");
    require(TitleFolder(root, game) == title.string(), "the title's folder in either case");
    const auto mods = List(root, game);
    require(mods.size() == 4, "four mods");
    require(mods[0].name == "60 FPS" && mods[0].kinds == kCode, "a patch");
    require(mods[1].name == "Everything" && mods[1].kinds == (kCode | kFiles | kCheats), "all three kinds");
    require(mods[2].name == "Texture pack" && mods[2].kinds == kFiles, "replacement files");
    require(mods[3].name == "cheat_infinite.txt" && mods[3].kinds == kCheats, "a loose cheat file");
    require(List(root, other).empty() && List(root, 0).empty(), "another game has none");
    require(Summary(mods, {}) == "60 FPS, Everything, Texture pack, cheat_infinite.txt", "all in use");
    require(Summary(mods, {"Everything", "Gone"}) == "60 FPS, Texture pack, cheat_infinite.txt (1 switched off)",
            "one switched off");
    require(Summary({}, {}) == "none", "no mods");
}

// A game in its own folder in roms/ (issue 168): its mods/ and cheats/, with mods/ and cheats/ of
// the game files folder.
void check_game_folder(const fs::path& base) {
    using namespace Eden::Mods;
    const std::uint64_t game = 0x0100DDDD00004000ULL;
    const fs::path folder = base / "roms" / "Sample";
    write_text(folder / "mods" / "Texture pack" / "romfs" / "a.bin", "x");
    write_text(folder / "mods" / "Shared name" / "exefs" / "0123456789ABCDEF.ips", "x");
    write_text(folder / "cheats" / "0123456789ABCDEF.txt",
               "[One]\n04000000 00000001 00000001\n[Two]\n04000000 00000002 00000002\n");
    write_text(base / "mods" / "0100DDDD00004000" / "Shared name" / "romfs" / "b.bin", "x");
    write_text(base / "mods" / "0100DDDD00004000" / "Speed" / "exefs" / "0123456789ABCDEF.pchtxt", "@enabled\n");
    write_text(base / "cheats" / "0100dddd00004000" / "0123456789ABCDEF.txt", "[Hidden]\n04000000 00000003 00000003\n");
    write_text(base / "cheats" / "0100dddd00004000" / "FEDCBA9876543210.txt",
               "[Other version]\n04000000 00000004 00000004\n[Second]\n04000000 00000005 00000005\n");
    const Places places{(base / "mods").string(), (base / "cheats").string(), folder.string()};
    const auto mods = List(places, game);
    require(mods.size() == 4, "game folder: four mods");
    require(mods[0].name == std::string(kCheatsFolder) && mods[0].kinds == kCheats, "game folder: the cheats folders");
    require(mods[1].name == "Shared name" && mods[1].kinds == kCode, "game folder: its mod of a shared name first");
    require(mods[2].name == "Speed" && mods[3].name == "Texture pack", "game folder: with the shared mods");
    const auto files = CheatFolderFiles(places, game);
    require(files.size() == 2 && files[0] == folder / "cheats" / "0123456789ABCDEF.txt" &&
                files[1].filename() == "FEDCBA9876543210.txt",
            "game folder: its cheat file for a version first, cheats/ for the others");
    require(mods[0].cheats.size() == 4, "game folder: the cheats of both files");
    require(List(Places{(base / "mods").string(), (base / "cheats").string(), {}}, game).size() == 3,
            "a game directly in roms/: mods/ and cheats/ only");
}

void check_cheats(const fs::path& base) {
    using namespace Eden::Mods;
    using Ids = std::vector<std::string>;
    const std::uint64_t game = 0x0100CCCC00003000ULL;
    const std::string root = (base / "mods").string();
    const fs::path title = base / "mods" / "0100CCCC00003000";
    // A collection: a master code, a section title without code, two frame rates, one more.
    const std::string text =
        "{Master}\n580F0000 01234567\n\n[--Frame rate--]\n"
        "[60 FPS]\n04000000 00ABCDEF 52800020\n[30 FPS]\n04000000 00ABCDEF 52800040\n"
        "[ Moon jump ]\r\n04000000 00111111 00000384\r\n";
    write_text(title / "Collection" / "Cheats" / "1111222233334444.txt", text);
    // Another build's file: a cheat the first has, and one of its own.
    write_text(title / "Collection" / "Cheats" / "5555666677778888.txt",
               "[60 FPS]\n04000000 00ABCDEF 52800020\n[Extra]\n04000000 00222222 00000001\n");
    write_text(title / "One cheat" / "cheats" / "1111222233334444.txt", "[Infinite]\n04000000 00222222 00000001\n");
    write_text(title / "cheat_loose.txt", text);
    const auto mods = List(root, game);
    require(mods.size() == 3 && mods[0].name == "Collection" && mods[1].name == "One cheat" &&
            mods[2].name == "cheat_loose.txt", "three mods with cheats");
    require(mods[0].cheats.size() == 4 && mods[0].cheats[0].name == "60 FPS" && mods[0].cheats[1].name == "30 FPS" &&
            mods[0].cheats[2].name == "Moon jump" && mods[0].cheats[3].name == "Extra" &&
            mods[0].cheats[0].id == "Collection#60 FPS", "a collection's cheats, once per name");
    require(mods[1].cheats.empty() && mods[1].cheat == "Infinite", "a single cheat runs with its mod");
    require(mods[2].cheats.size() == 3 && mods[2].cheats[1].id == "cheat_loose.txt#30 FPS", "a loose file's cheats");

    // Choosing: one frame rate takes the other's place, the rest combine, per mod.
    Ids chosen = ChooseCheat(mods[0], "60 FPS", true, {});
    chosen = ChooseCheat(mods[0], "Moon jump", true, chosen);
    chosen = ChooseCheat(mods[0], "30 FPS", true, chosen);
    require(chosen == Ids({"Collection#30 FPS", "Collection#Moon jump"}), "one of a group");
    require(ChooseCheat(mods[0], "30 FPS", true, chosen) == chosen && ChooseCheat(mods[0], "Nothing", true, chosen) == chosen,
            "chosen twice, or not there");
    chosen = ChooseCheat(mods[2], "60 FPS", true, chosen);
    require(chosen.size() == 3, "another mod's frame rate stays");
    require(ChooseCheat(mods[0], "Moon jump", false, chosen) == Ids({"Collection#30 FPS", "cheat_loose.txt#60 FPS"}),
            "switched off");
    require(Summary(mods, {}, chosen) == "Collection (2 of 4 cheats), One cheat, cheat_loose.txt (1 of 3 cheats)",
            "the chosen cheats in the log");
    const Ids off = CheatsOff(mods, chosen);
    require(off == Ids({"Collection#60 FPS", "Collection#Extra", "cheat_loose.txt#30 FPS", "cheat_loose.txt#Moon jump"}),
            "the cheats that stay off");

    // Eden's list of the collection: master, title, 60 FPS, 30 FPS, Moon jump.
    const Core::Memory::TextCheatParser parser;
    const auto parsed = parser.Parse(text);
    require(parsed.size() == 5 && parsed[0].enabled && !parsed[1].enabled && parsed[2].enabled, "Eden's parser");
    Eden::Cheats::Off() = off;
    std::vector<Core::Memory::CheatEntry> list;
    Eden::Cheats::Append(list, parsed, "Collection");
    require(list.size() == 5 && list[0].enabled && !list[1].enabled && !list[2].enabled && list[3].enabled && list[4].enabled,
            "only the chosen cheats run, with the master code");
    Eden::Cheats::Append(list, parsed, "One cheat");
    require(list.size() == 10 && list[7].enabled && list[8].enabled && list[9].enabled, "another mod's cheats all run");
    Eden::Cheats::Off().clear();
    list.clear();
    Eden::Cheats::Append(list, parsed, "Collection");
    require(list[2].enabled && list[3].enabled && list[4].enabled && !list[1].enabled, "nothing off: Eden's list as it is");
    // A name longer than Eden keeps is the same cheat on both sides.
    const std::string name(70, 'n');
    const auto cut = parser.Parse("[" + name + "]\n04000000 00333333 00000001\n");
    require(cut.size() == 2 && std::string(cut[1].definition.readable_name.data()).size() == 63 &&
            Eden::Cheats::Key("m", cut[1].definition.readable_name.data()) == Eden::Cheats::Key("m", name),
            "a long name");
}

// The launcher's Cheats list apart from its Mods list (issue 132): every cheat of the game,
// however its file came, and what the game's Mods and Cheats switches tell Eden.
void check_cheats_list(const fs::path& base) {
    using namespace Eden::Mods;
    using Ids = std::vector<std::string>;
    using Switch = CheatItem::Switch;
    const std::uint64_t game = 0x0100EEEE00005000ULL;
    const fs::path folder = base / "roms" / "Lists";
    const std::string code = "\n04000000 00000001 00000001\n";
    write_text(folder / "mods" / "Speed" / "exefs" / "0123456789ABCDEF.pchtxt", "@enabled\n");
    write_text(folder / "mods" / "Pack" / "romfs" / "a.bin", "x");
    write_text(folder / "mods" / "Pack" / "cheats" / "1111222233334444.txt", "[Bonus]" + code);
    write_text(folder / "mods" / "Collection" / "cheats" / "1111222233334444.txt",
               "[60 FPS]" + code + "[30 FPS]" + code + "[Moon jump]" + code);
    write_text(folder / "mods" / "Mixed collection" / "exefs" / "0123456789ABCDEF.ips", "x");
    write_text(folder / "mods" / "Mixed collection" / "cheats" / "1111222233334444.txt", "[A]" + code + "[B]" + code);
    write_text(folder / "mods" / "cheat_speed.txt", "[Run fast]" + code);
    write_text(folder / "cheats" / "1111222233334444.txt", "[Infinite lives]" + code);
    const auto mods = List(Places{(base / "nothing").string(), (base / "nothing").string(), folder.string()}, game);
    require(mods.size() == 6 && mods[0].name == std::string(kCheatsFolder) && mods[1].name == "Collection" &&
                mods[2].name == "Mixed collection" && mods[3].name == "Pack" && mods[4].name == "Speed" &&
                mods[5].name == "cheat_speed.txt",
            "cheats list: the game's mods");
    require(CheatsOnly(mods[0]) && CheatsOnly(mods[1]) && !CheatsOnly(mods[2]) && !CheatsOnly(mods[3]) &&
                !CheatsOnly(mods[4]) && CheatsOnly(mods[5]),
            "cheats list: the mods of cheats alone are not in the Mods list");
    require(mods[0].cheat == "Infinite lives" && mods[3].cheat == "Bonus" && mods[5].cheat == "Run fast" &&
                mods[2].cheats.size() == 2 && mods[1].cheats.size() == 3,
            "cheats list: one cheat named, several listed");

    // Every cheat, each switched where its kind keeps it; what is chosen today stays chosen.
    const Ids disabled{"cheat_speed.txt"}, chosen{"Collection#30 FPS", "Mixed collection#B"};
    const auto items = CheatItems(mods, disabled, chosen, {});
    require(items.size() == 8, "cheats list: eight cheats");
    const auto is = [&](std::size_t i, const char* mod, const char* name, Switch by, bool on) {
        return items[i].mod == mod && items[i].name == name && items[i].by == by && items[i].on == on;
    };
    require(is(0, "Cheats folder", "Infinite lives", Switch::mod, true) &&
                is(1, "Collection", "60 FPS", Switch::chosen, false) && is(2, "Collection", "30 FPS", Switch::chosen, true) &&
                is(3, "Collection", "Moon jump", Switch::chosen, false) &&
                is(4, "Mixed collection", "A", Switch::chosen, false) && is(5, "Mixed collection", "B", Switch::chosen, true) &&
                is(6, "Pack", "Bonus", Switch::own, true) && is(7, "cheat_speed.txt", "Run fast", Switch::mod, false),
            "cheats list: names, switches and their state");
    require(!CheatItems(mods, {}, {}, {Eden::Cheats::Key("Pack", "Bonus")})[6].on, "cheats list: a single cheat off on its own");

    // Both switches on: the mods switched off one by one, the cheats not chosen, a single cheat
    // switched off beside its mod's files.
    auto switched = Switches(mods, disabled, chosen, {}, true, true);
    require(switched.mods_off == disabled &&
                switched.cheats_off == Ids({"Collection#60 FPS", "Collection#Moon jump", "Mixed collection#A"}),
            "switches: both on");
    switched = Switches(mods, disabled, chosen, {"Pack#Bonus", "Gone#x"}, true, true);
    require(switched.cheats_off.size() == 4 && switched.cheats_off.back() == "Pack#Bonus", "switches: a single cheat off");
    // The Mods switch off: the mods with patches or files only; cheats follow their own switch.
    switched = Switches(mods, disabled, chosen, {}, false, true);
    require(switched.mods_off == Ids({"cheat_speed.txt", "Mixed collection", "Pack", "Speed"}) &&
                switched.cheats_off.size() == 3,
            "switches: Mods off");
    // The Cheats switch off: the mods of cheats alone, and every cheat of the others.
    switched = Switches(mods, disabled, chosen, {}, true, false);
    require(switched.mods_off == Ids({"cheat_speed.txt", "Cheats folder", "Collection"}) &&
                switched.cheats_off == Ids({Eden::Cheats::AllOf("Mixed collection"), Eden::Cheats::AllOf("Pack")}),
            "switches: Cheats off");
    Eden::Cheats::Off() = switched.cheats_off;
    require(!Eden::Cheats::Runs("Pack", "Bonus") && !Eden::Cheats::Runs("Mixed collection", "B") &&
                Eden::Cheats::Runs("Speed", "Bonus"),
            "switches: no cheat of a mod with patches or files runs");
    Eden::Cheats::Off().clear();
    switched = Switches(mods, {}, {}, {}, false, false);
    require(switched.mods_off.size() == 6, "switches: both off");
}

void check_settings(const fs::path& base) {
    const std::string settings = (base / "prosperoeden.json").string();
    const std::uint64_t game = 0x0100ABCD00001000ULL, other = 0x0100FFFF00002000ULL;
    require(Eden::LoadDisabledMods(game, settings).empty(), "nothing switched off at first");
    require(Eden::SaveGameSettings(game, {1, 4, 1}, settings), "game settings");
    require(Eden::SaveModEnabled(game, "60 FPS", false, settings) &&
            Eden::SaveModEnabled(game, "Texture pack", false, settings) &&
            Eden::SaveModEnabled(game, "60 FPS", false, settings), "switching off");
    auto off = Eden::LoadDisabledMods(game, settings);
    require(off == std::vector<std::string>({"Texture pack", "60 FPS"}), "the names switched off, once each");
    require(Eden::LoadDisabledMods(other, settings).empty(), "per game");
    require(Eden::SaveModEnabled(game, "Texture pack", true, settings), "switching on");
    off = Eden::LoadDisabledMods(game, settings);
    require(off == std::vector<std::string>({"60 FPS"}), "one left");
    const auto kept = Eden::LoadGameSettings(game, settings);
    require(kept.renderer == 1 && kept.resolution == 4 && kept.upscaling_filter == 1, "the other settings stay");
    require(Eden::SaveModEnabled(game, "60 FPS", true, settings) && Eden::LoadDisabledMods(game, settings).empty(),
            "all on again");
    require(!Eden::SaveModEnabled(0, "x", false, settings) && !Eden::SaveModEnabled(game, "", false, settings),
            "nothing to save");
    // The Library's Mods switch: on unless turned off, per game, and the mods' own switches and
    // the game's other settings stay as they are behind it.
    require(Eden::LoadModsEnabled(game, settings) && Eden::LoadModsEnabled(other, settings) &&
            Eden::LoadModsEnabled(0, settings), "the switch is on at first");
    require(Eden::SaveModEnabled(game, "60 FPS", false, settings) && Eden::SaveModsEnabled(game, false, settings),
            "the switch turned off");
    require(!Eden::LoadModsEnabled(game, settings) && Eden::LoadModsEnabled(other, settings), "off for that game only");
    require(Eden::LoadDisabledMods(game, settings) == std::vector<std::string>({"60 FPS"}), "a mod's own switch stays");
    require(Eden::LoadGameSettings(game, settings).resolution == 4, "the other settings stay with the switch off");
    require(Eden::SaveModsEnabled(game, true, settings) && Eden::LoadModsEnabled(game, settings), "the switch back on");
    std::ifstream written(settings);
    const std::string text{std::istreambuf_iterator<char>(written), {}};
    require(text.find("\"mods\"") == std::string::npos && text.find("\"mods_off\"") != std::string::npos,
            "on is not written; the mod switched off still is");
    require(!Eden::SaveModsEnabled(0, false, settings), "no game, nothing to save");
    // The chosen cheats, per game, beside the rest.
    require(Eden::LoadChosenCheats(game, settings).empty(), "no cheat chosen at first");
    require(Eden::SaveChosenCheats(game, {"Collection#60 FPS", "Collection#Moon jump"}, settings), "choosing cheats");
    require(Eden::LoadChosenCheats(game, settings) == std::vector<std::string>({"Collection#60 FPS", "Collection#Moon jump"}) &&
            Eden::LoadChosenCheats(other, settings).empty(), "the chosen cheats, per game");
    require(Eden::LoadDisabledMods(game, settings) == std::vector<std::string>({"60 FPS"}) &&
            Eden::LoadGameSettings(game, settings).resolution == 4, "the other settings stay with cheats chosen");
    require(Eden::SaveChosenCheats(game, {}, settings) && Eden::LoadChosenCheats(game, settings).empty() &&
            !Eden::SaveChosenCheats(0, {"x#y"}, settings), "none chosen again");
    // The game's Cheats switch, apart from its Mods switch: on unless turned off, per game.
    require(Eden::LoadCheatsEnabled(game, settings) && Eden::LoadCheatsEnabled(0, settings), "the Cheats switch is on at first");
    require(Eden::SaveChosenCheats(game, {"Collection#60 FPS"}, settings) && Eden::SaveCheatsEnabled(game, false, settings),
            "the Cheats switch turned off");
    require(!Eden::LoadCheatsEnabled(game, settings) && Eden::LoadCheatsEnabled(other, settings) &&
                Eden::LoadModsEnabled(game, settings),
            "the Cheats switch off for that game only, the Mods switch as it was");
    require(Eden::LoadChosenCheats(game, settings) == std::vector<std::string>({"Collection#60 FPS"}),
            "the chosen cheats stay behind the Cheats switch");
    require(Eden::SaveModsEnabled(game, false, settings) && !Eden::LoadCheatsEnabled(game, settings) &&
                Eden::SaveModsEnabled(game, true, settings),
            "the Mods switch leaves the Cheats switch alone");
    {
        std::ifstream in(settings);
        const std::string saved{std::istreambuf_iterator<char>(in), {}};
        require(saved.find("\"cheats\"") != std::string::npos, "the Cheats switch off is written");
    }
    require(Eden::SaveCheatsEnabled(game, true, settings) && Eden::LoadCheatsEnabled(game, settings), "the Cheats switch back on");
    {
        std::ifstream in(settings);
        const std::string saved{std::istreambuf_iterator<char>(in), {}};
        require(saved.find("\"cheats\"") == std::string::npos, "on is not written");
    }
    require(!Eden::SaveCheatsEnabled(0, false, settings), "no game, no Cheats switch to save");
    // The single cheats of mods with patches or files, switched off on their own.
    require(Eden::LoadCheatsOff(game, settings).empty(), "no single cheat off at first");
    require(Eden::SaveCheatOff(game, "Pack#Bonus", true, settings) && Eden::SaveCheatOff(game, "Pack#Bonus", true, settings) &&
                Eden::LoadCheatsOff(game, settings) == std::vector<std::string>({"Pack#Bonus"}) &&
                Eden::LoadCheatsOff(other, settings).empty(),
            "a single cheat off, once, per game");
    require(Eden::SaveCheatOff(game, "Pack#Bonus", false, settings) && Eden::LoadCheatsOff(game, settings).empty() &&
                !Eden::SaveCheatOff(0, "Pack#Bonus", true, settings) && !Eden::SaveCheatOff(game, "", true, settings),
            "a single cheat on again");
    require(Eden::SaveChosenCheats(game, {}, settings), "none chosen at the end");
}
} // namespace

int main() {
    char pattern[] = "/tmp/mods-check-XXXXXX";
    require(mkdtemp(pattern) != nullptr, "temporary folder");
    const fs::path base = pattern;
    check_ips();
    check_pchtxt();
    check_listing(base);
    check_cheats(base);
    check_game_folder(base);
    check_cheats_list(base);
    check_settings(base);
    fs::remove_all(base);
    std::puts("Mods: IPS and IPS32 patches, .pchtxt hex, text and long values, the folder listing, a game's own folder, "
              "switching off per game, the game's Mods and Cheats switches, cheats chosen one by one, the Cheats "
              "list PASS");
}
