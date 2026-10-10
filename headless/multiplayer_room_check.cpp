// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real upstream room server and the derived client over loopback.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "common/logging.h"
#include "network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "multiplayer_proxy.h"
#include "multiplayer.h"
#include "multiplayer_session.h"
#include "enet/enet.h"
#include "network/packet.h"

template <typename Predicate>
void Wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    assert(predicate());
}

void CheckProxySocket() {
    Network::ProxySocket socket;
    assert(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                             Network::Protocol::UDP) == Network::Errno::SUCCESS);
    assert(socket.Bind({Network::Domain::INET, {192, 168, 0, 2}, 1234}) == Network::Errno::SUCCESS);
    socket.SetNonBlock(true);
    const std::vector<u8> oversized(Eden::Multiplayer::MaxProxyPayloadBytes + 1);
    assert(socket.SendTo(0, oversized, nullptr).second == Network::Errno::MSGSIZE);
    Network::ProxyPacket packet{};
    packet.local_endpoint = {Network::Domain::INET, {192, 168, 0, 1}, 4321};
    packet.remote_endpoint = {Network::Domain::INET, {192, 168, 0, 2}, 1234};
    packet.protocol = Network::Protocol::UDP;
    const auto payload = [&](const std::vector<u8>& bytes) {
        packet.data.resize(ZSTD_compressBound(bytes.size()));
        const auto size = ZSTD_compress(packet.data.data(), packet.data.size(),
                                       bytes.data(), bytes.size(), 1);
        assert(!ZSTD_isError(size));
        packet.data.resize(size);
    };
    payload({4, 5, 6});
    socket.HandleProxyPacket(packet);
    std::vector<u8> bytes(Eden::Multiplayer::MaxProxyPayloadBytes);
    Network::SockAddrIn sender{};
    auto result = socket.RecvFrom(0, bytes, &sender);
    assert(result.first == 3 && bytes[0] == 4 && bytes[2] == 6 && sender.portno == 4321);
    packet.data = {1, 2, 3};
    socket.HandleProxyPacket(packet);
    assert(socket.RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);

    // Empty datagrams still occupy a queue slot, so both count and byte limits matter.
    payload({});
    for (std::size_t i = 0; i < Eden::Multiplayer::MaxProxyQueuePackets + 1; ++i)
        socket.HandleProxyPacket(packet);
    for (std::size_t i = 0; i < Eden::Multiplayer::MaxProxyQueuePackets; ++i)
        assert(socket.RecvFrom(0, bytes, nullptr).first == 0);
    assert(socket.RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);
    payload(bytes);
    const auto capacity = Eden::Multiplayer::MaxProxyQueueBytes / bytes.size();
    for (std::size_t i = 0; i <= capacity; ++i) socket.HandleProxyPacket(packet);
    for (std::size_t i = 0; i < capacity; ++i)
        assert(socket.RecvFrom(0, bytes, nullptr).first == static_cast<s32>(bytes.size()));
    assert(socket.RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);
    payload({7});
    socket.HandleProxyPacket(packet);
    assert(socket.RecvFrom(0, bytes, nullptr).first == 1 && bytes[0] == 7);

    payload({1, 2, 3});
    socket.HandleProxyPacket(packet);
    assert(socket.RecvFrom(0, std::span{bytes}.first(1), nullptr).second == Network::Errno::MSGSIZE);
    assert(socket.RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);

    Network::ProxySocket stream;
    assert(stream.Initialize(Network::Domain::INET, Network::Type::STREAM,
                             Network::Protocol::TCP) == Network::Errno::SUCCESS);
    assert(stream.Bind(packet.remote_endpoint) == Network::Errno::SUCCESS);
    stream.SetNonBlock(true);
    packet.protocol = Network::Protocol::TCP;
    stream.HandleProxyPacket(packet);
    // ReceivePacket is called under the socket lock in production; this test is single-threaded.
    assert(stream.ReceivePacket(Network::FLAG_MSG_PEEK, bytes, nullptr, 1).first == 1);
    assert(stream.RecvFrom(0, std::span{bytes}.first(1), nullptr).first == 1 && bytes[0] == 1);
    assert(stream.RecvFrom(0, bytes, nullptr).first == 2 && bytes[0] == 2 && bytes[1] == 3);
    assert(stream.RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);
    // A full-size packet must still fit after partial reads and a truncated datagram.
    packet.protocol = Network::Protocol::UDP;
    payload(bytes);
    for (std::size_t i = 0; i < capacity; ++i) socket.HandleProxyPacket(packet);
    for (std::size_t i = 0; i < capacity; ++i)
        assert(socket.RecvFrom(0, bytes, nullptr).first == static_cast<s32>(bytes.size()));
}

void CheckController(u16 port) {
    using namespace Eden::Multiplayer;
    RoomClient client;
    Network::Room room;
    assert(room.Create("Controller test", "", "127.0.0.1", port, "secret", 2,
                      "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
    Connection connection{"localhost", port, "PlayerOne", "wrong"};
    auto invalid = connection;
    invalid.nickname = "x";
    assert(!client.Connect(invalid));
    assert(client.Connect(connection));
    assert(!client.BeginGame());
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "The room password is incorrect.");
    connection.password = "secret";
    assert(client.Connect(connection));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    const auto snapshot = client.GetSnapshot();
    assert(snapshot.room == "Controller test" && snapshot.members == std::vector<std::string>{"PlayerOne"});
    assert(!client.Connect(connection));
    assert(client.BeginGame());
    assert(guest_socket_mode == 1 && !client.GameConnectionLost());
    client.Leave();
    assert(client.GetSnapshot().phase == Phase::Connected);
    client.EndGame();
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    assert(client.GetSnapshot().members.empty());
    assert(client.BeginGame());
    assert(guest_socket_mode == 0 && !client.Connect(connection));
    client.EndGame();
    assert(client.Connect(connection));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    assert(client.BeginGame());
    room.Destroy();
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "The connection to the room was lost.");
    assert(client.GameConnectionLost() && guest_socket_mode == 1);
    assert(!client.Connect(connection));
    client.EndGame();
    connection.host = "127.0.0.1";
    assert(client.Connect(connection));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto start = std::chrono::steady_clock::now();
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
    assert(client.Connect(connection));
    // Destruction must also cancel a join without waiting for its five-second timeout.
}

