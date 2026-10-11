// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include "diagnostics.h"

namespace Eden::Multiplayer {
// Launcher joins happen before Eden's per-game logger starts.
inline std::atomic<bool> detailed_logging{false};
inline void SetDetailedLogging(bool enabled) { detailed_logging.store(enabled); }
inline void Debug(const char* message) {
    if (detailed_logging.load()) Eden::Report("multiplayer debug", message);
}
}
