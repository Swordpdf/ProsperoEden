// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Eden::Multiplayer {
struct PublicRoom {
    std::string name, game, address;
    std::uint16_t port{};
    unsigned players{}, capacity{};
    bool password{}, compatible{};
};
struct RoomDirectory {
    std::vector<PublicRoom> rooms;
    std::string error;
    unsigned rejected{};
};
inline constexpr std::size_t MaxDirectoryBytes = 2 * 1024 * 1024;
RoomDirectory ParseRoomDirectory(std::string_view body, unsigned protocol);
RoomDirectory FetchRoomDirectory(unsigned protocol, const std::atomic<bool>& stop);
}
