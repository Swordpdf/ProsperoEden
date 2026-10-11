// ps5-native-app-boilerplate - The self-update helper's work, apart from the console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The helper runs outside the app's sandbox, started by the console's payload
// loader, because that is where an app's folder can be written and where the
// console does not slow file writes down. It trusts nothing it is sent: the
// title ID decides every path, the archive must match the digest it was
// announced with, and the unpacked app must be the title and version asked for.
#include "updater.hpp"
#include "update_check.h"
#include "self_update_protocol.h"
#include "self_update_sha256.h"
#include "archive.hpp"
#include "files.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace self_update
{
namespace
{
constexpr std::size_t kChildren = 4096;   // apps looked at in one folder
constexpr std::size_t kParam = 64 * 1024; // largest param.json read
constexpr std::uint64_t kDownloadMargin = 16ull << 20;
constexpr std::uint64_t kUnpackMargin = 64ull << 20;

bool title_id(const std::string &text)
{
    if (text.size() != 9)
        return false;
    for (std::size_t i = 0; i < 9; ++i)
        if (i < 4 ? text[i] < 'A' || text[i] > 'Z' : text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

bool content_version(const std::string &text)
{
    unsigned parts[3];
    return update_check_version_parse(text.c_str(), parts) == 1;
}

bool newer(const std::string &available, const std::string &installed)
{
    int comparable = 0;
    return update_check_version_compare(available.c_str(), installed.c_str(), &comparable) > 0 &&
           comparable;
}

bool hex_digest(const std::string &text)
{
    if (text.size() != 64)
        return false;
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

// Text shown in a notification: no control characters, not too long.
std::string shown(const std::string &text, std::size_t limit, const std::string &fallback)
{
    std::string out;
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f)
            continue;
        if (out.size() >= limit)
            break;
        out.push_back(c);
    }
    return out.empty() ? fallback : out;
}

bool list_entries(const std::string &folder, std::size_t limit, std::vector<std::string> &names)
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
        if (name == "." || name == "..")
            continue;
        if (names.size() >= limit)
            return false;
        names.push_back(name);
    }
}

// The title ID and content version an app folder declares.
bool read_param(const std::string &folder, std::string &title, std::string &version)
{
    std::string body;
    if (!read_small(folder + "/sce_sys/param.json", kParam, body))
        return false;
    char id[10];
    char content[12];
    if (update_check_json_string(body.data(), body.size(), "titleId", id, sizeof(id)) != 1 ||
        update_check_json_string(body.data(), body.size(), "contentVersion", content,
                                 sizeof(content)) != 1)
        return false;
    title = id;
    version = content;
    return true;
}

struct Copy
{
    std::string path, version;
};

// Every installed folder of the title under the roots. False: a root could not be read fully.
bool find_copies(const Environment &environment, const std::string &title, std::vector<Copy> &out)
{
    for (const auto &root : environment.roots)
    {
        if (kind(root) != Kind::directory)
            continue;
        std::vector<std::string> names;
        if (!list_entries(root, kChildren, names))
            return false;
        for (const auto &name : names)
        {
            const std::string folder = root + "/" + name;
            std::string id, version;
            if (kind(folder) != Kind::directory || !read_param(folder, id, version) || id != title)
                continue;
            bool known = false;
            for (const auto &copy : out)
                known = known || copy.path == folder;
            if (!known)
                out.push_back({folder, version});
        }
    }
    return true;
}

// ProsperoEden: ShadowMountPlus records where it mounted an app from in
// <registered>/app/<TITLEID>/mount.lnk (a folder) or mount_img.lnk (an image). 1: path is
// that folder; 0: no record; -1: the app is an image (its files can't be replaced one by one).
int mounted_source(const Environment &environment, const std::string &title, std::string &path)
{
    if (environment.registered.empty())
        return 0;
    const std::string app = environment.registered + "/app/" + title;
    std::FILE *file = std::fopen((app + "/mount.lnk").c_str(), "rb");
    if (!file)
        return kind(app + "/mount_img.lnk") == Kind::file ? -1 : 0;
    char text[512] = {};
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    path.assign(text, length);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ' ||
                             path.back() == '\0' || path.back() == '/'))
        path.pop_back();
    return path.size() > 1 && path[0] == '/' ? 1 : 0;
}

