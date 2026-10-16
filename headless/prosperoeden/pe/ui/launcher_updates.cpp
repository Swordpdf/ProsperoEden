// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/launcher_updates.hpp"
#include "pe/ui/runtime_filesystem.hpp"
#include "../../../remote/http.h"
#include <miniz.h>
#include <openssl/evp.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

namespace pe::ui::launcher_updates {
namespace {
namespace fs = runtime_fs;
constexpr std::size_t feed_limit = 256 * 1024, archive_limit = 128 * 1024 * 1024;
constexpr std::uint64_t expanded_limit = 256 * 1024 * 1024, file_limit = 64 * 1024 * 1024;
std::mutex worker_mutex;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void stopped(const std::atomic<bool>& cancel) { require(!cancel.load(), "Cancelled"); }
bool exists(const std::string& path) {
    // lstat is unavailable inside the PS5 application sandbox.
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) { close(fd); return true; }
    if (errno == ENOENT) return false;
    throw std::system_error(errno, std::generic_category(), "Cannot inspect " + path);
}
void directories(const std::string& path) {
    for (std::size_t i = 1; i <= path.size(); ++i) if (i == path.size() || path[i] == '/') {
        const auto part = path.substr(0, i);
        require(mkdir(part.c_str(), 0755) == 0 || errno == EEXIST, "Cannot create " + part);
        fs::check(part, fs::Kind::directory);
    }
}
void sync_directory(const std::string& path) {
    fs::Descriptor dir(path, fs::Kind::directory);
    // Some console mounts do not implement directory fsync. File fsync remains mandatory.
    if (fsync(dir.get()) != 0 && errno != EINVAL && errno != ENOTSUP)
        throw std::runtime_error("Cannot synchronize " + path);
}
std::string parent(const std::string& path) { return path.substr(0, path.find_last_of('/')); }
void move(const std::string& from, const std::string& to) {
    require(!exists(to), "Replacement destination already exists");
    require(rename(from.c_str(), to.c_str()) == 0, "Cannot replace launcher files");
    sync_directory(parent(from));
    if (parent(from) != parent(to)) sync_directory(parent(to));
}
std::string read(const std::string& path, std::size_t limit) {
    fs::Descriptor file(path, fs::Kind::file);
    struct stat info{};
    require(fstat(file.get(), &info) == 0 && info.st_size >= 0 && std::uint64_t(info.st_size) <= limit, "File exceeds size limit");
    std::string result(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t at = 0;
    while (at < result.size()) {
        const auto n = ::read(file.get(), result.data() + at, result.size() - at);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "Cannot read " + path); at += static_cast<std::size_t>(n);
    }
    return result;
}
Json json(const std::string& path, std::size_t limit = 65536) { return Json::parse(read(path, limit)); }
struct Output {
    int fd = -1;
    explicit Output(const std::string& path) {
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        require(fd >= 0, "Cannot create " + path);
    }
    ~Output() { if (fd >= 0) close(fd); }
    void write(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const char*>(data);
        while (size) {
            const auto n = ::write(fd, bytes, size);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "Cannot write launcher update"); bytes += n; size -= static_cast<std::size_t>(n);
        }
    }
    void finish() { require(fsync(fd) == 0, "Cannot synchronize launcher update"); }
};
void write_json(const std::string& path, const Json& value) {
    directories(parent(path)); const auto tmp = path + ".tmp";
    if (exists(tmp)) require(unlink(tmp.c_str()) == 0, "Cannot remove temporary metadata");
    { Output file(tmp); const auto text = value.dump(2) + "\n"; file.write(text.data(), text.size()); file.finish(); }
    require(rename(tmp.c_str(), path.c_str()) == 0, "Cannot save updater metadata"); sync_directory(parent(path));
}
void remove_tree(const std::string& path, unsigned depth = 0, unsigned* count = nullptr) {
    unsigned local = 0; if (!count) count = &local;
    require(depth <= 24 && ++*count <= 8192, "Launcher tree exceeds cleanup limits");
    if (!exists(path)) return;
    fs::Descriptor item(path, fs::Kind::resource); struct stat info{};
    require(fstat(item.get(), &info) == 0, "Cannot inspect updater resource");
    if (S_ISREG(info.st_mode)) require(unlink(path.c_str()) == 0, "Cannot remove updater file");
    else {
        for (const auto& name : fs::entries(path)) {
            require(fs::relative_path(name) && name.find('/') == name.npos, "Invalid updater entry");
            remove_tree(path + "/" + name, depth + 1, count);
        }
        require(rmdir(path.c_str()) == 0, "Cannot remove updater directory");
    }
}
struct StagingGuard {
    std::string path;
    bool keep{};
    ~StagingGuard() {
        if (!keep) try { remove_tree(path); }
        catch (const std::exception& e) { std::fprintf(stderr, "PE_LAUNCHER_UPDATE_CLEANUP %s\n", e.what()); }
    }
};
void sync_tree(const std::string& path, unsigned depth = 0) {
    require(depth <= 24, "Directory tree exceeds depth limit");
    for (const auto& name : fs::entries(path)) {
        const auto child = path + "/" + name;
        fs::Descriptor item(child, fs::Kind::resource); struct stat info{};
        require(fstat(item.get(), &info) == 0, "Cannot inspect staged resource");
        if (S_ISDIR(info.st_mode)) sync_tree(child, depth + 1);
    }
    sync_directory(path);
}
std::array<std::uint32_t, 3> version(const std::string& text) {
    std::array<std::uint32_t, 3> result{}; std::size_t at = 0;
    for (int i = 0; i != 3; ++i) {
        const auto end = i == 2 ? text.size() : text.find('.', at);
        require(end != text.npos && end > at && end - at <= 9, "Expected a three-part numeric version");
        auto parsed = std::from_chars(text.data() + at, text.data() + end, result[i]);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + end, "Invalid version"); at = end + 1;
    }
    return result;
}
bool supports(const Json& value, const Host& host, int state_schema) {
    if (value.at("host_contract") != host.contract || value.at("api").at("major") != host.api_major) return false;
    const auto minor_value = value.at("api").value("min_minor", Json(0));
    if (!minor_value.is_number_integer() || minor_value < 0 || minor_value > host.api_minor) return false;
    const int minor = value.at("api").value("min_minor", 0);
    const auto schema = value.value("state_schema", Json(1));
    if (minor < 0 || minor > host.api_minor || !schema.is_number_integer() || schema < 1 || schema > 2147483647 || schema != state_schema) return false;
    if (value.contains("min_app") && version(host.version) < version(value.at("min_app"))) return false;
    const auto caps = value.value("required_capabilities", Json::array());
    if (!caps.is_array() || caps.size() > 64) return false;
    for (const auto& cap : caps) if (!host.capabilities.contains(cap.get<std::string>())) return false;
    return true;
}
void release_valid(const Json& release) {
    version(release.at("version"));
    require(https_url(release.at("url")), "Invalid HTTPS archive URL");
    const auto hash = release.at("sha256").get<std::string>();
    require(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == hash.npos, "Invalid SHA-256");
    require(release.at("bytes").is_number_unsigned() || release.at("bytes").is_number_integer(), "Invalid archive size");
    const auto bytes = release.at("bytes").get<std::int64_t>();
    require(bytes > 0 && bytes <= static_cast<std::int64_t>(archive_limit), "Archive exceeds download limit");
    require(release.value("notes", std::string{}).size() <= 8192, "Release notes exceed limit");
}
std::string base(const std::string& root) { return root + "/launcher-updates"; }
std::string live(const std::string& root, const std::string& id) { return root + "/launchers/" + id; }
std::string work(const std::string& root, const std::string& id) { return base(root) + "/work/" + id; }
std::string backup(const std::string& root, const std::string& id) { return base(root) + "/backups/" + id; }
std::string journal(const std::string& root, const std::string& id) { return base(root) + "/transactions/" + id + ".json"; }
Json manifest(const std::string& root, const std::string& id) {
    require(valid_id(id), "Invalid launcher ID");
    return json(fs::resource_path(root + "/launchers", id + "/manifest.json"));
}
void recover_one(const std::string& root, const std::string& id) {
    const auto w = work(root, id), b = backup(root, id), j = journal(root, id);
    const auto record = json(j);
    require(record.at("id") == id && record.at("schema") == 1, "Invalid launcher recovery record");
    const auto phase = record.at("phase").get<std::string>();
    require(phase == "prepared" || phase == "committed", "Unknown launcher recovery phase");
    if (phase == "prepared") {
        if (exists(w + "/old")) { remove_tree(live(root, id)); move(w + "/old", live(root, id)); }
    } else if (exists(w + "/old")) {
        if (exists(b)) { require(!exists(w + "/prior"), "Ambiguous backup recovery"); move(b, w + "/prior"); }
        move(w + "/old", b);
    }
    remove_tree(w);
    require(unlink(j.c_str()) == 0, "Cannot finish launcher recovery"); sync_directory(parent(j));
}
void replace(const std::string& root, const std::string& id) {
    const auto w = work(root, id), j = journal(root, id);
    fs::check(w + "/new", fs::Kind::directory); fs::check(live(root, id), fs::Kind::directory);
    directories(base(root) + "/backups");
    write_json(j, {{"schema", 1}, {"id", id}, {"phase", "prepared"}});
    move(live(root, id), w + "/old"); move(w + "/new", live(root, id));
    write_json(j, {{"schema", 1}, {"id", id}, {"phase", "committed"}});
    recover_one(root, id);
}
struct Zip {
    mz_zip_archive zip{};
    explicit Zip(const std::string& path) { require(mz_zip_reader_init_file(&zip, path.c_str(), 0), "Invalid ZIP archive"); }
    ~Zip() { mz_zip_reader_end(&zip); }
};
void unpack(const std::string& path, const std::string& destination, const std::string& id, const std::atomic<bool>& cancel) {
    Zip reader(path); auto& zip = reader.zip;
    const auto count = mz_zip_reader_get_num_files(&zip);
    require(count > 0 && count <= 4096, "Archive exceeds entry limit");
    std::set<std::string> names; std::uint64_t total = 0;
    struct Entry { mz_uint index; std::string name; bool directory; std::uint64_t size; };
    std::vector<Entry> entries;
    for (mz_uint i = 0; i < count; ++i) {
        stopped(cancel); mz_zip_archive_file_stat stat{};
        require(mz_zip_reader_file_stat(&zip, i, &stat), "Cannot read ZIP entry");
        std::string name(stat.m_filename);
        require(mz_zip_reader_get_filename(&zip, i, nullptr, 0) == name.size() + 1, "ZIP entry name exceeds limit or contains NUL");
        const bool directory = mz_zip_reader_is_file_a_directory(&zip, i);
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        require(fs::relative_path(name) && std::count(name.begin(), name.end(), '/') <= 20, "Invalid archive path");
        require(name == id || name.starts_with(id + "/"), "ZIP must contain only the launcher ID directory");
        require(names.insert(name).second, "Duplicate archive path");
        const auto mode = (stat.m_external_attr >> 16) & S_IFMT;
        require(mode == 0 || mode == (directory ? S_IFDIR : S_IFREG), "Archive contains a link or special file");
        require(!stat.m_is_encrypted && stat.m_is_supported && (stat.m_method == 0 || stat.m_method == 8), "Unsupported ZIP encoding");
        require(stat.m_uncomp_size <= file_limit && total <= expanded_limit - stat.m_uncomp_size, "Archive exceeds expanded size limit");
        total += stat.m_uncomp_size;
        entries.push_back({i, name, directory, stat.m_uncomp_size});
    }
    require(names.contains(id + "/manifest.json"), "Archive is missing manifest.json");
    directories(destination);
    for (const auto& entry : entries) {
        stopped(cancel); if (entry.name == id) { require(entry.directory, "Invalid launcher root"); continue; }
        const auto target = destination + "/" + entry.name.substr(id.size() + 1);
        if (entry.directory) { directories(target); continue; }
        directories(parent(target)); Output file(target);
        struct Context { Output& file; const std::atomic<bool>& cancel; std::uint64_t size{}, expected; std::string error; } context{file, cancel, 0, entry.size, {}};
        auto sink = [](void* raw, mz_uint64 offset, const void* bytes, std::size_t size) -> std::size_t {
            auto& c = *static_cast<Context*>(raw);
            try {
                stopped(c.cancel); require(offset == c.size && size <= c.expected - c.size, "Invalid ZIP output length");
                c.file.write(bytes, size); c.size += size; return size;
            } catch (const std::exception& e) { c.error = e.what(); return 0; }
        };
        const bool ok = mz_zip_reader_extract_to_callback(&zip, entry.index, sink, &context, 0);
        require(ok && context.size == entry.size, context.error.empty() ? "ZIP CRC or extraction failure" : context.error);
        file.finish();
    }
    // Include implicit parent directories, which ZIP writers commonly omit.
    sync_tree(destination);
}
void copy_tree(const std::string& from, const std::string& to, const std::atomic<bool>& cancel,
               std::uint64_t& total, unsigned& count, unsigned depth = 0) {
    require(depth <= 20 && ++count <= 4096, "Backup exceeds resource limits"); stopped(cancel);
    fs::Descriptor item(from, fs::Kind::resource); struct stat info{};
    require(fstat(item.get(), &info) == 0, "Cannot inspect backup");
    if (S_ISDIR(info.st_mode)) {
        directories(to);
        for (const auto& name : fs::entries(from)) {
            require(fs::relative_path(name) && name.find('/') == name.npos, "Invalid backup path");
            copy_tree(from + "/" + name, to + "/" + name, cancel, total, count, depth + 1);
        }
        sync_directory(to);
    } else {
        require(info.st_size >= 0 && std::uint64_t(info.st_size) <= file_limit && total <= expanded_limit - info.st_size, "Backup exceeds size limit");
        total += info.st_size; Output output(to); std::array<char, 65536> bytes{};
        for (;;) {
            stopped(cancel); const auto n = ::read(item.get(), bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) continue;
            require(n >= 0, "Cannot read backup"); if (!n) break; output.write(bytes.data(), n);
        }
        output.finish();
    }
}
void* lua_memory(void* raw, void* ptr, std::size_t old, std::size_t size) {
    auto& used = *static_cast<std::size_t*>(raw); if (!ptr) old = 0;
    if (!size) { std::free(ptr); used -= old; return nullptr; }
    if (size > 16 * 1024 * 1024 || used - old > 16 * 1024 * 1024 - size) return nullptr;
    auto* next = std::realloc(ptr, size); if (next) used = used - old + size; return next;
}
void validate_pack(const std::string& folder, const Json& expected, const Host& host, const Json& installed) {
    const auto m = json(fs::resource_path(folder, "manifest.json"));
    require(m.at("schema") == 1 && m.at("id") == installed.at("id") && supports(m, host, installed.value("state_schema", 1)), "Downloaded launcher is incompatible");
    if (!expected.is_null()) {
        for (const auto* key : {"version", "host_contract", "api"}) require(m.at(key) == expected.at(key), "Downloaded manifest differs from update feed");
        require(m.value("required_capabilities", Json::array()) == expected.value("required_capabilities", Json::array()), "Downloaded capabilities differ from update feed");
        require(m.at("updates").at("feed") == installed.at("updates").at("feed"), "Downloaded launcher changed its update source");
    }
    require(m.at("assets").is_object() && m.at("assets").size() <= 32, "Invalid image resources");
    for (const auto& asset : m.at("assets")) fs::check(fs::resource_path(folder, asset.get<std::string>()), fs::Kind::file);
    fs::check(fs::resource_path(folder, m.at("font")), fs::Kind::file);
    if (m.contains("loading_scene")) json(fs::resource_path(folder, m.at("loading_scene")));
    if (!m.value("sounds", std::string{}).empty()) fs::check(fs::resource_path(folder, m.at("sounds")), fs::Kind::directory);
    const auto script = read(fs::resource_path(folder, m.at("entry")), 2 * 1024 * 1024);
    std::size_t used = 0; lua_State* lua = lua_newstate(lua_memory, &used);
    require(lua != nullptr, "Cannot allocate script validator");
    const int rc = luaL_loadbufferx(lua, script.data(), script.size(), "launcher-update", "t");
    const std::string error = rc == LUA_OK ? "" : (lua_tostring(lua, -1) ? lua_tostring(lua, -1) : "Invalid Lua script");
    lua_close(lua); require(rc == LUA_OK, error);
}
} // namespace

