// SPDX-License-Identifier: GPL-3.0-or-later
#include "multiplayer.h"
#include "multiplayer_session.h"
#include "multiplayer_logging.h"
#include "diagnostics.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "network/network.h"

namespace Eden::Multiplayer {
using namespace std::chrono_literals;
using Member = Network::RoomMember;

namespace {
const char* ErrorText(Member::Error error) {
    switch (error) {
    case Member::Error::LostConnection: return "Connection lost. Check your internet connection, then rejoin the room before restarting Local Wireless in the game.";
    case Member::Error::HostKicked: return "Removed by the host. Ask the host before trying to rejoin, or choose another room.";
    case Member::Error::NameCollision: return "Nickname already in use. Enter a different nickname and try again.";
    case Member::Error::IpCollision: return "Player address unavailable. Try joining again. If it repeats, ask the host to check the room server.";
    case Member::Error::WrongVersion: return "Incompatible room version. Update ProsperoEden or choose a compatible room; the host may also need to update the server.";
    case Member::Error::WrongPassword: return "Incorrect password. Enter the room password again; it is case-sensitive.";
    case Member::Error::CouldNotConnect: return "Could not reach the room. Check the address and UDP port, then retry. Ask the host to check that the server is online and its UDP port is reachable.";
    case Member::Error::RoomIsFull: return "Room full. Wait for a free place or choose another room.";
    case Member::Error::HostBanned: return "Banned from this room. Contact the host or choose another room.";
    case Member::Error::PermissionDenied: return "Permission denied. Ask the host whether you are allowed to perform this action.";
    case Member::Error::NoSuchUser: return "Player unavailable. Try again after the room information updates.";
    default: return "Connection failed. Try again. If it repeats, enable detailed logging in Settings and share the log with support.";
    }
}
bool ValidConnection(const Connection& connection) {
    if (connection.host.empty() || connection.host.size() > 253 || !connection.port ||
        connection.nickname.size() < 4 || connection.nickname.size() > 20 ||
        connection.password.size() > 128) return false;
    for (unsigned char c : connection.host)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    for (unsigned char c : connection.nickname)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '_')) return false;
    return true;
}
struct Resolution {
    std::atomic<bool> done{};
    int error{};
    std::string address;
};
} // namespace

struct RoomClient::Impl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    Snapshot snapshot;
    Connection requested;
    bool pending = false;
    bool disconnect = false;
    bool initialized = false;
    bool joining = false;
    std::stop_source attempt;
    std::shared_ptr<Resolution> resolution;
    std::shared_ptr<Member> member;
    Member::CallbackHandle<Member::State> states;
    Member::CallbackHandle<Member::Error> errors;
    Member::CallbackHandle<Network::RoomInformation> information;
    std::jthread worker;

    void Fail(std::string message) {
        std::lock_guard lock(mutex);
        if (snapshot.phase == Phase::Disconnecting) return;
        Eden::Report("multiplayer", message.c_str());
        if (!joining) snapshot.phase = Phase::Failed;
        snapshot.error = std::move(message);
        snapshot.room.clear();
        snapshot.members.clear();
        changed.notify_all();
    }

    std::string Resolve(const std::string& host, std::stop_token stop) {
        in_addr numeric{};
        if (inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
            Debug("Numeric IPv4 address; skipping hostname lookup");
            return host;
        }
        if (resolution && !resolution->done.load()) {
            Fail("A previous address lookup is still running. Retry later or use an IPv4 address.");
            return {};
        }
        const auto result = std::make_shared<Resolution>();
        Debug("Hostname lookup started");
        // libc DNS cannot be cancelled. At most one lookup survives cancellation; it owns no client state.
        std::thread([result, host] {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* addresses = nullptr;
            result->error = getaddrinfo(host.c_str(), nullptr, &hints, &addresses);
            if (result->error == 0) {
                char text[INET_ADDRSTRLEN]{};
                if (addresses && inet_ntop(AF_INET,
                    &reinterpret_cast<sockaddr_in*>(addresses->ai_addr)->sin_addr, text, sizeof(text)))
                    result->address = text;
                freeaddrinfo(addresses);
            }
            result->done.store(true);
        }).detach();
        resolution = result;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!stop.stop_requested() && !resolution->done.load() &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
        if (stop.stop_requested()) {
            Debug("Hostname lookup cancelled");
            return {};
        }
        const bool resolved = resolution->done.load();
        if (resolved) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "Hostname lookup result=%d usable_ipv4=%d",
                          resolution->error, !resolution->address.empty());
            Debug(detail);
        } else {
            Debug("Hostname lookup timed out");
        }
        if (!resolved || resolution->address.empty()) {
            Fail(!resolved ? "Address lookup timed out. Check the hostname and internet connection, or enter the server IPv4 address."
                           : "Address lookup failed. Check the hostname, or ask the host for the server IPv4 address.");
            return {};
        }
        Debug("Hostname lookup succeeded");
        return resolution->address;
    }

    void Join(Connection connection, std::stop_token stop) {
        const auto address = Resolve(connection.host, stop);
        if (address.empty() || stop.stop_requested()) return;
        Debug("Room handshake started");
        member->Join(connection.nickname, address.c_str(), connection.port, 0,
                     Network::NoPreferredIP, connection.password, "", stop);
        connection.password.clear();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        std::unique_lock lock(mutex);
        changed.wait_until(lock, deadline, [&] {
            return snapshot.phase != Phase::Connecting || !snapshot.error.empty() || stop.stop_requested();
        });
        const bool timed_out = snapshot.phase == Phase::Connecting && snapshot.error.empty() &&
                               !stop.stop_requested();
        lock.unlock();
        if (timed_out) Fail("Room join timed out. The server connection opened, but room acceptance did not finish. Retry; if it repeats, ask the host to check the server log.");
        if (timed_out || stop.stop_requested()) member->Leave();
    }

    void Run(std::stop_token shutdown) {
        while (!shutdown.stop_requested()) {
            Connection connection;
            std::stop_token stop;
            bool leave;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return pending || disconnect || shutdown.stop_requested(); });
                if (shutdown.stop_requested()) break;
                leave = disconnect;
                joining = !leave;
                disconnect = pending = false;
                connection = std::move(requested);
                stop = attempt.get_token();
            }
            member->Leave();
            if (leave) {
                std::lock_guard lock(mutex);
                snapshot = {};
                continue;
            }
            try {
                Join(std::move(connection), stop);
            } catch (const std::exception&) {
                Fail("Could not start the connection. Restart ProsperoEden and try again. If it repeats, enable detailed logging in Settings.");
                member->Leave();
            }
            {
                std::lock_guard lock(mutex);
                joining = false;
                if (!snapshot.error.empty() && snapshot.phase != Phase::Disconnecting)
                    snapshot.phase = Phase::Failed;
            }
        }
        member->Leave();
    }
};

