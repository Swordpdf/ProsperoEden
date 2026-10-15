// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the native directory reader while legacy app-sandbox APIs are denied.
#include "pe/ui/runtime_filesystem.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/syscall.h>

namespace fs = std::filesystem;
namespace packfs = pe::ui::runtime_fs;
static bool deny_legacy = false;
static int legacy_calls = 0, native_calls = 0, fault = 0, checks = 0;

extern "C" int __real_lstat(const char*, struct stat*);
extern "C" DIR* __real_opendir(const char*);
extern "C" int __wrap_lstat(const char* path, struct stat* info) {
    if (deny_legacy) { ++legacy_calls; errno = EPERM; return -1; }
    return __real_lstat(path, info);
}
extern "C" DIR* __wrap_opendir(const char* path) {
    if (deny_legacy) { ++legacy_calls; errno = EPERM; return nullptr; }
    return __real_opendir(path);
}
extern "C" int sceKernelGetdents(int fd, char* bytes, int count) {
    ++native_calls;
    assert(count == 65536);
    if (!fault) return static_cast<int>(syscall(SYS_getdents64, fd, bytes, count));
    if (fault == 1) { errno = EPERM; return -1; }
    if (fault == 2) return count + 1;
    constexpr auto start = offsetof(dirent, d_name);
    if (fault == 3) return start;
    std::uint16_t size = start + 5;
    std::memset(bytes, 1, size);
    if (fault == 4) size = start;
    if (fault == 5) size += 100;
    std::memcpy(bytes + offsetof(dirent, d_reclen), &size, sizeof(size));
    if (fault == 7) { bytes[start] = 0; }
    if (fault == 8) { std::memcpy(bytes + start, "a/b", 4); }
    return start + 5;
}
template<class Operation> void rejects(Operation operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    assert(rejected); ++checks;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path root = argv[1], pack = root / "switch-ui";
    assert(!fs::exists(root)); fs::create_directories(pack / "assets/sounds");
    std::ofstream(pack / "manifest.json") << "{}";
    std::ofstream(pack / "main.lua") << "return {}";
    std::ofstream(pack / "assets/sounds/focus.wav") << "wav";
    fs::create_symlink(pack / "main.lua", pack / "linked.lua");
    fs::create_directory_symlink(pack / "assets", pack / "linked-assets");
    fs::create_directory_symlink(pack, root / "linked-pack");
    assert(mkfifo((pack / "fifo").c_str(), 0600) == 0);
    const auto many = root / "many"; fs::create_directory(many);
    for (int i = 0; i < 2000; ++i) std::ofstream(many / ("item-" + std::to_string(i) + std::string(48, 'a')));

    deny_legacy = true;
    struct stat status{};
    assert(lstat(pack.c_str(), &status) == -1 && errno == EPERM);
    assert(opendir(pack.c_str()) == nullptr && errno == EPERM);
    legacy_calls = 0;
    packfs::check(root, packfs::Kind::directory); ++checks;
    assert(packfs::entries(pack / "assets/sounds") == std::vector<std::string>{"focus.wav"}); ++checks;
    assert(packfs::entries(pack / "assets").size() == 1); ++checks;
    assert(packfs::resource_path(pack, "main.lua") == (pack / "main.lua").string()); ++checks;
    assert(packfs::resource_path(pack, "assets/sounds/focus.wav") == (pack / "assets/sounds/focus.wav").string()); ++checks;
    packfs::check(pack / "manifest.json", packfs::Kind::file); ++checks;
    const int before = native_calls;
    assert(packfs::entries(many).size() == 2000 && native_calls > before + 2); ++checks;
    rejects([&] { packfs::check(pack / "main.lua", packfs::Kind::directory); });
    rejects([&] { packfs::check(pack, packfs::Kind::file); });
    rejects([&] { packfs::resource_path(pack, "missing"); });
    rejects([&] { packfs::resource_path(pack, "linked.lua"); });
    rejects([&] { packfs::resource_path(pack, "linked-assets/sounds/focus.wav"); });
    rejects([&] { packfs::resource_path(root / "linked-pack", "main.lua"); });
    rejects([&] { packfs::entries(root / "linked-pack"); });
    rejects([&] { packfs::resource_path(pack, "fifo"); });
    for (const std::string invalid : {"", "/main.lua", "../main.lua", "assets/../main.lua", "assets//x", "./main.lua", "assets/", "a\\b", "a:b", "a\nb"})
        rejects([&] { packfs::resource_path(pack, invalid); });
    for (fault = 1; fault <= 8; ++fault) rejects([&] { packfs::entries(pack); });
    fault = 0;
    assert(legacy_calls == 0); ++checks;
    deny_legacy = false; fs::remove_all(root);
    std::cout << "PASS " << checks << " native filesystem checks; no lstat/opendir, 64 KiB reads, confined resources and malformed-record rejection\n";
}
