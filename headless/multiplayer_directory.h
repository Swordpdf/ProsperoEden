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
    bool featured{};  // one of the project's own rooms (FeaturedRoom)
};
struct RoomDirectory {
    std::vector<PublicRoom> rooms;
    std::string error;
    unsigned rejected{};
};
inline constexpr std::size_t MaxDirectoryBytes = 2 * 1024 * 1024;
// The project's own rooms come first in Public rooms, in the order of their ports. They are
// known from the public list itself: a name that starts with the project's and one of its ports
// (a copied name on another port is an ordinary room).
inline constexpr std::string_view FeaturedRoomName = "BlackBearReloaded ";
inline constexpr std::uint16_t FeaturedRoomFirstPort = 24001, FeaturedRoomLastPort = 24005;
inline bool FeaturedRoom(std::string_view name, std::uint16_t port) {
    return name.starts_with(FeaturedRoomName) && port >= FeaturedRoomFirstPort && port <= FeaturedRoomLastPort;
}
RoomDirectory ParseRoomDirectory(std::string_view body, unsigned protocol);
RoomDirectory FetchRoomDirectory(unsigned protocol, const std::atomic<bool>& stop);
}
