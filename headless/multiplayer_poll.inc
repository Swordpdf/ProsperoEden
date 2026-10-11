// SPDX-License-Identifier: GPL-3.0-or-later
std::pair<s32, Errno> Poll(std::vector<PollFD>& pollfds, s32 timeout) {
    std::vector<PollFD> native;
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < pollfds.size(); ++i) {
        if (!pollfds[i].socket->PollProxy(pollfds[i].events)) {
            native.push_back(pollfds[i]);
            indices.push_back(i);
        }
    }
    if (native.size() == pollfds.size()) return PollNative(pollfds, timeout);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{std::max(timeout, 0)};
    const auto sample = [&] {
        s32 ready = 0;
        for (auto& entry : pollfds) {
            entry.revents = entry.socket->PollProxy(entry.events).value_or(PollEvents{});
            if (entry.revents != PollEvents{}) ++ready;
        }
        return ready;
    };
    for (;;) {
        const auto ready_before_wait = sample();
        for (auto& entry : native) entry.revents = PollEvents{};
        // Retain the native shutdown interrupt and mixed native sockets. Proxy data lives
        // in memory, so cap the native wait to one millisecond instead of busy spinning.
        // shortcut: use wakeup descriptors if console profiling finds this polling too costly.
        const auto [native_ready, error] = PollNative(native, ready_before_wait || timeout == 0 ? 0 : 1);
        if (error != Errno::SUCCESS) return {-1, error};
        s32 ready = sample();
        for (std::size_t i = 0; i < native.size(); ++i) {
            pollfds[indices[i]].revents = native[i].revents;
            if (native[i].revents != PollEvents{}) ++ready;
        }
        // A native interrupt also ends the wait when no guest descriptor is ready.
        if (ready || native_ready || timeout == 0 ||
            (timeout > 0 && std::chrono::steady_clock::now() >= deadline))
            return {ready, Errno::SUCCESS};
    }
}
