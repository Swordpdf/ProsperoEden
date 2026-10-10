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

file(READ "${MULTIPLAYER_SOURCE}/src/network/room_member.cpp" member_source)
multiplayer_replace(member_source "#include <atomic>"
    "#include <atomic>\n#include \"multiplayer_validation.h\"")
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
    [=[        ASSERT_MSG(room_member_impl->client != nullptr, "Could not create client");
        room_member_impl->client->maximumPacketSize = Eden::Multiplayer::MaxRoomPacketBytes;
        room_member_impl->client->maximumWaitingData = Eden::Multiplayer::MaxRoomPacketBytes;]=])
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
    list(REMOVE_ITEM ldn_sources hle/service/ldn/lan_discovery.cpp internal_network/socket_proxy.cpp)
    set_property(TARGET core PROPERTY SOURCES "${ldn_sources}")
    target_sources(core PRIVATE "${MULTIPLAYER_OUTPUT}/lan_discovery.cpp"
        "${MULTIPLAYER_OUTPUT}/socket_proxy.cpp")
    target_include_directories(core BEFORE PRIVATE "${MULTIPLAYER_OUTPUT}")
    target_include_directories(core PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(core PRIVATE zstd::zstd)
endif()
