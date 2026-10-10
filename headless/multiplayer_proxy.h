// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <vector>
#include <zstd.h>
#include "common/common_types.h"

namespace Eden::Multiplayer {
inline constexpr std::size_t MaxProxyPayloadBytes = 1024 * 1024;
inline constexpr std::size_t MaxProxyQueueBytes = 4 * MaxProxyPayloadBytes;
inline constexpr std::size_t MaxProxyQueuePackets = 1024;

inline bool DecodeProxyPayload(std::span<const u8> compressed, std::vector<u8>& output) {
    output.clear();
    const auto size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (size > MaxProxyPayloadBytes) return false; // Includes ERROR and UNKNOWN sentinels.
    if (ZSTD_findFrameCompressedSize(compressed.data(), compressed.size()) != compressed.size())
        return false;
    output.resize(static_cast<std::size_t>(size));
    const auto decoded = ZSTD_decompress(output.data(), output.size(),
                                         compressed.data(), compressed.size());
    if (ZSTD_isError(decoded) || decoded != size) {
        output.clear();
        return false;
    }
    return true;
}
} // namespace Eden::Multiplayer
