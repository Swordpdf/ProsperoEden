// ps5-native-app-boilerplate - Link-refusing file helpers for the installer's work folders.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <atomic>
#include <memory>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace self_update
{
namespace
{
// Lists a tree without following links: files (and links, which are removed
// as names) and folders, each folder after everything inside it.
bool collect(const std::string &path, unsigned depth, std::vector<std::string> &files,
             std::vector<std::string> &folders)
{
    struct stat info
    {
    };
    if (lstat(path.c_str(), &info) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode))
    {
        files.push_back(path);
        return true;
    }
    if (depth > 300)
        return false;
    std::vector<std::string> names;
    {
        std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(path.c_str()), closedir);
        if (!directory)
            return false;
        for (;;)
        {
            errno = 0;
            const auto *entry = readdir(directory.get());
            if (!entry)
            {
                if (errno)
                    return false;
                break;
            }
            const std::string name = entry->d_name;
            if (name != "." && name != "..")
                names.push_back(name);
        }
    }
    for (const auto &name : names)
        if (!collect(path + "/" + name, depth + 1, files, folders))
            return false;
    folders.push_back(path);
    return true;
}

// Removing a file costs about as much as creating one on the console's
// storage, so the files of a large app are removed by several workers at once.
struct RemoveWork
{
    const std::vector<std::string> &files;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    explicit RemoveWork(const std::vector<std::string> &list) : files(list)
    {
    }
};
void *remove_worker(void *opaque)
{
    auto &work = *static_cast<RemoveWork *>(opaque);
    for (;;)
    {
        const std::size_t index = work.next.fetch_add(1);
        if (index >= work.files.size())
            return nullptr;
        if (unlink(work.files[index].c_str()) != 0 && errno != ENOENT)
            work.failed = true;
    }
}
} // namespace

Kind kind(const std::string &path)
{
    struct stat info
    {
    };
    if (lstat(path.c_str(), &info) != 0)
        return errno == ENOENT || errno == ENOTDIR ? Kind::absent : Kind::unknown;
    return S_ISDIR(info.st_mode)   ? Kind::directory
           : S_ISREG(info.st_mode) ? Kind::file
                                   : Kind::other;
}

bool make_directory(const std::string &path)
{
    return mkdir(path.c_str(), 0755) == 0 || (errno == EEXIST && kind(path) == Kind::directory);
}

bool sync_directory(const std::string &path)
{
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    const bool ok = fsync(descriptor) == 0;
    return close(descriptor) == 0 && ok;
}

bool read_small(const std::string &path, std::size_t limit, std::string &body)
{
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return false;
    struct stat info
    {
    };
    bool ok = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
              static_cast<std::uint64_t>(info.st_size) <= limit;
    std::string bytes;
    char buffer[4096];
    while (ok)
    {
        const auto count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            ok = count == 0;
            break;
        }
        if (static_cast<std::size_t>(count) > limit - bytes.size())
            ok = false;
        else
            bytes.append(buffer, static_cast<std::size_t>(count));
    }
    close(descriptor);
    if (ok)
        body = std::move(bytes);
    return ok;
}

bool copy_file(const std::string &from, const std::string &to)
{
    const int source = open(from.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat info
    {
    };
    if (source < 0 || fstat(source, &info) != 0 || !S_ISREG(info.st_mode))
    {
        if (source >= 0)
            close(source);
        return false;
    }
    const std::string temporary = to + ".store-new";
    unlink(temporary.c_str());
    const int target = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    bool ok = target >= 0;
    std::vector<char> buffer(256 * 1024);
    while (ok)
    {
        const auto count = read(source, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            ok = count == 0;
            break;
        }
        for (ssize_t done = 0; ok && done < count;)
        {
            const auto wrote =
                write(target, buffer.data() + done, static_cast<std::size_t>(count - done));
            if (wrote < 0 && errno == EINTR)
                continue;
            ok = wrote > 0;
            done += wrote > 0 ? wrote : 0;
        }
    }
    close(source);
    if (target >= 0)
    {
        ok = fsync(target) == 0 && ok;
        ok = close(target) == 0 && ok;
    }
    ok = ok && rename(temporary.c_str(), to.c_str()) == 0;
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}

bool list_files(const std::string &folder, std::size_t limit, std::vector<std::string> &names)
{
    std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(folder.c_str()), closedir);
    if (!directory)
        return false;
    for (;;)
    {
        errno = 0;
        const auto *entry = readdir(directory.get());
        if (!entry)
            return errno == 0;
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || kind(folder + "/" + name) != Kind::file)
            continue;
        if (names.size() >= limit)
            return false;
        names.push_back(name);
    }
}

bool move_tree(const std::string &from, const std::string &to)
{
    if (kind(from) != Kind::directory || kind(to) != Kind::absent)
        return false;
    if (rename(from.c_str(), to.c_str()) == 0)
        return true;
    if (errno != EXDEV)
        return false;
    // Another drive: copy. collect() lists a folder after what it holds, so the folders are
    // made in the other order.
    std::vector<std::string> files, folders;
    bool ok = collect(from, 0, files, folders);
    const auto mode_of = [](const std::string &path, mode_t fallback)
    {
        struct stat info
        {
        };
        return lstat(path.c_str(), &info) == 0 ? static_cast<mode_t>(info.st_mode & 0777)
                                               : fallback;
    };
    for (auto folder = folders.rbegin(); ok && folder != folders.rend(); ++folder)
    {
        const std::string made = to + folder->substr(from.size());
        ok = mkdir(made.c_str(), 0755) == 0 && chmod(made.c_str(), mode_of(*folder, 0755)) == 0;
    }
    for (std::size_t i = 0; ok && i < files.size(); ++i)
    {
        if (kind(files[i]) != Kind::file)
            continue; // a link or a special file: not followed, not kept
        const std::string copy = to + files[i].substr(from.size());
        ok = copy_file(files[i], copy) && chmod(copy.c_str(), mode_of(files[i], 0644)) == 0;
    }
    if (!ok)
    {
        (void)remove_tree(to);
        return false;
    }
    (void)remove_tree(from);
    return true;
}

bool remove_tree(const std::string &path)
{
    std::vector<std::string> files, folders;
    if (!collect(path, 0, files, folders))
        return false;
    RemoveWork work(files);
    pthread_t workers[8];
    std::size_t running = 0;
    if (files.size() > 16)
        for (auto &worker : workers)
            if (pthread_create(&worker, nullptr, remove_worker, &work) == 0)
                workers[running++] = worker;
    if (running == 0)
        remove_worker(&work);
    for (std::size_t i = 0; i < running; ++i)
        pthread_join(workers[i], nullptr);
    if (work.failed.load())
        return false;
    for (const auto &folder : folders)
        if (rmdir(folder.c_str()) != 0 && errno != ENOENT)
            return false;
    return true;
}
} // namespace self_update
