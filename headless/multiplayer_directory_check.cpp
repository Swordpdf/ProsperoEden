// SPDX-License-Identifier: GPL-3.0-or-later
#include "multiplayer_directory.h"
#include "remote/http.h"
#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    using namespace Eden::Multiplayer;
    using nlohmann::json;
    json room{{"name", "Example"}, {"preferredGameName", "Game"}, {"address", "192.0.2.1"},
              {"port", 24872}, {"maxPlayers", 8}, {"netVersion", 1}, {"hasPassword", false}};
    auto parse = [&](const json& r) { return ParseRoomDirectory(json{{"rooms", json::array({r})}}.dump(), 1); };
    auto good = parse(room);
    assert(good.error.empty() && good.rooms.size() == 1 && good.rooms[0].compatible);
    auto wrong = room; wrong["netVersion"] = 4;
    assert(!parse(wrong).rooms[0].compatible);
    wrong = room; wrong["port"] = 65536;
    assert(parse(wrong).rooms.empty());
    wrong["port"] = -1; assert(parse(wrong).rooms.empty());
    wrong["port"] = 1.5; assert(parse(wrong).rooms.empty());
    wrong = room; wrong["address"] = "::1"; assert(parse(wrong).rooms.empty());
    wrong = room; wrong["name"] = std::string(513, 'a'); assert(parse(wrong).rooms.empty());
    wrong = room; wrong["players"] = "bad"; assert(parse(wrong).rooms.empty());
    wrong = room; wrong["hasPassword"] = "false"; assert(parse(wrong).rooms.empty());
    assert(!ParseRoomDirectory("{", 1).error.empty());
    assert(!ParseRoomDirectory("{\"rooms\":null}", 1).error.empty());
    assert(ParseRoomDirectory("{\"rooms\":[]}", 1).error.empty());
    assert(!ParseRoomDirectory(std::string(MaxDirectoryBytes + 1, ' '), 1).error.empty());
    auto mixed = ParseRoomDirectory(json{{"rooms", json::array({wrong, room})}}.dump(), 1);
    assert(mixed.rooms.size() == 1 && mixed.rejected == 1);
    wrong = room; wrong["name"] = "line\nbreak";
    assert(parse(wrong).rooms[0].name == "line break");
    std::string deep(1000, '['); deep += std::string(1000, ']');
    assert(!ParseRoomDirectory(deep, 1).error.empty());
    std::atomic<bool> stop{true}; assert(FetchRoomDirectory(1, stop).rooms.empty());
    remote_http_request req{};
    req.url = "http://127.0.0.1:1"; req.https_only = 1;
    req.sink = [](void*, const void*, std::size_t) { return 1; };
    remote_http_result response{};
    assert(remote_http_get(&req, &response) != 0 && response.curl_code == 1);
    if (argc > 1) {
        std::string body;
        req = {}; req.url = argv[1]; req.api_version = "1"; req.timeout_ms = 3000; req.user = &body;
        req.sink = [](void* user, const void* data, std::size_t size) {
            static_cast<std::string*>(user)->append(static_cast<const char*>(data), size); return 1;
        };
        assert(remote_http_get(&req, &response) == 0 && response.status == 200 && body == "1");
    }
    std::cout << "Public directory validation and HTTP checks PASS\n";
}
