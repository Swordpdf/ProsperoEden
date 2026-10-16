// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

namespace pe::ui::launcher_updates {
using Json = nlohmann::json;

struct Host {
    std::string version;
    std::string contract = "launcher-v1.0";
    int api_major = 1, api_minor = 1;
    std::set<std::string> capabilities;
};

// The transport must enforce HTTPS, including redirects, and stop on a false sink result.
using Sink = std::function<bool(const void*, std::size_t)>;
using Fetch = std::function<void(const std::string&, std::size_t, const Sink&, const std::atomic<bool>&)>;
Fetch https_transport();
bool valid_id(const std::string& id);
bool https_url(const std::string& url);
bool newer(const std::string& candidate, const std::string& installed);
Json choose_release(const Json& feed, const Json& manifest, const Host& host);

class Manager {
public:
    Manager(std::string data_root, Host host, Fetch fetch = https_transport());
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Json status(const std::string& id) const;
    bool check(const std::string& id, bool automatic = false);
    bool download();
    bool rollback(const std::string& id);
    // Call only after the host has unloaded the pack and stopped its consumers.
    bool commit();
    void cancel();
    bool busy() const;
    bool replacing() const;
    bool automatic() const;
    bool set_automatic(bool enabled);
    void poll();
    static void recover(const std::string& data_root);
private:
    struct State;
    std::shared_ptr<State> state_;
    std::string root_;
    Host host_;
    Fetch fetch_;
    Json settings_;
    bool save_settings();
    bool start(const std::string& id, const std::string& phase,
               std::function<void(State&)> work);
};
} // namespace pe::ui::launcher_updates
