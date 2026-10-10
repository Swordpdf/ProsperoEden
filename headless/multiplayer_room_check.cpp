// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real upstream room server and the derived client over loopback.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "common/logging.h"
#include "common/assert.h"
#include "network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "multiplayer_proxy.h"
#include "multiplayer.h"
#include "multiplayer_session.h"
#include "enet/enet.h"
#include "network/packet.h"

// Compile the generated production destructor without the unrelated IPC framework.
namespace Service::Sockets {
class BSD_USA {
public:
    struct FileDescriptor { std::shared_ptr<Network::SocketBase> socket; };
    static inline std::atomic<unsigned> live_instances{};
    static inline std::array<std::optional<FileDescriptor>, 128> file_descriptors{};
    BSD_USA() { ++live_instances; }
    ~BSD_USA();
};
#include "bsd_lifetime.cpp"
}

void CheckBsdLifetime() {
    using Service::Sockets::BSD_USA;
    auto first = std::make_unique<BSD_USA>();
    auto second = std::make_unique<BSD_USA>();
    auto last = std::make_unique<BSD_USA>();
    auto socket = std::make_shared<Network::ProxySocket>();
    socket->Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP);
    BSD_USA::file_descriptors[0] = BSD_USA::FileDescriptor{socket};
    BSD_USA::file_descriptors[1] = BSD_USA::FileDescriptor{socket};
    first.reset();
    second.reset();
    assert(socket->IsOpened() && BSD_USA::file_descriptors[0]);
    last.reset();
    assert(!socket->IsOpened() && BSD_USA::live_instances == 0);
    for (const auto& entry : BSD_USA::file_descriptors) assert(!entry);
    std::weak_ptr<Network::ProxySocket> released = socket;
    socket.reset();
    assert(released.expired());
    BSD_USA next_game;
    for (const auto& entry : BSD_USA::file_descriptors) assert(!entry);
}

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
    socket.SetNonBlock(false);
    Network::Errno closed_result{};
    const auto close_start = std::chrono::steady_clock::now();
    std::jthread receive([&] { closed_result = socket.RecvFrom(0, bytes, nullptr).second; });
    socket.Close();
    receive.join();
    assert(closed_result == Network::Errno::BADF);
    assert(std::chrono::steady_clock::now() - close_start < std::chrono::seconds{2});
}

