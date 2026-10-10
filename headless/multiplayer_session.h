// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
namespace Eden::Multiplayer {
// A room loss must not make newly created guest sockets escape onto the host network.
inline std::atomic<int> guest_socket_mode{-1}; // -1: no game; 0: offline; 1: room proxy
}
