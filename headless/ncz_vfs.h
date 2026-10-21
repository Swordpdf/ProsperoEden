// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// NSZ and XCZ: an NSP or XCI whose larger NCAs nsz compressed into NCZ files. Eden finds a .ncz
// where it looks for a .nca (FileSys::NSP, FileSys::XCI; headless/CMakeLists.txt) and reads the
// NCA it was made from through Open (ncz.cpp, on headless/ncz.h).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs_types.h"

namespace Eden::Ncz {
// The NCA an NCZ file holds, named as it was (<id>.nca); nullptr when `file` is null or is not an
// NCZ this can read (the log says why).
FileSys::VirtualFile Open(FileSys::VirtualFile file);

// Whether a solid NCZ may be unpacked now: while a game runs (the frontend says when, and that a
// new game starts). An unpack under way stops when it may not. Outside a game, a read
// decompresses it up to the read and keeps a few MiB around it, in a .pieces file beside the
// unpacked ones for later starts.
void SetUnpackAllowed(bool allowed);

// Where solid NCZs are unpacked to, and what removes files there: it is given which names to
// keep. The NCZ side asks it to remove an unpack cut short (*.part) at once, and the other games'
// unpacked copies before it unpacks one. The frontend sets it; until it does, a solid NCZ is never
// unpacked.
void SetUnpackFolder(std::string folder,
                     std::function<void(const std::function<bool(const std::string&)>& keep)> clear);
} // namespace Eden::Ncz
