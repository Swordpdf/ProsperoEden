// ps5-native-app-boilerplate - The self-update helper's work, apart from the console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace self_update
{
// The connection to the app: both return the bytes moved, or 0 or less when it ended.
struct Io
{
    std::function<long(void *data, std::size_t size)> read;
    std::function<long(const void *data, std::size_t size)> write;
};

// Everything the helper asks of the console, so the same code runs in host tests.
struct Environment
{
    // Folders whose direct children can be installed apps, as ShadowMountPlus scans them.
    std::vector<std::string> roots;
    // The drives those folders are on. The work folder is <drive>/self-update/<TITLEID>, on
    // the app's own drive so that putting files in place is a rename.
    std::vector<std::string> drives;
    // A running title has a folder "<TITLEID>_<n>" here.
    std::string sandboxes = "/mnt/sandbox";
    // Where the console keeps its own copies of an app's sce_sys ("/user"); empty: none.
    std::string registered = "/user";
    // Free bytes on the filesystem that holds path.
    std::function<bool(const std::string &path, std::uint64_t &bytes)> space;
    std::function<void(unsigned milliseconds)> sleep;
    std::function<void(const std::string &message)> notify;
    std::function<void(const std::string &line)> log;
    // Where the version an update replaces is kept, as <previous>/<TITLEID>-backup (the one
    // before it is removed): the files the new version took the place of, as they were. The
    // folder's parent must exist. Empty: the replaced files are deleted.
    std::string previous;
    // How long the app may take to close after "apply".
    unsigned exit_wait_ms = 120000;
    // Left alone for this long after the app is gone, before its files are touched.
    unsigned settle_ms = 1500;
};

// The console's drives ("/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0" to "/mnt/usb7") and
// the usual app folders on them (homebrew, etaHEN/games, and an external drive itself).
std::vector<std::string> default_drives();
std::vector<std::string> default_roots();

// The one of drives that path is inside; empty when it is in none.
std::string drive_root(const std::vector<std::string> &drives, const std::string &path);

// 1: running, 0: not, -1: unknown.
int title_running(const std::string &sandboxes, const std::string &title);

// Puts every top-level entry of staged in target's place, moving target's own
// entries to backup first. On failure target is as it was. Both folders must be
// on target's filesystem; backup must not exist.
bool swap_entries(const std::string &target, const std::string &staged, const std::string &backup);

// One whole conversation (kit/self_update_protocol.h). 0: the app was updated.
int run(const Environment &environment, const Io &io);
} // namespace self_update