bool same_filesystem(const std::string &first, const std::string &second)
{
    struct stat a
    {
    };
    struct stat b
    {
    };
    return stat(first.c_str(), &a) == 0 && stat(second.c_str(), &b) == 0 && a.st_dev == b.st_dev;
}

// The console starts an app only if its files can be read and run by everyone, which is how an
// app copied to the console arrives (mode 0777, owner and group 0). Unpacked files are made
// the same; with any other mode the console answers "Can't start the game or app" (found on a
// console: process creation fails with EACCES).
bool open_permissions(const std::string &path, unsigned depth = 0)
{
    const Kind what = kind(path);
    if ((what != Kind::directory && what != Kind::file) || depth > 32)
        return false;
    if (chmod(path.c_str(), 0777) != 0)
        return false;
    (void)chown(path.c_str(), 0, 0); // best effort: the mode is what decides
    if (what == Kind::file)
        return true;
    std::vector<std::string> names;
    if (!list_entries(path, kArchiveEntries, names))
        return false;
    for (const auto &name : names)
        if (!open_permissions(path + "/" + name, depth + 1))
            return false;
    return true;
}

// ShadowMountPlus copies an app's sce_sys once, when it first registers the
// title. After an update those copies are brought up to date here. Best effort.
void refresh_registered(const Environment &environment, const std::string &title,
                        const std::string &target)
{
    if (environment.registered.empty())
        return;
    const std::string source = target + "/sce_sys";
    std::vector<std::string> names;
    if (!list_files(source, 64, names))
        return;
    const std::string app = environment.registered + "/app/" + title;
    for (const auto &folder : {environment.registered + "/appmeta/" + title, app + "/sce_sys"})
        if (kind(folder) == Kind::directory)
            for (const auto &name : names)
                copy_file(source + "/" + name, folder + "/" + name);
    if (kind(app + "/icon0.png") == Kind::file)
        copy_file(source + "/icon0.png", app + "/icon0.png");
}

// The conversation with the app. Lines out are serialised; after the archive
// has arrived a thread of its own reads what the app says.
struct Session
{
    // Copies: the listening thread can outlive the call that started it.
    const Environment environment;
    const Io io;
    Session(const Environment &environment_, const Io &io_) : environment(environment_), io(io_)
    {
    }
    std::mutex output;
    std::atomic<bool> cancelled{false}, apply{false}, closed{false}, unpacking{false};
    std::atomic<std::uint64_t> written{0};
    std::uint64_t to_unpack = 0;

