# SPDX-License-Identifier: GPL-3.0-or-later
# Keep Eden's wire format; validate lengths before trusting remote allocation requests.
# Also usable from the focused host check without configuring the emulator.
function(multiplayer_replace variable before after)
    string(FIND "${${variable}}" "${before}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Pinned multiplayer source changed: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" result "${${variable}}")
    set(${variable} "${result}" PARENT_SCOPE)
endfunction()

file(READ "${MULTIPLAYER_SOURCE}/src/network/packet.h" packet_header)
multiplayer_replace(packet_header "#include <array>" "#include <array>\n#include <string>")
multiplayer_replace(packet_header [=[    out_data.resize(size);

    // Then extract the data
    for (std::size_t i = 0; i < out_data.size(); ++i) {
        T character;
        Read(character);
        out_data[i] = character;
    }]=] [=[    out_data.clear();
    // Every supported element consumes at least one byte. Do not allocate from an
    // unchecked count, including for vectors of variable-length strings.
    if (!CheckSize(size)) {
        return *this;
    }
    for (u32 i = 0; i < size; ++i) {
        T value{};
        if (!Read(value)) {
            out_data.clear();
            return *this;
        }
        out_data.push_back(std::move(value));
    }]=])
multiplayer_replace(packet_header "        T character;" "        T character{};")
multiplayer_replace(packet_header "#include <vector>" "#include <vector>\n#include <utility>")

file(READ "${MULTIPLAYER_SOURCE}/src/network/packet.cpp" packet_source)
multiplayer_replace(packet_source "if (out_data && CheckSize(size_in_bytes)) {"
    "if (out_data && CheckSize(size_in_bytes) && size_in_bytes != 0) {")
multiplayer_replace(packet_source "    read_pos += length;\n}"
    "    if (CheckSize(length)) {\n        read_pos += length;\n    }\n}")
multiplayer_replace(packet_source "read_pos + size <= data.size()"
    "read_pos <= data.size() && size <= data.size() - read_pos")

file(MAKE_DIRECTORY "${MULTIPLAYER_OUTPUT}/network")
file(WRITE "${MULTIPLAYER_OUTPUT}/network/packet.h.in" "${packet_header}")
configure_file("${MULTIPLAYER_OUTPUT}/network/packet.h.in"
    "${MULTIPLAYER_OUTPUT}/network/packet.h" COPYONLY)
file(WRITE "${MULTIPLAYER_OUTPUT}/packet.cpp.in" "${packet_source}")
configure_file("${MULTIPLAYER_OUTPUT}/packet.cpp.in" "${MULTIPLAYER_OUTPUT}/packet.cpp" COPYONLY)

file(READ "${MULTIPLAYER_SOURCE}/src/network/room_member.h" member_header)
multiplayer_replace(member_header "#include <functional>" "#include <functional>\n#include <stop_token>")
multiplayer_replace(member_header "const std::string& password = \"\", const std::string& token = \"\");"
    "const std::string& password = \"\", const std::string& token = \"\",\n              std::stop_token stop = {});")
file(WRITE "${MULTIPLAYER_OUTPUT}/network/room_member.h.in" "${member_header}")
configure_file("${MULTIPLAYER_OUTPUT}/network/room_member.h.in"
    "${MULTIPLAYER_OUTPUT}/network/room_member.h" COPYONLY)
file(READ "${MULTIPLAYER_SOURCE}/src/network/room_member.cpp" member_source)
multiplayer_replace(member_source "#include <atomic>"
    "#include <atomic>\n#include <chrono>\n#include \"multiplayer_validation.h\"\n#include \"multiplayer_send.h\"")
