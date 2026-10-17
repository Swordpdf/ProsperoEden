// SPDX-License-Identifier: GPL-3.0-or-later
// Cheats chosen one by one. Eden runs every cheat of every mod that is on; a mod that lists
// several (mods.h) has each of them chosen in the launcher instead (Library > Game settings >
// Cheats), and the ones not chosen are named here before the game starts. The derived
// patch_manager.cpp (headless/CMakeLists.txt) passes each mod's cheat list through Append(),
// which leaves those in the list but switched off: Eden's cheat machine skips them.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Eden::Cheats {
// A cheat as both sides name it: its mod, then its own name without the blanks around it. Eden
// keeps the first 63 bytes of a name, so no more than that is compared.
inline std::string Key(std::string_view mod, std::string_view cheat) {
    while (!cheat.empty() && std::isspace(static_cast<unsigned char>(cheat.front()))) cheat.remove_prefix(1);
    while (!cheat.empty() && std::isspace(static_cast<unsigned char>(cheat.back()))) cheat.remove_suffix(1);
    cheat = cheat.substr(0, 60);
    while (!cheat.empty() && std::isspace(static_cast<unsigned char>(cheat.back()))) cheat.remove_suffix(1);
    return std::string(mod) + "#" + std::string(cheat);
}

// Every cheat of a mod at once: the game's Cheats switch off (mods.h) leaves a mod that also
// patches the code or replaces files running without them.
inline std::string AllOf(std::string_view mod) {
    return std::string(mod) + "#*";
}

// The cheats that stay off in the game about to start (main.cpp fills it before Eden boots).
inline std::vector<std::string>& Off() {
    static std::vector<std::string> off;
    return off;
}

inline bool Runs(std::string_view mod, std::string_view cheat) {
    const auto& off = Off();
    return std::find(off.begin(), off.end(), Key(mod, cheat)) == off.end() &&
           std::find(off.begin(), off.end(), AllOf(mod)) == off.end();
}

// A mod's cheat list as Eden parsed it, added to the game's: the first entry is the file's
// master code, which always stays.
template <typename Entries>
void Append(Entries& out, const Entries& list, std::string_view mod) {
    for (auto entry : list) {
        if (entry.cheat_id != 0 && entry.enabled) entry.enabled = Runs(mod, entry.definition.readable_name.data());
        out.push_back(entry);
    }
}
} // namespace Eden::Cheats
