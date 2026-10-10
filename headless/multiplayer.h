// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Eden::Multiplayer {
struct Connection {
    std::string host;
    std::uint16_t port = 24872;
    std::string nickname;
    std::string password;
};
enum class Phase { Idle, Connecting, Connected, Disconnecting, Failed };
struct Snapshot {
    Phase phase = Phase::Idle;
    std::string error;
    std::string room;
    std::vector<std::string> members;
};

// One application-lifetime owner, created after logging, before game services, and destroyed after them.
class RoomClient {
public:
    RoomClient();
    ~RoomClient();
    RoomClient(const RoomClient&) = delete;
    RoomClient& operator=(const RoomClient&) = delete;
    bool Connect(Connection connection);
    void Leave();
    Snapshot GetSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Eden::Multiplayer