multiplayer_replace(member_source [=[                case ENET_EVENT_TYPE_RECEIVE:
                    switch (event.packet->data[0]) {]=] [=[                case ENET_EVENT_TYPE_RECEIVE:
                    if (!Eden::Multiplayer::ValidRoomPacket(
                            {event.packet->data, event.packet->dataLength})) {
                        enet_packet_destroy(event.packet);
                        SetState(State::Idle);
                        SetError(Error::UnknownError);
                        break;
                    }
                    switch (event.packet->data[0]) {]=])
multiplayer_replace(member_source [=[        ASSERT_MSG(room_member_impl->client != nullptr, "Could not create client");]=]
    [=[        if (!room_member_impl->client) {
            room_member_impl->SetState(State::Idle);
            room_member_impl->SetError(Error::CouldNotConnect);
            return;
        }
        room_member_impl->client->maximumPacketSize = Eden::Multiplayer::MaxRoomPacketBytes;
        room_member_impl->client->maximumWaitingData = Eden::Multiplayer::MaxRoomPacketBytes;]=])
multiplayer_replace(member_source "const std::string& password, const std::string& token) {"
    "const std::string& password, const std::string& token, std::stop_token stop) {")
multiplayer_replace(member_source "    enet_address_set_host(&address, server_addr);"
    [=[    if (stop.stop_requested()) {
        room_member_impl->SetState(State::Idle);
        return;
    }
    if (enet_address_set_host(&address, server_addr) != 0) {
        room_member_impl->SetState(State::Idle);
        room_member_impl->SetError(Error::CouldNotConnect);
        return;
    }]=])
multiplayer_replace(member_source
    "    int net = enet_host_service(room_member_impl->client, &event, ConnectionTimeoutMs);"
    [=[    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{ConnectionTimeoutMs};
    int net = 0;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        net = enet_host_service(room_member_impl->client, &event, 20);
        if (net != 0) break;
    }
    if (stop.stop_requested()) {
        if (net > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        enet_peer_reset(room_member_impl->server);
        room_member_impl->server = nullptr;
        room_member_impl->SetState(State::Idle);
        return;
    }]=])
multiplayer_replace(member_source [=[        enet_peer_disconnect(room_member_impl->server, 0);
        room_member_impl->SetState(State::Idle);]=] [=[        if (net > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        enet_peer_reset(room_member_impl->server);
        room_member_impl->server = nullptr;
        room_member_impl->SetState(State::Idle);]=])
multiplayer_replace(member_source [=[    while (enet_host_service(client, &event, ConnectionTimeoutMs) > 0) {]=]
    [=[    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::chrono::steady_clock::now() < deadline) {
        if (enet_host_service(client, &event, 20) <= 0) continue;]=])
multiplayer_replace(member_source [=[                        ASSERT_MSG(member_information.size() > 0,
                                "We have not yet received member information.");]=]
    [=[                        if (state != State::Joining || member_information.empty()) {
                            SetState(State::Idle);
                            SetError(Error::UnknownError);
                            break;
                        }]=])
multiplayer_replace(member_source [=[                    if (state == State::Joined || state == State::Moderator) {]=]
    [=[                    if (IsConnected()) {]=])
multiplayer_replace(member_source [=[    room_member_impl->client = nullptr;
}]=] [=[    room_member_impl->client = nullptr;
    room_member_impl->server = nullptr;
    std::lock_guard lock(room_member_impl->send_list_mutex);
    room_member_impl->send_list.clear();
    room_member_impl->send_list_budget.Reset();
    room_member_impl->send_failed = false;
}]=])
multiplayer_replace(member_source "    std::vector<Packet> send_list;"
    "    Eden::Multiplayer::RoomSendBudget send_list_budget, enet_send_budget;\n    std::atomic<bool> send_failed{false};\n    std::vector<Packet> send_list;")
multiplayer_replace(member_source "    send_list.push_back(std::move(packet));"
    [=[    if (!IsConnected() || send_failed) return;
    if (!send_list_budget.Add(packet.GetDataSize())) {
        send_failed = true;
        return;
    }
    send_list.push_back(std::move(packet));]=])
multiplayer_replace(member_source "            std::vector<Packet> packets;"
    [=[            if (send_failed.exchange(false)) {
                SetState(State::Idle);
                SetError(Error::LostConnection);
            }
            if (!IsConnected()) break;
            std::vector<Packet> packets;]=])
multiplayer_replace(member_source "                packets.swap(send_list);"
    "                packets.swap(send_list);\n                send_list_budget.Reset();")
multiplayer_replace(member_source [=[                ENetPacket* enetPacket = enet_packet_create(packet.GetData(), packet.GetDataSize(),
                                                            ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(server, 0, enetPacket);]=]
    [=[                const auto size = packet.GetDataSize();
                if (!enet_send_budget.Add(size)) {
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }
                ENetPacket* enetPacket = enet_packet_create(packet.GetData(), size, ENET_PACKET_FLAG_RELIABLE);
                if (!enetPacket) {
                    enet_send_budget.Release(size);
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }
                enetPacket->userData = &enet_send_budget;
                enetPacket->freeCallback = [](ENetPacket* sent) {
                    static_cast<Eden::Multiplayer::RoomSendBudget*>(sent->userData)->Release(sent->dataLength);
                };
                if (enet_peer_send(server, 0, enetPacket) != 0) {
                    enet_packet_destroy(enetPacket);
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }]=])
file(WRITE "${MULTIPLAYER_OUTPUT}/room_member.cpp.in" "${member_source}")
configure_file("${MULTIPLAYER_OUTPUT}/room_member.cpp.in"
    "${MULTIPLAYER_OUTPUT}/room_member.cpp" COPYONLY)

file(READ "${MULTIPLAYER_SOURCE}/src/core/hle/service/ldn/lan_discovery.cpp" ldn_source)
multiplayer_replace(ldn_source "#include \"core/hle/service/ldn/lan_discovery.h\""
    "#include \"core/hle/service/ldn/lan_discovery.h\"\n#include \"multiplayer_ldn.h\"")
multiplayer_replace(ldn_source "void LANDiscovery::ReceivePacket(const Network::LDNPacket& packet) {"
    "void LANDiscovery::ReceivePacket(const Network::LDNPacket& packet) {\n    if (!Eden::Multiplayer::ValidLdnPacket(packet)) return;")
multiplayer_replace(ldn_source "        connected_clients.push_back(packet.local_ip);"
    [=[        if (state != State::AccessPointCreated ||
            connected_clients.size() >= StationCountMax ||
            std::find(connected_clients.begin(), connected_clients.end(), packet.local_ip) !=
                connected_clients.end()) return;
        connected_clients.push_back(packet.local_ip);]=])
file(WRITE "${MULTIPLAYER_OUTPUT}/lan_discovery.cpp.in" "${ldn_source}")
configure_file("${MULTIPLAYER_OUTPUT}/lan_discovery.cpp.in"
    "${MULTIPLAYER_OUTPUT}/lan_discovery.cpp" COPYONLY)

file(READ "${MULTIPLAYER_SOURCE}/src/core/internal_network/socket_proxy.h" proxy_header)
multiplayer_replace(proxy_header "    std::queue<ProxyPacket> received_packets;"
    "    std::queue<ProxyPacket> received_packets;\n    std::size_t received_bytes = 0;")
file(MAKE_DIRECTORY "${MULTIPLAYER_OUTPUT}/core/internal_network")
file(WRITE "${MULTIPLAYER_OUTPUT}/core/internal_network/socket_proxy.h.in" "${proxy_header}")
configure_file("${MULTIPLAYER_OUTPUT}/core/internal_network/socket_proxy.h.in"
    "${MULTIPLAYER_OUTPUT}/core/internal_network/socket_proxy.h" COPYONLY)
file(READ "${MULTIPLAYER_SOURCE}/src/core/internal_network/socket_proxy.cpp" proxy_source)
multiplayer_replace(proxy_source "#include <chrono>"
    "#include <chrono>\n#include \"multiplayer_proxy.h\"")
multiplayer_replace(proxy_source [=[                                          const SockAddrIn* addr) {
    ASSERT(flags == 0);]=]
    [=[                                          const SockAddrIn* addr) {
    ASSERT(flags == 0);
    if (message.size() > Eden::Multiplayer::MaxProxyPayloadBytes) return {-1, Errno::MSGSIZE};]=])
multiplayer_replace(proxy_source [=[    decompressed.data = Common::Compression::DecompressDataZSTD(packet.data);

    std::lock_guard guard(packets_mutex);
    received_packets.push(decompressed);]=] [=[    if (!Eden::Multiplayer::DecodeProxyPayload(packet.data, decompressed.data)) return;

    std::lock_guard guard(packets_mutex);
    // A stalled guest must not accumulate unbounded data from a room peer.
    if (received_packets.size() >= Eden::Multiplayer::MaxProxyQueuePackets ||
        decompressed.data.size() > Eden::Multiplayer::MaxProxyQueueBytes - received_bytes) return;
    received_bytes += decompressed.data.size();
    received_packets.push(std::move(decompressed));]=])
multiplayer_replace(proxy_source "received_packets.pop();"
    "received_bytes -= packet.data.size();\n                received_packets.pop();")
multiplayer_replace(proxy_source [=[            std::vector<u8> numArray(packet.data.size() - max_length);
            std::copy(packet.data.begin() + max_length, packet.data.end(),
                      std::back_inserter(numArray));
            packet.data = numArray;]=] [=[            if (!peek) {
                packet.data.erase(packet.data.begin(), packet.data.begin() + max_length);
                received_bytes -= max_length;
            }]=])
file(WRITE "${MULTIPLAYER_OUTPUT}/socket_proxy.cpp.in" "${proxy_source}")
configure_file("${MULTIPLAYER_OUTPUT}/socket_proxy.cpp.in"
    "${MULTIPLAYER_OUTPUT}/socket_proxy.cpp" COPYONLY)

file(READ "${MULTIPLAYER_SOURCE}/src/core/hle/service/sockets/bsd.cpp" bsd_source)
multiplayer_replace(bsd_source "#include \"network/network.h\""
    "#include \"network/network.h\"\n#include \"multiplayer_session.h\"")
multiplayer_replace(bsd_source "    if (room_member && room_member->IsConnected()) {"
    [=[    const int room_mode = Eden::Multiplayer::guest_socket_mode.load();
    if (room_mode == 1 || (room_mode < 0 && room_member && room_member->IsConnected())) {]=])
file(WRITE "${MULTIPLAYER_OUTPUT}/bsd.cpp.in" "${bsd_source}")
configure_file("${MULTIPLAYER_OUTPUT}/bsd.cpp.in" "${MULTIPLAYER_OUTPUT}/bsd.cpp" COPYONLY)

if(TARGET network)
    get_target_property(packet_sources network SOURCES)
    list(REMOVE_ITEM packet_sources packet.cpp room_member.cpp)
    set_property(TARGET network PROPERTY SOURCES "${packet_sources}")
    target_sources(network PRIVATE "${MULTIPLAYER_OUTPUT}/packet.cpp"
        "${MULTIPLAYER_OUTPUT}/room_member.cpp")
    target_include_directories(network BEFORE PUBLIC "${MULTIPLAYER_OUTPUT}")
    target_include_directories(network PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
if(TARGET core)
    get_target_property(ldn_sources core SOURCES)
    list(REMOVE_ITEM ldn_sources hle/service/ldn/lan_discovery.cpp internal_network/socket_proxy.cpp hle/service/sockets/bsd.cpp)
    set_property(TARGET core PROPERTY SOURCES "${ldn_sources}")
    target_sources(core PRIVATE "${MULTIPLAYER_OUTPUT}/lan_discovery.cpp"
        "${MULTIPLAYER_OUTPUT}/socket_proxy.cpp" "${MULTIPLAYER_OUTPUT}/bsd.cpp")
    target_include_directories(core BEFORE PRIVATE "${MULTIPLAYER_OUTPUT}")
    target_include_directories(core PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(core PRIVATE zstd::zstd)
endif()
