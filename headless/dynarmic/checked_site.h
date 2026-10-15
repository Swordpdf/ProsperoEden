// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <deque>

namespace Eden {
struct CheckedSite {
    std::uint64_t location;
    unsigned name;
    std::uint8_t budget = 64;
    bool demoted = false;
};
} // namespace Eden
