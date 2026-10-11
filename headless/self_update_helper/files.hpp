// ps5-native-app-boilerplate - Link-refusing file helpers for the installer's work folders.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace self_update
{
enum class Kind
{
    absent,
    directory,
    file,
    other, // A symbolic link or special file: never followed, never trusted.
    unknown
};
Kind kind(const std::string &path);
// One level; an existing real directory is accepted.
bool make_directory(const std::string &path);
bool sync_directory(const std::string &path);
// Regular file of at most limit bytes, opened without following a link.
bool read_small(const std::string &path, std::size_t limit, std::string &body);
// Copies a regular file (never through a link) to a temporary name beside
// the destination, then renames it over the destination.
bool copy_file(const std::string &from, const std::string &to);
// The names of the regular files directly in a folder; at most limit of them.
bool list_files(const std::string &folder, std::size_t limit, std::vector<std::string> &names);
// Moves a folder to a place where nothing is: a rename when both are on one drive, otherwise a
// copy of its folders and regular files (links are left out) with their modes, and then the
// removal of the original. On failure nothing is left at to and from is as it was.
bool move_tree(const std::string &from, const std::string &to);
// Removes a file or a whole folder without following links. True when nothing
// is left at path, including when nothing was there.
bool remove_tree(const std::string &path);
} // namespace self_update
