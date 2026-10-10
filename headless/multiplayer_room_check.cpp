// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real upstream room server and the derived client over loopback.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "common/logging.h"
#include "network/network.h"

template <typename Predicate>
void Wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    assert(predicate());
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const auto port = static_cast<u16>(std::strtoul(argv[1], nullptr, 10));
    Common::Log::Initialize();
    assert(Network::Init());
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
}
