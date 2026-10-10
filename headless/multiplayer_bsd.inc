// SPDX-License-Identifier: GPL-3.0-or-later
std::pair<s32, Errno> BSD_USA::SocketImpl(Domain domain, Type type, Protocol protocol) {
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW) &&
        !(type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP))
        return {-1, Errno::INVAL};
    const bool unknown = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unknown, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);
    const bool connection_based = IsConnectionBased(type);
    if (Settings::values.airplane_mode.GetValue() && connection_based)
        return {-1, Errno::NOTCONN};
    auto member = Network::GetRoomMember().lock();
    const int mode = Eden::Multiplayer::guest_socket_mode.load();
    std::shared_ptr<Network::SocketBase> socket;
    if (mode == 1 || (mode < 0 && member && member->IsConnected()))
        socket = std::make_shared<Network::ProxySocket>();
    else
        socket = std::make_shared<Network::Socket>();
    const auto error = socket->Initialize(Translate(domain), Translate(type), Translate(protocol));
    if (error != Network::Errno::SUCCESS) return {-1, Translate(error)};
    const auto fd = file_descriptors.Insert(std::move(socket), connection_based);
    return {fd, fd < 0 ? Errno::MFILE : Errno::SUCCESS};
}

std::pair<s32, Errno> BSD_USA::PollImpl(std::vector<u8>& write_buffer,
                                     std::span<const u8> read_buffer, s32 nfds, s32 timeout) {
    if (nfds <= 0) return {-1, Errno::SUCCESS};
    const auto count = static_cast<std::size_t>(nfds);
    if (count > MAX_FD || timeout < -1 || read_buffer.size() / sizeof(PollFD) < count ||
        write_buffer.size() / sizeof(PollFD) < count) return {-1, Errno::INVAL};
    std::vector<PollFD> fds(count);
    std::memcpy(fds.data(), read_buffer.data(), count * sizeof(PollFD));
    std::vector<std::shared_ptr<FileDescriptor>> retained;
    std::vector<Network::PollFD> host_fds;
    std::vector<std::size_t> indices;
    s32 invalid = 0;
    for (std::size_t i = 0; i < count; ++i) {
        fds[i].revents = PollEvents{};
        if (fds[i].fd < 0) continue;
        auto entry = file_descriptors.Get(fds[i].fd);
        if (!entry) {
            fds[i].revents = PollEvents::Nval;
            ++invalid;
            continue;
        }
        host_fds.push_back({entry->socket.get(), Translate(fds[i].events), {}});
        retained.push_back(std::move(entry));
        indices.push_back(i);
    }
    auto result = Network::Poll(host_fds, invalid ? 0 : timeout);
    for (std::size_t i = 0; i < indices.size(); ++i)
        fds[indices[i]].revents = Translate(host_fds[i].revents);
    std::memcpy(write_buffer.data(), fds.data(), count * sizeof(PollFD));
    if (result.first >= 0) result.first += invalid;
    return Translate(result);
}

std::pair<s32, Errno> BSD_USA::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto entry = file_descriptors.Get(fd);
    if (!entry) return {-1, Errno::BADF};
    auto [accepted, error] = entry->socket->Accept();
    if (error != Network::Errno::SUCCESS) return {-1, Translate(error)};
    if (!accepted.socket) return {-1, Errno::NOTCONN};
    const auto new_fd = file_descriptors.Insert(std::move(accepted.socket), entry->is_connection_based);
    if (new_fd < 0) return {-1, Errno::MFILE};
    PutValue(write_buffer, Translate(accepted.sockaddr_in));
    return {new_fd, Errno::SUCCESS};
}

Errno BSD_USA::CloseImpl(s32 fd) {
    return Translate(file_descriptors.Close(fd));
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    const auto [duplicated, error] = file_descriptors.Duplicate(fd);
    if (error != Network::Errno::SUCCESS) return Translate(error);
    return duplicated;
}
