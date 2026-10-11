// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <fcntl.h>
#include <sys/socket.h>

namespace Eden::Multiplayer {
// ENet asks for a non-blocking socket with fcntl(O_NONBLOCK) and ignores the result. A sandboxed
// console title's libc refuses fcntl on sockets (see update_check/console_curl.c), so the room
// socket stayed blocking: enet_host_service then waits in recvmsg for the next datagram instead
// of returning after its timeout, and the member loop never gets to send the join request,
// acknowledgements or pings unless the room happens to send something that produces an event.
inline bool MakeSocketNonBlocking(int socket) {
#if defined(__PROSPERO__)
    constexpr int ConsoleNonBlocking = 0x1200; // SO_NBIO, the console's own option
    const int on = 1;
    return setsockopt(socket, SOL_SOCKET, ConsoleNonBlocking, &on, sizeof(on)) == 0;
#else
    const int flags = fcntl(socket, F_GETFL);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}
} // namespace Eden::Multiplayer