RoomClient::RoomClient() : impl(std::make_unique<Impl>()) {
    auto& p = *impl;
    p.initialized = Network::Init();
    if (!p.initialized) {
        p.snapshot.error = "Networking unavailable. Restart ProsperoEden and check the console network connection before trying again.";
        return;
    }
    p.member = Network::GetRoomMember().lock();
    p.states = p.member->BindOnStateChanged([&p](const Member::State& state) {
        Debug(Network::GetStateStr(state));
        std::lock_guard lock(p.mutex);
        if (p.snapshot.phase == Phase::Disconnecting) return;
        if (p.snapshot.error.empty() &&
            (state == Member::State::Joined || state == Member::State::Moderator))
            p.snapshot.phase = Phase::Connected;
        // Idle precedes the error callback; preserve its specific error instead of guessing here.
        p.changed.notify_all();
    });
    p.errors = p.member->BindOnError([&p](const Member::Error& error) { p.Fail(ErrorText(error)); });
    p.information = p.member->BindOnRoomInformationChanged([&p](const Network::RoomInformation& info) {
        std::lock_guard lock(p.mutex);
        if (p.snapshot.phase == Phase::Disconnecting) return;
        p.snapshot.room = info.name;
        p.snapshot.members.clear();
        // Copy on the receive thread; the upstream member list is not safe to read from the UI.
        for (const auto& member : p.member->GetMemberInformation()) {
            std::string label = member.nickname;
            if (!member.game_info.name.empty()) {
                label += " - " + member.game_info.name;
                if (!member.game_info.version.empty()) label += " (" + member.game_info.version + ")";
            }
            p.snapshot.members.push_back(std::move(label));
        }
    });
    p.worker = std::jthread([&p](std::stop_token stop) { p.Run(stop); });
}

RoomClient::~RoomClient() {
    auto& p = *impl;
    if (!p.initialized) return;
    {
        std::lock_guard lock(p.mutex);
        p.snapshot.phase = Phase::Disconnecting;
        p.attempt.request_stop();
        p.worker.request_stop();
    }
    p.changed.notify_all();
    p.worker.join();
    p.member->Unbind(p.states);
    p.member->Unbind(p.errors);
    p.member->Unbind(p.information);
    p.member.reset();
    Network::Shutdown();
}

bool RoomClient::Connect(Connection connection) {
    auto& p = *impl;
    std::lock_guard lock(p.mutex);
    if (!p.initialized || guest_socket_mode.load() >= 0 || p.pending || p.disconnect ||
        (p.snapshot.phase != Phase::Idle && p.snapshot.phase != Phase::Failed))
        return false;
    if (!ValidConnection(connection)) {
        p.snapshot = {Phase::Failed, "Enter a valid host, port and 4–20 character nickname.", {}, {}};
        return false;
    }
    p.snapshot = {Phase::Connecting, {}, {}, {}};
    Debug("Join requested");
    p.requested = std::move(connection);
    p.attempt = std::stop_source{};
    p.pending = true;
    p.changed.notify_all();
    return true;
}

void RoomClient::Leave() {
    auto& p = *impl;
    std::lock_guard lock(p.mutex);
    if (!p.initialized || guest_socket_mode.load() >= 0 ||
        p.snapshot.phase == Phase::Idle || p.snapshot.phase == Phase::Disconnecting)
        return;
    p.snapshot = {Phase::Disconnecting, {}, {}, {}};
    Debug("Leave requested");
    p.attempt.request_stop();
    p.requested = {};
    p.disconnect = true;
    p.changed.notify_all();
}

Snapshot RoomClient::GetSnapshot() const {
    std::lock_guard lock(impl->mutex);
    return impl->snapshot;
}

bool RoomClient::BeginGame() {
    std::lock_guard lock(impl->mutex);
    const auto phase = impl->snapshot.phase;
    if (guest_socket_mode.load() >= 0 || (phase != Phase::Idle && phase != Phase::Connected)) return false;
    if (phase == Phase::Connected) {
        const auto state = impl->member->GetState();
        if (state != Member::State::Joined && state != Member::State::Moderator) return false;
    }
    guest_socket_mode = phase == Phase::Connected ? 1 : 0;
    Eden::Report("multiplayer", phase == Phase::Connected ?
        "Starting game with room networking" : "Starting game offline");
    return true;
}
void RoomClient::EndGame() { guest_socket_mode = -1; }
bool RoomClient::GameConnectionLost() const {
    if (guest_socket_mode.load() != 1) return false;
    const auto state = impl->member->GetState();
    return state != Member::State::Joined && state != Member::State::Moderator;
}
} // namespace Eden::Multiplayer