    void log(const std::string &line) const
    {
        if (environment.log)
            environment.log(line);
    }
    bool say(const std::string &line)
    {
        std::lock_guard lock(output);
        std::size_t done = 0;
        while (done < line.size())
        {
            const long count = io.write(line.data() + done, line.size() - done);
            if (count <= 0)
                return false;
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    bool read_exact(void *data, std::size_t size) const
    {
        auto *bytes = static_cast<char *>(data);
        while (size)
        {
            const long count = io.read(bytes, size);
            if (count <= 0)
                return false;
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
        return true;
    }
    bool read_line(std::string &line) const
    {
        line.clear();
        for (;;)
        {
            char byte = 0;
            if (io.read(&byte, 1) <= 0)
                return false;
            if (byte == '\n')
                return true;
            if (line.size() >= SELF_UPDATE_LINE)
                return false;
            line.push_back(byte);
        }
    }
    void sleep(unsigned milliseconds) const
    {
        if (environment.sleep)
            environment.sleep(milliseconds);
        else
            usleep(milliseconds * 1000u);
    }
};

void *listen(void *argument)
{
    // Its own reference keeps the session alive for as long as it reads.
    const std::unique_ptr<std::shared_ptr<Session>> holder(
        static_cast<std::shared_ptr<Session> *>(argument));
    auto &session = **holder;
    std::string line;
    while (session.read_line(line))
    {
        if (line == "apply")
            session.apply = true;
        else if (!session.apply.load())
            session.cancelled = true; // "cancel", or anything unexpected
    }
    // The app closing the connection before "apply" is a cancel; after it, it is the app leaving.
    if (!session.apply.load())
        session.cancelled = true;
    session.closed = true;
    return nullptr;
}

void *report(void *argument)
{
    auto &session = *static_cast<Session *>(argument);
    while (session.unpacking.load())
    {
        session.say("p " + std::to_string(session.written.load()) + " " +
                    std::to_string(session.to_unpack) + "\n");
        session.sleep(200);
    }
    return nullptr;
}

// Saves the archive as it arrives, hashing it. The file is kept only when the
// end mark arrived, its size is the one announced and its digest matches.
bool receive_archive(Session &session, const std::string &path, std::uint64_t size,
                     const std::string &digest, std::string &error)
{
    error = "The download could not be saved";
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return false;
    const std::uint64_t limit = size ? size : SELF_UPDATE_MAX_ARCHIVE;
    std::vector<char> piece(SELF_UPDATE_PIECE);
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    std::uint64_t received = 0;
    bool complete = false, saved = true, connected = true;
    for (;;)
    {
        unsigned char header[4];
        if (!session.read_exact(header, sizeof(header)))
        {
            connected = false;
            break;
        }
        const std::size_t length = static_cast<std::size_t>(header[0]) |
                                   (static_cast<std::size_t>(header[1]) << 8) |
                                   (static_cast<std::size_t>(header[2]) << 16) |
                                   (static_cast<std::size_t>(header[3]) << 24);
        if (length == 0)
        {
            complete = true;
            break;
        }
        if (length > piece.size() || length > limit - received)
        {
            error = "The download is larger than the catalog lists";
            break;
        }
        if (!session.read_exact(piece.data(), length))
        {
            connected = false;
            break;
        }
        self_update_sha256_add(&hash, piece.data(), length);
        received += length;
        for (std::size_t done = 0; done < length && saved;)
        {
            const auto count = write(descriptor, piece.data() + done, length - done);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                saved = false;
            else
                done += static_cast<std::size_t>(count);
        }
        if (!saved)
            break;
    }
    saved = saved && fsync(descriptor) == 0;
    saved = close(descriptor) == 0 && saved;
    if (!connected)
    {
        session.cancelled = true;
        error = "Cancelled";
        return false;
    }
    if (!complete || !saved)
        return false;
    unsigned char bytes[32];
    self_update_sha256_finish(&hash, bytes);
    if ((size && received != size) || !self_update_sha256_matches(bytes, digest.c_str()))
    {
        error = "The download doesn't match the catalog's listing";
        return false;
    }
    error.clear();
    return true;
}
} // namespace

std::vector<std::string> default_drives()
{
    std::vector<std::string> drives{"/data", "/mnt/ext0", "/mnt/ext1"};
    for (int i = 0; i < 8; ++i)
        drives.push_back("/mnt/usb" + std::to_string(i));
    return drives;
}

std::vector<std::string> default_roots()
{
    std::vector<std::string> roots;
    for (const auto &drive : default_drives())
    {
        roots.push_back(drive + "/homebrew");
        roots.push_back(drive + "/etaHEN/games");
        if (drive != "/data")
            roots.push_back(drive);
    }
    return roots;
}

std::string drive_root(const std::vector<std::string> &drives, const std::string &path)
{
    for (const auto &drive : drives)
        if (path.size() > drive.size() && path.compare(0, drive.size(), drive) == 0 &&
            path[drive.size()] == '/')
            return drive;
    return {};
}

int title_running(const std::string &sandboxes, const std::string &title)
{
    if (!title_id(title))
        return -1;
    std::vector<std::string> names;
    if (list_entries(sandboxes, 1024, names))
    {
        for (const auto &name : names)
            if (name.size() > 10 && name[9] == '_' && name.compare(0, 9, title) == 0)
                return 1;
        return 0;
    }
    // The folder can't be listed: ask for the usual sandbox by name.
    return kind(sandboxes + "/" + title + "_000") == Kind::absent ? -1 : 1;
}

bool swap_entries(const std::string &target, const std::string &staged, const std::string &backup)
{
    std::vector<std::string> present, new_names;
    if (!list_entries(target, kChildren, present) ||
        !list_entries(staged, kChildren, new_names) || new_names.empty() ||
        mkdir(backup.c_str(), 0755) != 0)
        return false;
    // ProsperoEden: only the entries the release replaces are moved aside (and deleted with the
    // work folder). Whatever else the folder holds is the player's (language.txt, the earlier
    // versions' assets/ with keys, firmware and games) and stays where it is.
    std::vector<std::string> old_names;
    for (const auto &name : present)
        if (std::find(new_names.begin(), new_names.end(), name) != new_names.end())
            old_names.push_back(name);
    std::size_t moved = 0, placed = 0;
    bool ok = true;
    for (; ok && moved < old_names.size(); ++moved)
        ok = rename((target + "/" + old_names[moved]).c_str(),
                    (backup + "/" + old_names[moved]).c_str()) == 0;
    if (!ok)
        --moved; // the one that failed was not moved
    for (; ok && placed < new_names.size(); ++placed)
        ok = rename((staged + "/" + new_names[placed]).c_str(),
                    (target + "/" + new_names[placed]).c_str()) == 0;
    if (!ok && placed)
        --placed;
    if (ok)
    {
        (void)sync_directory(target);
        return true;
    }
    // Back to how it was: the new entries return to staged, the old ones to target.
    for (std::size_t i = 0; i < placed; ++i)
        (void)rename((target + "/" + new_names[i]).c_str(), (staged + "/" + new_names[i]).c_str());
    for (std::size_t i = 0; i < moved; ++i)
        (void)rename((backup + "/" + old_names[i]).c_str(), (target + "/" + old_names[i]).c_str());
    (void)rmdir(backup.c_str());
    (void)sync_directory(target);
    return false;
}

int run(const Environment &environment, const Io &io)
{
    const auto shared = std::make_shared<Session>(environment, io);
    Session &session = *shared;
    std::string magic, verb, title, installed, available, size_text, digest, name, label;
    if (!session.read_line(magic) || magic != SELF_UPDATE_MAGIC || !session.read_line(verb) ||
        !session.read_line(title) || !session.read_line(installed) ||
        !session.read_line(available) || !session.read_line(size_text) ||
        !session.read_line(digest) || !session.read_line(name) || !session.read_line(label))
        return 1;
    char *end = nullptr;
    errno = 0;
    const std::uint64_t size = std::strtoull(size_text.c_str(), &end, 10);
    if (verb != "update" || !title_id(title) || !content_version(installed) ||
        !content_version(available) || !newer(available, installed) || !hex_digest(digest) ||
        size_text.empty() || errno || *end != '\0' || size > SELF_UPDATE_MAX_ARCHIVE)
    {
        session.say("fail The request was refused\n");
        return 1;
    }
    name = shown(name, 64, title);
    label = shown(label, 40, available);

    // The one installed folder of the title, at the version that asked: the folder
    // ShadowMountPlus mounted it from, when it recorded one; otherwise the one copy under the roots.
    std::vector<Copy> copies;
    std::string source;
    const int mounted = mounted_source(environment, title, source);
    if (mounted < 0)
    {
        session.say("fail The app is installed as an image; update it by replacing the image\n");
        return 1;
    }
    if (mounted > 0)
    {
        std::string id, version;
        if (kind(source) == Kind::directory && read_param(source, id, version) && id == title)
        {
            copies.push_back({source, version});
            session.log("installed folder (ShadowMountPlus): " + source);
        }
    }
    if (copies.empty() && (!find_copies(environment, title, copies) || copies.empty()))
    {
        session.say("fail The app's folder wasn't found. Apps installed as an image can't "
                    "update themselves\n");
        return 1;
    }
    if (copies.size() != 1)
    {
        session.say("fail More than one copy of the app is installed\n");
        return 1;
    }
    const std::string target = copies[0].path;
    if (copies[0].version != installed)
    {
        session.say("fail The installed files aren't the version that is running\n");
        return 1;
    }
    const std::string drive = drive_root(environment.drives, target);
    if (drive.empty())
    {
        session.say("fail The app is installed where it can't be updated\n");
        return 1;
    }
    const std::string base = drive + "/self-update", work = base + "/" + title;
    const std::string archive = work + "/update.zip", staged = work + "/staged";
    const auto clean = [&]
    {
        (void)remove_tree(work);
        (void)rmdir(base.c_str());
    };
    std::uint64_t free_bytes = 0;
    if (!remove_tree(work) || !make_directory(base) || !make_directory(work) ||
        !same_filesystem(target, work))
    {
        clean();
        session.say("fail The update's work folder couldn't be made\n");
        return 1;
    }
    if (environment.space && environment.space(work, free_bytes) &&
        free_bytes < size + kDownloadMargin)
    {
        clean();
        session.say("fail There isn't enough free space for the download\n");
        return 1;
    }
    session.log("update " + title + " " + installed + " -> " + available + " in " + target);
    if (!session.say("ready\n"))
    {
        clean();
        return 1;
    }

    std::string error;
    if (!receive_archive(session, archive, size, digest, error))
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + error + "\n");
        return 1;
    }

