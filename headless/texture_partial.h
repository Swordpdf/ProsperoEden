// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "performance.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/types.h"

namespace Tegra {
class MemoryManager;
}

namespace VideoCommon {
// Port (texture_util.cpp): the block rows of a one-layer block-linear image, with several levels and
// no tile width spacing, that hold the guest byte ranges `dirty` (offsets from the image start,
// sorted, not overlapping). A block row (8 << block height rows of tiles across the full stride) is
// contiguous in guest memory, so a guest range maps to whole rows of one or more levels. Unswizzles
// those rows into `output` and returns their copies, or nothing when the image's level layout is not
// one these rows can follow (the caller uploads the whole image).
std::optional<std::vector<BufferImageCopy>> UnswizzleImageRows(
    const ImageInfo& info, u32 guest_size, std::span<const std::pair<u32, u32>> dirty,
    const std::function<void(u32 offset, u8* data, u32 size)>& read, Common::ScratchBuffer<u8>& output);
// Whether `rows` (the copies' buffer) matches the same rows of a whole-image UnswizzleImage of `guest`.
bool CheckImageRows(Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info, u32 guest_size,
                    std::span<const u8> guest, std::span<const BufferImageCopy> copies, std::span<const u8> rows);
bool CanUnswizzleImageRows(const ImageInfo& info, u32 guest_size);
} // namespace VideoCommon

// A game streaming small tiles into a large texture (a 5120x5120 BC4 atlas, 17.8 MB) made Eden
// upload the whole image again after each burst of writes, 4-8 ms of GPU-thread work each, up to nine
// times in a frame: 40-75 ms frames. Here a guest write to a large block-linear image whose contents
// the GPU already holds stops tracking only the 4 KiB pages it wrote, and the next use uploads the
// block rows those pages hold. Every page untracked here is tracked again before anything else
// untracks the image (the counts balance). dev-settings tex_partial=off restores the whole-image
// upload; tex_partial_check=on also unswizzles the whole image and compares (EDEN_TEX_PARTIAL).
// Texture cache lock held throughout.
namespace Eden::TexturePartial {
struct State {
    bool uploaded = false;                    // a whole upload filled the image once
    std::vector<u64> pages;                   // device pages untracked by guest writes, sorted
};
inline std::unordered_map<u32, State> images; // by ImageId index (the Vulkan cache only)

inline bool Eligible(const VideoCommon::ImageBase& image) {
    using VideoCommon::ImageFlagBits;
    constexpr auto excluded = ImageFlagBits::Sparse | ImageFlagBits::Converted |
                              ImageFlagBits::AcceleratedUpload | ImageFlagBits::AsynchronousDecode;
    return Eden::Performance::tex_partial.load(std::memory_order_relaxed) &&
           image.guest_size_bytes >= Eden::Performance::tex_partial_min.load(std::memory_order_relaxed) &&
           False(image.flags & excluded) && True(image.flags & ImageFlagBits::Tracked) &&
           image.cpu_addr < ~(1ULL << 40) && image.info.num_samples == 1 &&
           VideoCommon::CanUnswizzleImageRows(image.info, image.guest_size_bytes);
}

// A guest write the image takes partially: true when handled here (the image is then CpuModified
// and stays Tracked, with the written pages untracked).
template <class DeviceMemory>
bool Write(DeviceMemory& device_memory, VideoCommon::ImageBase& image, u32 id, u64 addr, u64 size) {
    using VideoCommon::ImageFlagBits;
    const auto it = images.find(id);
    if (it == images.end()) return false;
    State& state = it->second;
    if (True(image.flags & ImageFlagBits::CpuModified)) {
        if (state.pages.empty()) return false;
    } else if (!state.uploaded || !Eligible(image)) {
        return false;
    }
    image.flags |= ImageFlagBits::CpuModified;
    const u64 begin = (std::max)(addr, u64(image.cpu_addr));
    const u64 end = (std::min)(addr + size, u64(image.cpu_addr) + image.guest_size_bytes);
    for (u64 page = begin >> 12; begin < end && page <= (end - 1) >> 12; ++page) {
        const auto at = std::lower_bound(state.pages.begin(), state.pages.end(), page);
        if (at != state.pages.end() && *at == page) continue;
        state.pages.insert(at, page);
        device_memory.UpdatePagesCachedCount(page << 12, 4096, -1);
    }
    return true;
}

// Tracks the written pages again; returns them (empty when none).
template <class DeviceMemory>
std::vector<u64> End(DeviceMemory& device_memory, u32 id) {
    const auto it = images.find(id);
    if (it == images.end() || it->second.pages.empty()) return {};
    std::vector<u64> pages = std::move(it->second.pages);
    it->second.pages.clear();
    for (const u64 page : pages) device_memory.UpdatePagesCachedCount(page << 12, 4096, 1);
    return pages;
}

// The guest byte ranges, from the image start, that the pages cover.
inline std::vector<std::pair<u32, u32>> Ranges(std::span<const u64> pages, u64 cpu_addr, u64 size) {
    std::vector<std::pair<u32, u32>> ranges;
    for (const u64 page : pages) {
        const u64 begin = (std::max)(page << 12, cpu_addr) - cpu_addr;
        const u64 end = (std::min)((page + 1) << 12, cpu_addr + size) - cpu_addr;
        if (begin >= end) continue;
        if (!ranges.empty() && ranges.back().second == begin) {
            ranges.back().second = u32(end);
        } else {
            ranges.emplace_back(u32(begin), u32(end));
        }
    }
    return ranges;
}
} // namespace Eden::TexturePartial