void CheckProxyPoll() {
    using namespace Network;
    ProxySocket proxy;
    proxy.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP);
    const SockAddrIn endpoint{Domain::INET, {127, 0, 0, 1}, 1234};
    proxy.Bind(endpoint);
    std::vector<PollFD> fds{{&proxy, PollEvents::In, {}}};
    assert(Poll(fds, 0).first == 0 && fds[0].revents == PollEvents{});
    fds[0].events = PollEvents::Out;
    assert(Poll(fds, 0).first == 1 && fds[0].revents == PollEvents::Out);
    fds[0].events = PollEvents::In;
    const auto start = std::chrono::steady_clock::now();
    assert(Poll(fds, 20).first == 0);
    assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{20});
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});

    ProxyPacket packet{};
    packet.remote_endpoint = endpoint;
    packet.protocol = Protocol::UDP;
    const std::array<u8, 1> bytes{42};
    packet.data.resize(ZSTD_compressBound(bytes.size()));
    packet.data.resize(ZSTD_compress(packet.data.data(), packet.data.size(), bytes.data(), bytes.size(), 1));
    std::jthread arrival([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        proxy.HandleProxyPacket(packet);
    });
    assert(Poll(fds, -1).first == 1 && fds[0].revents == PollEvents::In);
    arrival.join();
    assert(Poll(fds, 0).first == 1); // Poll never consumes the packet.
    std::array<u8, 1> received{};
    assert(proxy.RecvFrom(0, received, nullptr).first == 1 && received == bytes);
    assert(Poll(fds, 0).first == 0);

    Socket native, sender;
    assert(native.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    assert(sender.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    assert(native.Bind({Domain::INET, {127, 0, 0, 1}, 0}) == Errno::SUCCESS);
    const auto [address, address_error] = native.GetSockName();
    assert(address_error == Errno::SUCCESS);
    fds.push_back({&native, PollEvents::In, {}});
    assert(sender.SendTo(0, bytes, &address).first == 1);
    assert(Poll(fds, 1000).first == 1);
    assert(fds[0].revents == PollEvents{} && fds[1].revents == PollEvents::In);
    assert(native.RecvFrom(0, received, nullptr).first == 1);
    proxy.HandleProxyPacket(packet);
    assert(sender.SendTo(0, bytes, &address).first == 1);
    assert(Poll(fds, 1000).first == 2);
    assert(proxy.RecvFrom(0, received, nullptr).first == 1);
    assert(native.RecvFrom(0, received, nullptr).first == 1);
    assert(Poll(fds, 0).first == 0); // No stale readiness from the previous native poll.
    std::vector<PollFD> native_only{{&native, PollEvents::In, {}}};
    assert(Poll(native_only, 0).first == 0);

    std::jthread interrupt([] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        CancelPendingSocketOperations();
    });
    assert(Poll(fds, -1).first == 0);
    interrupt.join();
    RestartSocketOperations();
    std::jthread close([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        proxy.Close();
    });
    assert(Poll(fds, -1).first == 1 && fds[0].revents == PollEvents::Nval);
    close.join();
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

void CheckSocketDelivery(u16 port) {
    using namespace Eden::Multiplayer;
    RoomClient client;
    Network::Room room;
    assert(room.Create("Socket delivery", "", "127.0.0.1", port, "", 2,
                      "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
    assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    Network::RoomMember peer;
    peer.Join("PlayerTwo", "127.0.0.1", port);
    Wait([&] { return peer.GetState() == Network::RoomMember::State::Joined; });
    Network::ProxyPacket packet{};
    packet.local_endpoint = {Network::Domain::INET, peer.GetFakeIpAddress(), 4321};
    packet.remote_endpoint = {Network::Domain::INET,
                             Network::GetRoomMember().lock()->GetFakeIpAddress(), 1234};
    packet.protocol = Network::Protocol::UDP;
    const u8 data[]{4, 5, 6};
    packet.data.resize(ZSTD_compressBound(sizeof(data)));
    packet.data.resize(ZSTD_compress(packet.data.data(), packet.data.size(), data, sizeof(data), 1));
    const auto make_socket = [&] {
        auto socket = std::make_shared<Network::ProxySocket>();
        socket->Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP);
        socket->Bind(packet.remote_endpoint);
        socket->SetNonBlock(true);
        return socket;
    };
    auto socket = make_socket();
    auto duplicate = socket;
    assert(socket->IsOpened());
    peer.SendProxyPacket(packet);
    std::array<u8, 3> bytes{};
    bool received = false;
    Wait([&] {
        if (!received) received = socket->RecvFrom(0, bytes, nullptr).first == 3;
        return received;
    });
    assert(bytes == (std::array<u8, 3>{4, 5, 6}));
    assert(duplicate->RecvFrom(0, bytes, nullptr).second == Network::Errno::AGAIN);
    socket->Close();
    assert(!duplicate->IsOpened());
    assert(duplicate->RecvFrom(0, bytes, nullptr).second == Network::Errno::BADF);
    duplicate.reset();
    socket.reset();
    // Destruction must unbind safely while the room thread is delivering packets.
    std::jthread traffic([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            peer.SendProxyPacket(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    for (int i = 0; i < 50; ++i) {
        auto transient = make_socket();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        transient->Close();
    }
    traffic.request_stop();
    traffic.join();
    peer.Leave();
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    room.Destroy();
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
    Network::NetworkInstance socket_network;
    assert(Network::Init());
    CheckProxySocket();
    CheckBsdLifetime();
    CheckProxyPoll();
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
    CheckSocketDelivery(port);
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
