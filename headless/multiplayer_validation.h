// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include "network/packet.h"
#include "network/room_member.h"

namespace Eden::Multiplayer {
// Enough for a compressed UDP datagram or the metadata of 254 room members;
// limits ENet's allocation before a remote fragmented message is assembled.
inline constexpr std::size_t MaxRoomPacketBytes = 1024 * 1024;

// Validate before upstream handlers publish partially parsed state or allocate
// a member list from a remote count. Keep the upstream serialization unchanged.
inline bool ValidRoomPacket(std::span<const u8> bytes) {
    if (bytes.empty() || bytes.size() > MaxRoomPacketBytes) return false;
    Network::Packet packet;
    packet.Append(bytes.data(), bytes.size());
    u8 type{};
    packet.Read(type);
    const auto strings = [&](unsigned count) {
        std::string value;
        for (unsigned i = 0; i < count && packet; ++i) packet.Read(value);
    };
    using namespace Network;
    switch (type) {
    case IdJoinSuccess:
    case IdJoinSuccessAsMod: {
        IPv4Address ip{};
        packet.Read(ip);
        break;
    }
    case IdRoomInformation: {
        strings(2); // name, description
        u32 slots{}, members{};
        u16 port{};
        packet.Read(slots).Read(port);
        strings(2); // preferred game, host username
        packet.Read(members);
        if (!packet || slots > MaxConcurrentConnections || members > slots) return false;
        for (u32 i = 0; i < members && packet; ++i) {
            strings(1);
            IPv4Address ip{};
            packet.Read(ip);
            strings(1);
            u64 game{};
            packet.Read(game);
            strings(4); // version, username, display name, avatar
        }
        break;
    }
    case IdProxyPacket: {
        u8 family{}, protocol{}, broadcast{};
        IPv4Address ip{};
        u16 port{};
        for (unsigned i = 0; i < 2; ++i) {
            packet.Read(family).Read(ip).Read(port);
            if (!packet || family > static_cast<u8>(Domain::INET)) return false;
        }
        packet.Read(protocol).Read(broadcast);
        if (protocol > static_cast<u8>(Protocol::PFSYNC) || broadcast > 1) return false;
        std::vector<u8> payload;
        packet.Read(payload);
        break;
    }
    case IdLdnPacket: {
        u8 kind{}, broadcast{};
        IPv4Address ip{};
        packet.Read(kind).Read(ip).Read(ip).Read(broadcast);
        if (kind > static_cast<u8>(LDNPacketType::DestroyNetwork) || broadcast > 1) return false;
        std::vector<u8> payload;
        packet.Read(payload);
        // The guest receiver validates the structure's exact size and fields.
        break;
    }
    case IdChatMessage:
        strings(3);
        break;
    case IdStatusMessage: {
        u8 status{};
        packet.Read(status);
        if (status < IdMemberJoin || status > IdAddressUnbanned) return false;
        strings(2);
        break;
    }
    case IdModBanListResponse: {
        std::vector<std::string> entries;
        packet.Read(entries).Read(entries);
        break;
    }
    case IdVersionMismatch: {
        u32 version{};
        packet.Read(version);
        break;
    }
    case IdNameCollision:
    case IdIpCollision:
    case IdWrongPassword:
    case IdCloseRoom:
    case IdRoomIsFull:
    case IdHostKicked:
    case IdHostBanned:
    case IdModPermissionDenied:
    case IdModNoSuchUser:
        break;
    default:
        return false;
    }
    return packet && packet.EndOfPacket();
}
} // namespace Eden::Multiplayer
