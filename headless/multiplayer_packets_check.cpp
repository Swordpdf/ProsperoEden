// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "network/packet.h"
#include "multiplayer_validation.h"
#include "multiplayer_ldn.h"
#include "multiplayer_proxy.h"

int main() {
    using Network::Packet;
    const std::vector<u8> bytes{0, 1, 127, 255};
    const std::vector<std::string> strings{"", "PlayerOne", "PlayerTwo"};
    Packet valid;
    valid.Write(bytes).Write(strings);
    std::vector<u8> decoded;
    std::vector<std::string> names;
    valid.Read(decoded).Read(names);
    assert(valid && valid.EndOfPacket() && decoded == bytes && names == strings);
    const std::array<u8, 8> expected{0, 0, 0, 4, 0, 1, 127, 255};
    assert(std::memcmp(valid.GetData(), expected.data(), expected.size()) == 0);

    // Exercise every truncation, including a partial count and partial string.
    for (std::size_t size = 0; size < valid.GetDataSize(); ++size) {
        Packet truncated;
        truncated.Append(valid.GetData(), size);
        truncated.Read(decoded).Read(names);
        assert(!truncated);
    }
    Packet hostile_count;
    hostile_count.Write(std::numeric_limits<u32>::max());
    hostile_count.Read(decoded);
    assert(!hostile_count && decoded.empty());

    Packet partial_strings;
    partial_strings.Write(u32{2}).Write(std::string{"one"});
    partial_strings.Read(names);
    assert(!partial_strings && names.empty());

    // Overflowing requests and ignored bytes must invalidate the packet without copying.
    Packet overflow;
    overflow.Write(u8{42});
    overflow.IgnoreBytes(1);
    u8 untouched = 17;
    overflow.Read(&untouched, std::numeric_limits<std::size_t>::max());
    assert(!overflow && untouched == 17);
    Packet ignored;
    ignored.IgnoreBytes(1);
    assert(!ignored);
    ignored.Clear();
    ignored.Read(&untouched, 0);
    assert(ignored && ignored.EndOfPacket() && untouched == 17);

    Packet short_array;
    short_array.Write(u8{99});
    std::array<u8, 4> address{};
    short_array.Read(address);
    assert(!short_array && address[0] == 99 && address[1] == 0);

    const auto valid_room = [](const Packet& packet) {
        return Eden::Multiplayer::ValidRoomPacket(
            {static_cast<const u8*>(packet.GetData()), packet.GetDataSize()});
    };
    Packet joined;
    joined.Write(u8{Network::IdJoinSuccess}).Write(Network::IPv4Address{192, 168, 0, 1});
    assert(valid_room(joined));
    for (std::size_t size = 0; size < joined.GetDataSize(); ++size) {
        Packet truncated;
        truncated.Append(joined.GetData(), size);
        assert(!valid_room(truncated));
    }
    joined.Write(u8{0});
    assert(!valid_room(joined));
    Packet info;
    info.Write(u8{Network::IdRoomInformation}).Write(std::string{"Test room"})
        .Write(std::string{}).Write(u32{2}).Write(u16{24872})
        .Write(std::string{}).Write(std::string{}).Write(u32{0});
    assert(valid_room(info));
    Packet bad_info;
    bad_info.Append(info.GetData(), info.GetDataSize() - sizeof(u32));
    bad_info.Write(std::numeric_limits<u32>::max());
    assert(!valid_room(bad_info));

    Network::LDNPacket ldn{};
    ldn.type = Network::LDNPacketType::Scan;
    assert(Eden::Multiplayer::ValidLdnPacket(ldn));
    ldn.type = Network::LDNPacketType::ScanResp;
    assert(!Eden::Multiplayer::ValidLdnPacket(ldn));
    Service::LDN::NetworkInfo network{};
    network.ldn.node_count_max = 8;
    network.ldn.node_count = 1;
    const auto set_network = [&] {
        ldn.data.resize(sizeof(network));
        std::memcpy(ldn.data.data(), &network, sizeof(network));
    };
    set_network();
    assert(Eden::Multiplayer::ValidLdnPacket(ldn));
    network.ldn.node_count = 9;
    set_network();
    assert(!Eden::Multiplayer::ValidLdnPacket(ldn));
    network.ldn.node_count = 1;
    network.ldn.advertise_data_size = Service::LDN::AdvertiseDataSizeMax + 1;
    set_network();
    assert(!Eden::Multiplayer::ValidLdnPacket(ldn));
    ldn.type = Network::LDNPacketType::Connect;
    ldn.data.resize(sizeof(Service::LDN::NodeInfo));
    assert(Eden::Multiplayer::ValidLdnPacket(ldn));
    ldn.data.pop_back();
    assert(!Eden::Multiplayer::ValidLdnPacket(ldn));
    const auto compress = [](const std::vector<u8>& raw) {
        std::vector<u8> compressed(ZSTD_compressBound(raw.size()));
        const auto size = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
        assert(!ZSTD_isError(size));
        compressed.resize(size);
        return compressed;
    };
    auto compressed = compress(bytes);
    assert(Eden::Multiplayer::DecodeProxyPayload(compressed, decoded) && decoded == bytes);
    for (std::size_t size = 0; size < compressed.size(); ++size)
        assert(!Eden::Multiplayer::DecodeProxyPayload({compressed.data(), size}, decoded));
    compressed = compress({});
    assert(Eden::Multiplayer::DecodeProxyPayload(compressed, decoded) && decoded.empty());
    compressed = compress(std::vector<u8>(Eden::Multiplayer::MaxProxyPayloadBytes + 1, 0));
    assert(!Eden::Multiplayer::DecodeProxyPayload(compressed, decoded) && decoded.empty());
    assert(!Eden::Multiplayer::DecodeProxyPayload(bytes, decoded));
}
