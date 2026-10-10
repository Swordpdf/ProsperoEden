// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>
#include "core/hle/service/ldn/ldn_types.h"

namespace Eden::Multiplayer {
inline bool ValidLdnPacket(const Network::LDNPacket& packet) {
    using namespace Service::LDN;
    using Network::LDNPacketType;
    switch (packet.type) {
    case LDNPacketType::Scan:
    case LDNPacketType::DestroyNetwork:
        return packet.data.empty();
    case LDNPacketType::ScanResp:
    case LDNPacketType::SyncNetwork: {
        if (packet.data.size() != sizeof(NetworkInfo)) return false;
        NetworkInfo info{};
        std::memcpy(&info, packet.data.data(), sizeof(info));
        if (info.ldn.node_count_max > NodeCountMax ||
            info.ldn.node_count > info.ldn.node_count_max ||
            info.ldn.advertise_data_size > AdvertiseDataSizeMax) return false;
        return true;
    }
    case LDNPacketType::Connect:
    case LDNPacketType::Disconnect:
        return packet.data.size() == sizeof(NodeInfo);
    default:
        return false;
    }
}
} // namespace Eden::Multiplayer
