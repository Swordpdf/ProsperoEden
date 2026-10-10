// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real upstream room server and the derived client over loopback.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "common/logging.h"
#include "common/assert.h"
#include "common/settings.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "multiplayer_proxy.h"
#include "multiplayer.h"
#include "multiplayer_session.h"
#include "multiplayer_descriptors.h"
#include "enet/enet.h"
#include "network/packet.h"

// Compile production BSD operations without the unrelated IPC framework.
namespace Service::Sockets {
class BSD_USA {
public:
    using FileDescriptor = Eden::Multiplayer::SocketDescriptor;
    static constexpr auto MAX_FD = Eden::Multiplayer::SocketDescriptors::Capacity;
    static inline std::atomic<unsigned> live_instances{};
    static inline Eden::Multiplayer::SocketDescriptors file_descriptors;
    bool is_user = true;
    BSD_USA() { ++live_instances; }
    ~BSD_USA();
    std::pair<s32, Errno> SocketImpl(Domain, Type, Protocol);
    std::pair<s32, Errno> PollImpl(std::vector<u8>&, std::span<const u8>, s32, s32);
    std::pair<s32, Errno> AcceptImpl(s32, std::vector<u8>&);
    Errno CloseImpl(s32);
    std::variant<s32, Errno> DuplicateSocketImpl(s32);
};
#include "bsd_helpers.cpp"
#include "bsd_lifetime.cpp"
#include "multiplayer_bsd.inc"
}

void CheckBsdService() {
    using namespace Service::Sockets;
    BSD_USA bsd;
    Eden::Multiplayer::guest_socket_mode = 1;
    const auto [fd, error] = bsd.SocketImpl(Domain::INET, Type::DGRAM, Protocol::UDP);
    assert(fd == 0 && error == Errno::SUCCESS);
    const auto duplicate = std::get<s32>(bsd.DuplicateSocketImpl(fd));
    assert(duplicate == 1);
    assert(bsd.CloseImpl(duplicate) == Errno::SUCCESS);
    assert(bsd.file_descriptors.Get(fd)->socket->IsOpened());
    std::array<PollFD, 3> input{{{fd, PollEvents::Out, {}}, {128, PollEvents::In, {}}, {-1, PollEvents::In, {}}}};
    std::vector<u8> read(sizeof(input)), written(sizeof(input));
    std::memcpy(read.data(), input.data(), read.size());
    assert(bsd.PollImpl(written, read, 3, 0).first == 2);
    std::memcpy(input.data(), written.data(), written.size());
    assert(input[0].revents == PollEvents::Out && input[1].revents == PollEvents::Nval &&
           input[2].revents == PollEvents{});
    assert(bsd.PollImpl(written, {}, 1, 0).second == Errno::INVAL);
    assert(bsd.PollImpl(written, read, 129, 0).second == Errno::INVAL);
    assert(bsd.PollImpl(written, read, 1, -2).second == Errno::INVAL);
    std::vector<u8> empty;
    assert(bsd.PollImpl(empty, read, 1, 0).second == Errno::INVAL);
    assert(bsd.AcceptImpl(fd, written).second == Errno::NOTCONN);
    std::weak_ptr<Network::SocketBase> released = bsd.file_descriptors.Get(fd)->socket;
    input[0] = {fd, PollEvents::In, {}};
    std::memcpy(read.data(), input.data(), read.size());
    std::jthread close([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        assert(bsd.CloseImpl(fd) == Errno::SUCCESS);
    });
    assert(bsd.PollImpl(written, read, 1, -1).first == 1);
    close.join();
    std::memcpy(input.data(), written.data(), sizeof(PollFD));
    assert(input[0].revents == PollEvents::Nval && released.expired());
    assert(bsd.CloseImpl(fd) == Errno::BADF);
    assert(std::get<Errno>(bsd.DuplicateSocketImpl(128)) == Errno::BADF);
    Eden::Multiplayer::guest_socket_mode = -1;
}

void CheckBsdLifetime() {
    using Service::Sockets::BSD_USA;
    auto first = std::make_unique<BSD_USA>();
    auto second = std::make_unique<BSD_USA>();
    auto last = std::make_unique<BSD_USA>();
    auto socket = std::make_shared<Network::ProxySocket>();
    socket->Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP);
    assert(BSD_USA::file_descriptors.Insert(socket, false) == 0);
    assert(BSD_USA::file_descriptors.Duplicate(0).first == 1);
    first.reset();
    second.reset();
    assert(socket->IsOpened() && BSD_USA::file_descriptors.Get(0));
    last.reset();
    assert(!socket->IsOpened() && BSD_USA::live_instances == 0);
    for (int fd = 0; fd < 128; ++fd) assert(!BSD_USA::file_descriptors.Get(fd));
    std::weak_ptr<Network::ProxySocket> released = socket;
    socket.reset();
    assert(released.expired());
    BSD_USA next_game;
    for (int fd = 0; fd < 128; ++fd) assert(!BSD_USA::file_descriptors.Get(fd));
}

void CheckDescriptors() {
    using namespace Network;
    Eden::Multiplayer::SocketDescriptors table;
    const auto make = [] {
        auto socket = std::make_shared<ProxySocket>();
        socket->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP);
        return socket;
    };
    assert(!table.Get(-1) && !table.Get(128));
    assert(table.Close(128) == Errno::BADF && table.Duplicate(-1).second == Errno::BADF);
    assert(table.Insert(nullptr, false) == -1);
    auto original = make();
    assert(table.Insert(original, false) == 0);
    auto retained = table.Get(0);
    assert(table.Duplicate(0).first == 1);
    retained->flags = FLAG_O_NONBLOCK;
    assert(table.Get(1)->flags == FLAG_O_NONBLOCK);
    assert(table.Close(0) == Errno::SUCCESS && original->IsOpened());
    auto replacement = make();
    assert(table.Insert(replacement, false) == 0);
    assert(table.Close(1) == Errno::SUCCESS && !original->IsOpened());
    assert(retained->socket == original && table.Get(0)->socket == replacement);
    table.Clear();
    assert(!replacement->IsOpened());
    for (int fd = 0; fd < 128; ++fd) assert(table.Insert(make(), false) == fd);
    assert(table.Insert(make(), false) == -1 && table.Duplicate(0).second == Errno::MFILE);
    table.Clear();
    std::vector<std::jthread> workers;
    for (int thread = 0; thread < 4; ++thread) workers.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            auto socket = make();
            const auto fd = table.Insert(socket, false);
            assert(fd >= 0);
            auto snapshot = table.Get(fd);
            const auto [duplicate, error] = table.Duplicate(fd);
            assert(error == Errno::SUCCESS && snapshot->socket == socket);
            assert(table.Close(fd) == Errno::SUCCESS && socket->IsOpened());
            assert(table.Close(duplicate) == Errno::SUCCESS && !socket->IsOpened());
            assert(snapshot->socket == socket);
        }
    });
    workers.clear();
    for (int fd = 0; fd < 128; ++fd) assert(!table.Get(fd));
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
    auto member = Network::GetRoomMember().lock();
    assert(member);
    member->SendGameInfo({"Example game", 1, "1.2.3"});
    Wait([&] { return client.GetSnapshot().members ==
        std::vector<std::string>{"PlayerOne - Example game (1.2.3)"}; });
    member->SendGameInfo({});
    Wait([&] { return client.GetSnapshot().members == std::vector<std::string>{"PlayerOne"}; });
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
    CheckDescriptors();
    CheckBsdService();
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