bool valid_id(const std::string& id) {
    return id != "builtin" && !id.empty() && id.size() <= 48 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == id.npos;
}
bool https_url(const std::string& url) {
    if (!url.starts_with("https://") || url.size() > 2048) return false;
    const auto end = url.find('/', 8); const auto authority = url.substr(8, end == url.npos ? url.size() - 8 : end - 8);
    if (authority.empty() || authority.find_first_of("@?#\\") != authority.npos) return false;
    for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\' || c == '#') return false;
    return true;
}
bool newer(const std::string& a, const std::string& b) { return version(a) > version(b); }
Json choose_release(const Json& feed, const Json& m, const Host& host) {
    version(m.at("version"));
    const auto state = m.value("state_schema", Json(1));
    require(state.is_number_integer() && state >= 1 && state <= 2147483647, "Invalid installed state schema");
    require(feed.at("schema") == 1 && feed.at("id") == m.at("id"), "Update feed does not match this launcher");
    const auto& releases = feed.at("releases");
    require(releases.is_array() && releases.size() <= 64, "Invalid release list"); Json best;
    std::set<std::array<std::uint32_t, 3>> versions;
    for (const auto& release : releases) {
        release_valid(release);
        require(versions.insert(version(release.at("version"))).second, "Duplicate release version");
        if (supports(release, host, state.get<int>()) && newer(release.at("version"), m.at("version")) &&
            (best.is_null() || newer(release.at("version"), best.at("version")))) best = release;
    }
    return best;
}
Fetch https_transport() {
    return [](const std::string& url, std::size_t limit, const Sink& sink, const std::atomic<bool>& cancel) {
        require(https_url(url), "Updates require HTTPS");
        struct Context { const Sink& sink; const std::atomic<bool>& cancel; std::size_t size{}, limit; bool rejected{}; } context{sink, cancel, 0, limit, false};
        remote_http_request request{}; request.url = url.c_str(); request.https_only = 1; request.timeout_ms = 120000;
        request.user = &context;
        request.begin = [](void* raw, int status, std::uint64_t size) {
            auto& c = *static_cast<Context*>(raw); return !c.cancel && status == 200 && (size == 0 || size <= c.limit) ? 1 : 0;
        };
        request.stop = [](void* raw) { return static_cast<Context*>(raw)->cancel.load() ? 1 : 0; };
        request.sink = [](void* raw, const void* bytes, std::size_t size) {
            auto& c = *static_cast<Context*>(raw);
            if (c.cancel || size > c.limit - c.size) { c.rejected = true; return 0; }
            try { c.size += size; return c.sink(bytes, size) ? 1 : 0; }
            catch (...) { c.rejected = true; return 0; }
        };
        remote_http_result result{}; const int ok = remote_http_get(&request, &result);
        stopped(cancel);
        require(ok == 0 && result.status == 200 && !context.rejected, result.error[0] ? result.error : "Update download failed or exceeded its limit");
    };
}
struct Manager::State {
    std::mutex mutex;
    std::atomic<bool> cancel{false}, running{false};
    Json status = {{"phase", "idle"}, {"id", ""}};
};
Manager::Manager(std::string root, Host host, Fetch fetch) : state_(std::make_shared<State>()), root_(std::move(root)), host_(std::move(host)), fetch_(std::move(fetch)) {
    settings_ = {{"schema", 1}, {"automatic_checks", true}, {"checked", Json::object()}};
    try { const auto saved = json(root_ + "/config/launcher-updates.json");
        if (saved.is_object() && saved.value("schema", 0) == 1) {
            if (saved.contains("automatic_checks") && saved["automatic_checks"].is_boolean()) settings_["automatic_checks"] = saved["automatic_checks"];
            if (saved.contains("checked") && saved["checked"].is_object() && saved["checked"].size() <= 64) {
                const auto now = std::time(nullptr);
                for (auto it = saved["checked"].begin(); it != saved["checked"].end(); ++it)
                    if (valid_id(it.key()) && it.value().is_number_integer() && it.value() >= 0 && it.value() <= now)
                        settings_["checked"][it.key()] = it.value();
            }
        }
    } catch (...) {}
    try { recover(root_); } catch (const std::exception& e) { state_->status = {{"phase", "failed"}, {"id", ""}, {"error", e.what()}}; }
}
Manager::~Manager() { cancel(); }
void Manager::recover(const std::string& root) {
    if (!exists(base(root) + "/transactions")) return;
    fs::check(base(root), fs::Kind::directory);
    for (const auto& name : fs::entries(base(root) + "/transactions")) {
        if (!name.ends_with(".json")) continue;
        const auto id = name.substr(0, name.size() - 5); require(valid_id(id), "Invalid recovery ID"); recover_one(root, id);
    }
}
bool Manager::start(const std::string& id, const std::string& phase, std::function<void(State&)> fn) {
    if (busy() || !valid_id(id)) return false;
    // Detached workers capture only owned data. Leaving the menu never waits for network I/O.
    auto state = std::make_shared<State>(); state->running = true; state->status = {{"id", id}, {"phase", phase}}; state_ = state;
    try { std::thread([state, fn = std::move(fn)] {
        std::unique_lock serial(worker_mutex);
        try { stopped(state->cancel); fn(*state); }
        catch (const std::exception& e) { std::lock_guard lock(state->mutex); state->status["phase"] = state->cancel ? "cancelled" : "failed"; state->status["error"] = e.what(); }
        state->running = false;
    }).detach(); }
    catch (const std::exception& e) { state->running = false; state->status["phase"] = "failed"; state->status["error"] = e.what(); return false; }
    return true;
}
Json Manager::status(const std::string& id) const {
    std::lock_guard lock(state_->mutex); auto result = state_->status;
    if (!id.empty() && result.value("id", "") != id) result = {{"phase", "idle"}, {"id", id}};
    result["automatic_checks"] = automatic();
    return result;
}
bool Manager::busy() const { return state_->running.load(); }
bool Manager::replacing() const { std::lock_guard lock(state_->mutex); return state_->status.value("phase", "") == "installing" && busy(); }
bool Manager::automatic() const { return settings_.value("automatic_checks", true); }
bool Manager::save_settings() { try { write_json(root_ + "/config/launcher-updates.json", settings_); return true; } catch (...) { return false; } }
bool Manager::set_automatic(bool enabled) { const auto previous = settings_; settings_["automatic_checks"] = enabled; if (save_settings()) return true; settings_ = previous; return false; }
void Manager::poll() {
    if (busy()) return;
    std::lock_guard lock(state_->mutex);
    if (state_->status.value("record_check", false)) {
        settings_["checked"][state_->status.at("id").get<std::string>()] = state_->status.at("checked_at");
        save_settings(); state_->status.erase("record_check");
    }
}
bool Manager::check(const std::string& id, bool automatic_check) {
    if (!valid_id(id) || busy()) return false;
    const auto now = std::time(nullptr);
    if (automatic_check && (!automatic() || now - settings_["checked"].value(id, std::int64_t(0)) < 6 * 60 * 60)) return false;
    const auto root = root_; const auto host = host_; const auto fetch = fetch_;
    return start(id, "checking", [=](State& s) {
        { std::lock_guard lock(s.mutex); s.status.update({{"checked_at", now}, {"record_check", true}}); }
        const auto m = manifest(root, id);
        if (!m.contains("updates")) { std::lock_guard lock(s.mutex); s.status["phase"] = "unconfigured"; return; }
        const auto url = m.at("updates").at("feed").get<std::string>(); require(https_url(url), "Invalid HTTPS update feed");
        std::string text; fetch(url, feed_limit, [&](const void* bytes, std::size_t size) {
            if (size > feed_limit - text.size()) return false;
            text.append(static_cast<const char*>(bytes), size); return true;
        }, s.cancel);
        stopped(s.cancel); const auto release = choose_release(Json::parse(text), m, host);
        std::lock_guard lock(s.mutex); s.status.update({{"phase", release.is_null() ? "current" : "available"}, {"installed_version", m.at("version")},
            {"release", release}, {"manifest", m}, {"checked_at", now}, {"record_check", true}});
    });
}
bool Manager::download() {
    if (busy()) return false;
    Json snapshot; { std::lock_guard lock(state_->mutex); snapshot = state_->status; }
    if (snapshot.value("phase", "") != "available") return false;
    const auto id = snapshot.at("id").get<std::string>(), root = root_; const auto host = host_; const auto fetch = fetch_;
    return start(id, "downloading", [=](State& s) {
        const auto m = manifest(root, id); require(m == snapshot.at("manifest"), "Installed launcher changed; check again");
        const auto release = snapshot.at("release"); require(supports(release, host, m.value("state_schema", 1)), "Update is incompatible");
        const auto w = work(root, id); directories(base(root)); directories(base(root) + "/work");
        require(!exists(journal(root, id)), "Launcher needs recovery before updating"); remove_tree(w); directories(w);
        StagingGuard staging{w};
        struct Digest { EVP_MD_CTX* ctx = EVP_MD_CTX_new(); ~Digest() { EVP_MD_CTX_free(ctx); } } digest;
        require(digest.ctx && EVP_DigestInit_ex(digest.ctx, EVP_sha256(), nullptr) == 1, "Cannot initialize checksum");
        const auto expected = release.at("bytes").get<std::size_t>(); std::size_t done = 0;
        { Output file(w + "/download.zip");
          fetch(release.at("url"), expected, [&](const void* bytes, std::size_t size) {
            try { stopped(s.cancel); require(size <= expected - done, "Archive exceeds declared size"); file.write(bytes, size);
                require(EVP_DigestUpdate(digest.ctx, bytes, size) == 1, "Cannot hash download"); done += size;
                std::lock_guard lock(s.mutex); s.status["done"] = done; s.status["total"] = expected; return true;
            } catch (...) { return false; }
          }, s.cancel); file.finish(); }
        stopped(s.cancel); require(done == expected, "Archive size differs from feed");
        unsigned char hash[EVP_MAX_MD_SIZE]{}; unsigned size = 0;
        require(EVP_DigestFinal_ex(digest.ctx, hash, &size) == 1 && size == 32, "Cannot finish checksum");
        std::string hex; for (unsigned i = 0; i < size; ++i) { char byte[3]; std::snprintf(byte, sizeof(byte), "%02x", hash[i]); hex += byte; }
        require(hex == release.at("sha256").get<std::string>(), "Archive SHA-256 differs from feed");
        { std::lock_guard lock(s.mutex); s.status["phase"] = "validating"; }
        unpack(w + "/download.zip", w + "/new", id, s.cancel); validate_pack(w + "/new", release, host, m);
        require(unlink((w + "/download.zip").c_str()) == 0, "Cannot remove staged archive");
        stopped(s.cancel);
        std::lock_guard lock(s.mutex); s.status.update({{"phase", "ready"}, {"release", release}, {"manifest", m}});
        staging.keep = true;
    });
}
bool Manager::rollback(const std::string& id) {
    const auto root = root_; const auto host = host_;
    return start(id, "validating", [=](State& s) {
        const auto m = manifest(root, id); const auto w = work(root, id);
        require(!exists(journal(root, id)), "Launcher needs recovery before rollback"); remove_tree(w); directories(w);
        StagingGuard staging{w};
        std::uint64_t total = 0; unsigned count = 0; copy_tree(backup(root, id), w + "/new", s.cancel, total, count);
        validate_pack(w + "/new", nullptr, host, m); stopped(s.cancel);
        std::lock_guard lock(s.mutex); s.status.update({{"phase", "ready"}, {"manifest", m}, {"rollback", true}});
        staging.keep = true;
    });
}
bool Manager::commit() {
    if (busy()) return false;
    Json snapshot; { std::lock_guard lock(state_->mutex); snapshot = state_->status; }
    if (snapshot.value("phase", "") != "ready") return false;
    const auto root = root_, id = snapshot.at("id").get<std::string>();
    return start(id, "installing", [=](State& s) {
        require(manifest(root, id) == snapshot.at("manifest"), "Installed launcher changed before replacement");
        const auto next_version = json(work(root, id) + "/new/manifest.json").value("version", std::string{});
        stopped(s.cancel);
        // Cancellation is deliberately ignored after the durable transaction starts.
        try { replace(root, id); }
        catch (...) { if (exists(journal(root, id))) recover_one(root, id); throw; }
        std::lock_guard lock(s.mutex); s.status.update({{"phase", "installed"}, {"installed_version", next_version}, {"rollback", snapshot.value("rollback", false)}});
    });
}
void Manager::cancel() {
    if (replacing()) return;
    state_->cancel = true;
    if (!busy()) { std::lock_guard lock(state_->mutex); if (state_->status.value("phase", "") == "ready") state_->status["phase"] = "cancelled"; }
}
} // namespace pe::ui::launcher_updates