void CheckTestRoom(u16 port, bool stall) {
    using namespace Eden::Multiplayer;
    RoomClient client;
    ENetAddress address{};
    assert(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    address.port = port;
    auto* server = enet_host_create(&address, 1, Network::NumChannels, 0, 0);
    assert(server);
    std::atomic<bool> paused{};
    std::jthread replies([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            ENetEvent event{};
            if (enet_host_service(server, &event, 10) <= 0) continue;
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                const bool join = event.packet->dataLength && event.packet->data[0] == Network::IdJoinRequest;
                enet_packet_destroy(event.packet);
                if (!join) continue;
                if (stall) {
                    Network::Packet info;
                    info.Write(u8{Network::IdRoomInformation}).Write(std::string{"Test room"})
                        .Write(std::string{}).Write(u32{2}).Write(port)
                        .Write(std::string{}).Write(std::string{}).Write(u32{1})
                        .Write(std::string{"PlayerOne"}).Write(Network::IPv4Address{192, 168, 0, 1})
                        .Write(std::string{}).Write(u64{0}).Write(std::string{})
                        .Write(std::string{}).Write(std::string{}).Write(std::string{});
                    auto* packet = enet_packet_create(info.GetData(), info.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
                    assert(enet_peer_send(event.peer, 0, packet) == 0);
                }
                // Without room information, the otherwise valid success must be rejected.
                const u8 reply[]{Network::IdJoinSuccess, 192, 168, 0, 1};
                auto* packet = enet_packet_create(reply, sizeof(reply), ENET_PACKET_FLAG_RELIABLE);
                assert(enet_peer_send(event.peer, 0, packet) == 0);
                enet_host_flush(server);
                if (stall) paused = true;
            }
        }
    });
    assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
    if (stall) {
        Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
        auto member = Network::GetRoomMember().lock();
        Network::ProxyPacket packet{};
        packet.protocol = Network::Protocol::UDP;
        packet.data.resize(Eden::Multiplayer::MaxProxyPayloadBytes / 2, 7);
        // The peer has stopped processing ENet packets, including acknowledgements.
        for (unsigned i = 0; i < 20 && client.GetSnapshot().phase == Phase::Connected; ++i) {
            member->SendProxyPacket(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == (stall ? "The connection to the room was lost." : "The room connection failed."));
    paused = false;
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    replies.request_stop();
    replies.join();
    enet_host_destroy(server);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const auto port = static_cast<u16>(std::strtoul(argv[1], nullptr, 10));
    Common::Log::Initialize();
    assert(Network::Init());
    CheckProxySocket();
    {
        Network::Room room;
        assert(room.Create("Offline test", "", "127.0.0.1", port, "secret", 2,
                           "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
        Network::RoomMember first, second;
        std::atomic<bool> rejected{}, scanned{}, proxied{}, lost{};
        const auto errors = first.BindOnError([&](const auto& error) {
            if (error == Network::RoomMember::Error::WrongPassword) rejected = true;
            if (error == Network::RoomMember::Error::LostConnection) lost = true;
        });
        first.Join("PlayerOne", "127.0.0.1", port, 0, Network::NoPreferredIP, "wrong");
        Wait([&] { return rejected.load(); });
        first.Leave();
        first.Join("PlayerOne", "127.0.0.1", port, 0, Network::NoPreferredIP, "secret");
        second.Join("PlayerTwo", "127.0.0.1", port, 0, Network::NoPreferredIP, "secret");
        const auto joined = [](const auto& member) {
            return member.GetState() == Network::RoomMember::State::Joined;
        };
        Wait([&] { return joined(first) && joined(second); });
        assert(first.GetFakeIpAddress() != second.GetFakeIpAddress());
        const auto ldn_callback = second.BindOnLdnPacketReceived([&](const auto& packet) {
            scanned = packet.type == Network::LDNPacketType::Scan && packet.broadcast;
        });
        const auto proxy_callback = second.BindOnProxyPacketReceived([&](const auto& packet) {
            proxied = packet.data == std::vector<u8>{1, 2, 3} &&
                      packet.remote_endpoint.portno == 1234;
        });
        first.SendLdnPacket({Network::LDNPacketType::Scan, first.GetFakeIpAddress(), {}, true, {}});
        Network::ProxyPacket packet{};
        packet.local_endpoint = {Network::Domain::INET, first.GetFakeIpAddress(), 1234};
        packet.remote_endpoint = {Network::Domain::INET, second.GetFakeIpAddress(), 1234};
        packet.protocol = Network::Protocol::UDP;
        packet.data = {1, 2, 3};
        first.SendProxyPacket(packet);
        Wait([&] { return scanned.load() && proxied.load(); });
        second.Leave();
        second.Unbind(ldn_callback);
        second.Unbind(proxy_callback);
        room.Destroy();
        Wait([&] { return lost.load(); });
        first.Leave();
        first.Unbind(errors);
    }
    Network::Shutdown();
    CheckController(port);
    CheckTestRoom(port, false);
    CheckTestRoom(port, true);
    const auto start = std::chrono::steady_clock::now();
    {
        Eden::Multiplayer::RoomClient client;
        assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
}