    pthread_t listener{};
    auto *reference = new std::shared_ptr<Session>(shared);
    if (pthread_create(&listener, nullptr, listen, reference) != 0)
    {
        delete reference;
        clean();
        session.say("fail The helper couldn't start\n");
        return 1;
    }
    // The listener stays in its read until the app goes away, which a failed
    // update doesn't wait for: nothing it touches is used after this function.
    pthread_detach(listener);
    const auto fail = [&](const std::string &reason)
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + reason + "\n");
        return 1;
    };

    ArchiveInfo info;
    if (!inspect_archive(archive, title, info, error))
        return fail(error);
    if (environment.space && environment.space(work, free_bytes) &&
        free_bytes < info.unpacked + kUnpackMargin)
        return fail("There isn't enough free space to unpack the update");
    session.to_unpack = info.unpacked;
    session.unpacking = true;
    pthread_t reporter{};
    const bool reporting = pthread_create(&reporter, nullptr, report, &session) == 0;
    const bool unpacked =
        extract_archive(archive, title, staged, session.cancelled, session.written, error);
    session.unpacking = false;
    if (reporting)
        pthread_join(reporter, nullptr);
    if (!unpacked)
        return fail(error.empty() ? "The update could not be unpacked" : error);
    (void)unlink(archive.c_str());
    if (!open_permissions(staged))
        return fail("The update's files could not be prepared");

    std::string staged_title, staged_version;
    if (!read_param(staged, staged_title, staged_version) || staged_title != title ||
        staged_version != available)
        return fail("The download isn't the version the catalog lists");
    session.say("p " + std::to_string(info.unpacked) + " " + std::to_string(info.unpacked) + "\n");
    if (!session.say("staged\n"))
    {
        clean();
        return 1;
    }

    while (!session.apply.load())
    {
        if (session.cancelled.load())
        {
            clean();
            return 1;
        }
        session.sleep(50);
    }
    session.say("applying\n");

    // Only a closed app is touched. Unknown counts as running.
    unsigned waited = 0;
    while (title_running(environment.sandboxes, title) != 0)
    {
        if (waited >= environment.exit_wait_ms)
        {
            clean();
            session.log("the app did not close; nothing was changed");
            if (environment.notify)
                environment.notify(name + " wasn't updated: it didn't close. Nothing was changed.");
            return 1;
        }
        session.sleep(250);
        waited += 250;
    }
    session.sleep(environment.settle_ms);

    if (!swap_entries(target, staged, work + "/backup"))
    {
        clean();
        session.log("the files could not be replaced; the app is as it was");
        if (environment.notify)
            environment.notify(name + " wasn't updated: its files couldn't be replaced.");
        return 1;
    }
    refresh_registered(environment, title, target);
    // The files the update replaced are kept as the previous version, so that it can be put
    // back by hand if the new one misbehaves. Only the latest one is kept.
    if (!environment.previous.empty())
    {
        const std::string kept = environment.previous + "/" + title + "-backup";
        if (make_directory(environment.previous) && remove_tree(kept) &&
            move_tree(work + "/backup", kept))
            session.log("version " + installed + " is kept in " + kept);
        else
            session.log("version " + installed + " could not be kept in " + kept);
    }
    clean();
    session.log("updated " + title + " to " + available);
    if (environment.notify)
        environment.notify(name + " was updated to " + label + ". Open it again.");
    return 0;
}
} // namespace self_update
