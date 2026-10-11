// SPDX-License-Identifier: GPL-3.0-or-later
#include "multiplayer_directory.h"
#include "multiplayer_logging.h"
#include "remote/http.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace Eden::Multiplayer {
namespace {
std::string Text(const nlohmann::json& row, const char* key, std::size_t limit) {
    auto value = row.at(key).get<std::string>();
    if (value.size() > limit || value.find('\0') != std::string::npos)
        throw std::runtime_error("invalid room text");
    // Directory text is untrusted; keep it on its own row.
    for (char& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return value;
}
// A directory entry must not point a console at its own network or at itself: loopback,
// private, carrier-grade NAT, link-local, multicast, reserved and broadcast are refused.
bool PublicAddress(const in_addr& address) {
    const std::uint32_t ip = ntohl(address.s_addr);
    const auto in = [ip](std::uint32_t network, int bits) { return (ip >> (32 - bits)) == (network >> (32 - bits)); };
    return !(in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8) ||
             in(0xA9FE0000, 16) || in(0xAC100000, 12) || in(0xC0A80000, 16) || ip >= 0xE0000000);
}
// The JSON parser refuses the whole document for one malformed byte; a single room's bad name
// would hide every other room. Malformed UTF-8 becomes '?' before parsing.
std::string WellFormed(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[i + k]); };
        const unsigned char lead = byte(0);
        const std::size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 :
            lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        bool valid = length != 0 && length <= text.size() - i;
        for (std::size_t k = 1; valid && k < length; ++k) valid = (byte(k) & 0xC0) == 0x80;
        // No overlong forms, surrogates or values past U+10FFFF.
        if (valid && length == 3) valid = !(lead == 0xE0 && byte(1) < 0xA0) && !(lead == 0xED && byte(1) > 0x9F);
        if (valid && length == 4) valid = !(lead == 0xF0 && byte(1) < 0x90) && !(lead == 0xF4 && byte(1) > 0x8F);
        if (valid) result.append(text, i, length);
        else result += '?';
        i += valid ? length : 1;
    }
    return result;
}
unsigned Number(const nlohmann::json& value, unsigned maximum) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > maximum)
        throw std::runtime_error("invalid room number");
    return value.get<unsigned>();
}
}

RoomDirectory ParseRoomDirectory(std::string_view body, unsigned protocol) {
    RoomDirectory result;
    if (body.size() > MaxDirectoryBytes) return {{}, "The public room list is too large."};
    try {
        // Limit nesting before the parser can exhaust the console stack.
        auto document = nlohmann::json::parse(WellFormed(body), [](int depth, auto, auto&) {
            if (depth > 16) throw std::runtime_error("room list nesting limit");
            return true;
        });
        const auto& rows = document.at("rooms");
        if (!rows.is_array()) throw std::runtime_error("invalid room list");
        if (rows.size() > 2048) return {{}, "The public room list is too large."};
        for (const auto& row : rows) {
            try {
                PublicRoom room;
                room.name = Text(row, "name", 512);
                room.game = Text(row, "preferredGameName", 512);
                room.address = Text(row, "address", 15);
                in_addr address{};
                if (room.name.empty() || inet_pton(AF_INET, room.address.c_str(), &address) != 1 ||
                    !PublicAddress(address))
                    throw std::runtime_error("unsupported address");
                room.port = static_cast<std::uint16_t>(Number(row.at("port"), 65535));
                room.capacity = Number(row.at("maxPlayers"), 65535);
                if (!room.port || !room.capacity) throw std::runtime_error("invalid room limits");
                room.password = row.at("hasPassword").get<bool>();
                room.compatible = Number(row.at("netVersion"), UINT32_MAX) == protocol;
                if (row.contains("players")) {
                    if (!row.at("players").is_array() || row.at("players").size() > 65535)
                        throw std::runtime_error("invalid player list");
                    room.players = static_cast<unsigned>(row.at("players").size());
                }
                result.rooms.push_back(std::move(room));
            } catch (const std::exception&) { ++result.rejected; }
        }
        if (!rows.empty() && result.rooms.empty()) result.error = "No usable rooms were returned.";
        std::stable_sort(result.rooms.begin(), result.rooms.end(), [](const auto& a, const auto& b) {
            if (a.compatible != b.compatible) return a.compatible > b.compatible;
            if (a.players != b.players) return a.players > b.players; // where people are, first
            return a.name < b.name;
        });
    } catch (const std::exception&) { result = {{}, "The public room list could not be read."}; }
    return result;
}

RoomDirectory FetchRoomDirectory(unsigned protocol, const std::atomic<bool>& stop) {
    struct Download {
        std::string body;
        const std::atomic<bool>& stop;
        bool oversized{};
    } download{{}, stop};
    if (stop.load()) return {};
    remote_http_request request{};
    request.url = "https://api.ynet-fun.xyz/lobby";
    request.api_version = "1";
    request.https_only = 1;
    request.timeout_ms = 15000;
    request.user = &download;
    request.sink = [](void* user, const void* data, std::size_t size) -> int {
        auto& d = *static_cast<Download*>(user);
        if (size > MaxDirectoryBytes - d.body.size()) { d.oversized = true; return 0; }
        try { d.body.append(static_cast<const char*>(data), size); }
        catch (...) { return 0; }
        return 1;
    };
    request.stop = [](void* user) { return static_cast<Download*>(user)->stop.load() ? 1 : 0; };
    remote_http_result response{};
    Debug("Public room directory request started");
    const auto start = std::chrono::steady_clock::now();
    const int rc = remote_http_get(&request, &response);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "Public directory: http=%d curl=%d bytes=%zu elapsed_ms=%lld cancelled=%d",
                  response.status, response.curl_code, download.body.size(),
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count()), stop.load());
    Debug(detail);
    if (stop.load()) return {};
    if (download.oversized) return {{}, "The public room list is too large."};
    if (rc != 0) return {{}, "Could not download public rooms. Check your connection and try Refresh."};
    if (response.status != 200) {
        return {{}, "The public room service returned HTTP " + std::to_string(response.status) + ". Try Refresh later."};
    }
    auto result = ParseRoomDirectory(download.body, protocol);
    std::snprintf(detail, sizeof(detail), "Public directory: rooms=%zu rejected=%u", result.rooms.size(), result.rejected);
    Debug(detail);
    return result;
}
}
