// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include "core/internal_network/sockets.h"

namespace Eden::Multiplayer {
struct SocketDescriptor {
    std::shared_ptr<Network::SocketBase> socket;
    std::atomic<s32> flags{0};
    bool is_connection_based = false;
};

// Lookups retain ownership; no table lock is held while a guest blocks on a socket.
class SocketDescriptors {
public:
    static constexpr std::size_t Capacity = 128;
    std::shared_ptr<SocketDescriptor> Get(s32 fd) const {
        std::lock_guard lock(mutex);
        return Valid(fd) ? slots[fd] : nullptr;
    }
    s32 Insert(std::shared_ptr<Network::SocketBase> socket, bool connection_based) {
        if (!socket) return -1;
        auto entry = std::make_shared<SocketDescriptor>();
        entry->socket = std::move(socket);
        entry->is_connection_based = connection_based;
        std::lock_guard lock(mutex);
        return InsertLocked(std::move(entry));
    }
    std::pair<s32, Network::Errno> Duplicate(s32 fd) {
        std::lock_guard lock(mutex);
        if (!Valid(fd) || !slots[fd]) return {-1, Network::Errno::BADF};
        const auto duplicated = InsertLocked(slots[fd]);
        return {duplicated, duplicated < 0 ? Network::Errno::MFILE : Network::Errno::SUCCESS};
    }
    Network::Errno Close(s32 fd) {
        std::shared_ptr<SocketDescriptor> entry;
        bool last;
        {
            std::lock_guard lock(mutex);
            if (!Valid(fd) || !slots[fd]) return Network::Errno::BADF;
            entry = std::move(slots[fd]);
            last = std::find(slots.begin(), slots.end(), entry) == slots.end();
        }
        return last ? entry->socket->Close() : Network::Errno::SUCCESS;
    }
    void Clear() {
        decltype(slots) retired;
        {
            std::lock_guard lock(mutex);
            retired.swap(slots);
        }
        for (const auto& entry : retired)
            if (entry && entry->socket->IsOpened()) entry->socket->Close();
    }
private:
    static bool Valid(s32 fd) { return fd >= 0 && static_cast<std::size_t>(fd) < Capacity; }
    s32 InsertLocked(std::shared_ptr<SocketDescriptor> entry) {
        const auto free = std::find(slots.begin(), slots.end(), nullptr);
        if (free == slots.end()) return -1;
        *free = std::move(entry);
        return static_cast<s32>(free - slots.begin());
    }
    mutable std::mutex mutex;
    std::array<std::shared_ptr<SocketDescriptor>, Capacity> slots{};
};
} // namespace Eden::Multiplayer
