// SPDX-License-Identifier: GPL-3.0-or-later
// MihawkGen neural frame generation: global state the renderer reads.
// main.cpp sets these before a session's renderer starts; the Vulkan present path
// (prepare-vulkan-port.py) reads them to decide whether to initialise MihawkGen
// and generate frames through it instead of LSFG.
#pragma once

#include <atomic>
#include <string>

namespace Eden::MihawkGenBridge {

inline std::atomic<bool> active{false};
inline std::string weights_path;
inline std::atomic<int> target_rate{0};
inline std::atomic<int> multiplier{2};

} // namespace Eden::MihawkGenBridge
