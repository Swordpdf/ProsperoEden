// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__PROSPERO__) || defined(PS5_NATIVE) || defined(PE_RUNTIME_NATIVE_FS_TEST)
extern "C" int sceKernelGetdents(int, char*, int);
#endif

namespace pe::ui::runtime_fs {
enum class Kind { directory, file, resource };

class Descriptor {
public:
    explicit Descriptor(const std::string& path, Kind kind)
        : fd_(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                   (kind == Kind::directory ? O_DIRECTORY : 0))) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "Cannot open " + path);
        struct stat info{};
        const int error = fstat(fd_, &info) == 0 ? 0 : errno;
        const bool valid = kind == Kind::directory ? S_ISDIR(info.st_mode) :
                           kind == Kind::file ? S_ISREG(info.st_mode) :
                           S_ISREG(info.st_mode) || S_ISDIR(info.st_mode);
        if (error || !valid) {
            close(fd_); fd_ = -1;
            if (error) throw std::system_error(error, std::generic_category(), "Cannot inspect " + path);
            throw std::runtime_error("Unsupported resource type: " + path);
        }
    }
    ~Descriptor() { if (fd_ >= 0) close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

inline void check(const std::string& path, Kind kind) { Descriptor resource(path, kind); }

inline bool relative_path(std::string_view text) {
    if (text.empty() || text.size() > 240 || text.front() == '/' || text.find('\\') != text.npos) return false;
    for (std::size_t at = 0; at < text.size();) {
        const auto end = text.find('/', at);
        const auto part = text.substr(at, end == text.npos ? text.size() - at : end - at);
        if (part.empty() || part == "." || part == ".." || part.find(':') != part.npos) return false;
        for (unsigned char c : part) if (c < 32 || c == 127) return false;
        at = end == text.npos ? text.size() : end + 1;
    }
    return text.back() != '/';
}

inline std::string resource_path(const std::string& root, const std::string& relative) {
    if (!relative_path(relative)) throw std::runtime_error("Invalid pack resource path");
    check(root, Kind::directory);
    std::string path = root;
    for (std::size_t at = 0; at < relative.size();) {
        const auto end = relative.find('/', at);
        path += "/" + relative.substr(at, end == relative.npos ? relative.size() - at : end - at);
        try { check(path, end == relative.npos ? Kind::resource : Kind::directory); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string("Missing resource or symbolic link: ") + error.what());
        }
        at = end == relative.npos ? relative.size() : end + 1;
    }
    return path;
}

inline std::vector<std::string> entries(const std::string& path) {
    Descriptor directory(path, Kind::directory);
    std::vector<std::string> result;
    auto append = [&](std::string_view name) {
        if (name == "." || name == "..") return;
        if (name.empty() || name.find('/') != name.npos) throw std::runtime_error("Invalid directory entry: " + path);
        if (result.size() >= 4096) throw std::runtime_error("Launcher directory exceeds entry limit: " + path);
        result.emplace_back(name);
    };
#if defined(__PROSPERO__) || defined(PS5_NATIVE) || defined(PE_RUNTIME_NATIVE_FS_TEST)
    // App sandboxes deny lstat; the mounted PS5 filesystem also needs large directory reads.
    // Opening without following links keeps resource checks confined to the pack.
    std::vector<char> buffer(65536);
    for (;;) {
        const int count = sceKernelGetdents(directory.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (count == 0) break;
        if (count < 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot list " + path);
        if (count > static_cast<int>(buffer.size())) throw std::runtime_error("Invalid directory read size: " + path);
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
            constexpr auto name_offset = offsetof(dirent, d_name);
            const auto available = static_cast<std::size_t>(count) - offset;
            if (available <= name_offset) throw std::runtime_error("Truncated directory record: " + path);
            std::uint16_t length{};
            std::memcpy(&length, buffer.data() + offset + offsetof(dirent, d_reclen), sizeof(length));
            if (length <= name_offset || length > available) throw std::runtime_error("Invalid directory record length: " + path);
            const char* name = buffer.data() + offset + name_offset;
            const char* end = static_cast<const char*>(std::memchr(name, 0, length - name_offset));
            if (!end) throw std::runtime_error("Unterminated directory entry: " + path);
            append({name, static_cast<std::size_t>(end - name)});
            offset += length;
        }
    }
#else
    const int duplicate = dup(directory.get());
    if (duplicate < 0) throw std::system_error(errno, std::generic_category(), "Cannot duplicate " + path);
    DIR* raw = fdopendir(duplicate);
    if (!raw) {
        const int error = errno; close(duplicate);
        throw std::system_error(error, std::generic_category(), "Cannot list " + path);
    }
    struct Close { DIR* dir; ~Close() { closedir(dir); } } close{raw};
    for (;;) {
        errno = 0;
        const auto* entry = readdir(raw);
        if (!entry) {
            if (errno) throw std::system_error(errno, std::generic_category(), "Cannot list " + path);
            break;
        }
        append(entry->d_name);
    }
#endif
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace pe::ui::runtime_fs
