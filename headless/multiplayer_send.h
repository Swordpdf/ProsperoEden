// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cassert>
#include <cstddef>
#include "multiplayer_validation.h"

namespace Eden::Multiplayer {
// Used under the send-list mutex or solely by ENet's owning thread.
class RoomSendBudget {
public:
    bool Add(std::size_t size) {
        if (size > MaxRoomPacketBytes || count == 1024 || size > 4 * MaxRoomPacketBytes - bytes)
            return false;
        bytes += size;
        ++count;
        return true;
    }
    void Release(std::size_t size) {
        assert(count && size <= bytes);
        --count;
        bytes -= size;
    }
    void Reset() { bytes = count = 0; }
private:
    std::size_t bytes = 0;
    std::size_t count = 0;
};
} // namespace Eden::Multiplayer
