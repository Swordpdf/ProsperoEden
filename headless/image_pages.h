// Eden PS5 port: which pages of guest memory a texture cache image has ever been in.
//
// A guest read of memory the GPU caches follow asks the rasterizer whether something the GPU
// wrote must be copied back first (RasterizerVulkan::GetFlushArea). Only the texture cache can
// say so, for an image whose place in guest memory overlaps the read, and to ask it the guest
// thread takes the texture cache's lock, which the GPU thread holds through every draw. A large
// open-world game asks 700 to 3,000 times a frame, and a guest core's profile had the question
// and the wait for that lock as the largest thing the emulator itself costs it.
//
// The texture cache finds the images of a read through the places they were registered at
// (TextureCache::RegisterImage: one for an image, one for each piece of a sparse one). Every such
// place is marked here, a bit a 4 KiB page, and a mark is never taken away. A read whose pages
// have no mark cannot overlap an image that is or was registered, so the texture cache would
// find none and change nothing: the answer is given without asking, and without the lock.
// Marks are set inside the registration, under the texture cache's lock; a read that looks just
// before one is set is a read that came before the registration.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Eden::ImagePages {
inline constexpr unsigned kPageBits = 12;                          // Core::DEVICE_PAGEBITS
inline constexpr std::uint64_t kSpace = std::uint64_t{1} << 34;    // the device address space
inline constexpr std::size_t kWords = static_cast<std::size_t>(kSpace >> kPageBits) / 64;
inline std::array<std::atomic<std::uint64_t>, kWords> held{};

// Bits first to last of a word.
constexpr std::uint64_t Bits(unsigned first, unsigned last) noexcept {
    return (~std::uint64_t{0} >> (63 - last)) & (~std::uint64_t{0} << first);
}

// An image, or one piece of a sparse image, is registered at [address, address + size).
inline void Mark(std::uint64_t address, std::uint64_t size) noexcept {
    if (size == 0 || address >= kSpace) return;
    const std::uint64_t first = address >> kPageBits;
    const std::uint64_t last = ((std::min)(size, kSpace - address) + address - 1) >> kPageBits;
    for (std::uint64_t word = first / 64; word <= last / 64; ++word) {
        const std::uint64_t bits = Bits(word == first / 64 ? static_cast<unsigned>(first % 64) : 0,
                                        word == last / 64 ? static_cast<unsigned>(last % 64) : 63);
        if ((held[word].load(std::memory_order_relaxed) & bits) != bits) held[word].fetch_or(bits);
    }
}

// Whether no image was ever registered in the pages of [address, address + size). False for a
// range the map does not cover.
inline bool NeverHeld(std::uint64_t address, std::uint64_t size) noexcept {
    if (size == 0) size = 1;
    if (address >= kSpace || size > kSpace - address) return false;
    const std::uint64_t first = address >> kPageBits;
    const std::uint64_t last = (address + size - 1) >> kPageBits;
    for (std::uint64_t word = first / 64; word <= last / 64; ++word) {
        const std::uint64_t bits = Bits(word == first / 64 ? static_cast<unsigned>(first % 64) : 0,
                                        word == last / 64 ? static_cast<unsigned>(last % 64) : 63);
        if (held[word].load(std::memory_order_acquire) & bits) return false;
    }
    return true;
}
} // namespace Eden::ImagePages
