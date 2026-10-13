// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The picture of the loading screen (loading_scene.glsl): the game's cover and, beside it in the
// same texture, a small blurred copy that the screen spreads behind everything. Built once per
// start from the cover the launcher keeps of the game; both graphics backends upload it as it is.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "hud.h"

namespace Eden::Loading {
// The blurred copy is this many texels a side and stands this many to the right of the cover
// (loading_scene.glsl knows both).
inline constexpr int kArtBlur = 64;
inline constexpr int kArtGap = 2;
inline constexpr int kArtLargest = 512;  // a larger cover is reduced to this

struct Art {
    int width{1}, height{1};            // the texture
    int cover_width{}, cover_height{};  // the cover in its top left corner; 0: the game has none
    std::vector<uint8_t> rgba = std::vector<uint8_t>(4);  // rows from the top
};

// A cover as the launcher writes them (metadata_bridge.cpp): an uncompressed TGA of 32 bits.
// Returns its pixels as RGBA with rows from the top; false when the file is anything else.
inline bool ReadCover(const std::string& path, int& width, int& height, std::vector<uint8_t>& rgba) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    unsigned char header[18]{};
    bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header) && header[1] == 0 && header[2] == 2 &&
              header[16] == 32;
    width = header[12] | header[13] << 8;
    height = header[14] | header[15] << 8;
    ok = ok && width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
         std::fseek(file, header[0], SEEK_CUR) == 0;
    if (ok) {
        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        ok = std::fread(rgba.data(), 1, rgba.size(), file) == rgba.size();
    }
    std::fclose(file);
    if (!ok) return false;
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);  // stored blue first
    if (!(header[17] & 0x20)) {
        // Rows from the bottom: turn the picture over.
        const size_t row = static_cast<size_t>(width) * 4;
        for (int y = 0; y < height / 2; ++y)
            std::swap_ranges(rgba.begin() + static_cast<ptrdiff_t>(row * static_cast<size_t>(y)),
                             rgba.begin() + static_cast<ptrdiff_t>(row * static_cast<size_t>(y + 1)),
                             rgba.begin() + static_cast<ptrdiff_t>(row * static_cast<size_t>(height - 1 - y)));
    }
    return true;
}

// The texture for a cover of width x height RGBA pixels (rows from the top).
inline Art MakeArt(const uint8_t* cover, int width, int height) {
    Art art;
    if (!cover || width <= 0 || height <= 0) return art;
    // The mean of a block of the cover, over the dark a clear pixel shows.
    const auto mean = [&](int x0, int y0, int x1, int y1, float out[4]) {
        x1 = std::max(x1, x0 + 1);
        y1 = std::max(y1, y0 + 1);
        float sum[4]{};
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const uint8_t* pixel = cover + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                                static_cast<size_t>(x)) * 4;
                const float alpha = static_cast<float>(pixel[3]) / 255.0f;
                for (int k = 0; k < 3; ++k) sum[k] += static_cast<float>(pixel[k]) * alpha;
                sum[3] += static_cast<float>(pixel[3]);
            }
        const float count = static_cast<float>((x1 - x0) * (y1 - y0));
        for (int k = 0; k < 4; ++k) out[k] = sum[k] / count;
    };
    int step = 1;
    while (std::max(width, height) / step > kArtLargest) ++step;
    art.cover_width = std::max(1, width / step);
    art.cover_height = std::max(1, height / step);
    art.width = art.cover_width + kArtGap + kArtBlur;
    art.height = std::max(art.cover_height, kArtBlur);
    art.rgba.assign(static_cast<size_t>(art.width) * static_cast<size_t>(art.height) * 4, 0);
    const auto at = [&](int x, int y) {
        return art.rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(art.width) + static_cast<size_t>(x)) * 4;
    };
    for (int y = 0; y < art.cover_height; ++y)
        for (int x = 0; x < art.cover_width; ++x) {
            uint8_t* out = at(x, y);
            if (step == 1) {
                std::copy_n(cover + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4,
                            4, out);
                continue;
            }
            float value[4]{};
            mean(x * step, y * step, x * step + step, y * step + step, value);
            // Colours were weighted by their cover: undo it for the stored pixel.
            const float alpha = std::max(value[3] / 255.0f, 1.0f / 255.0f);
            for (int k = 0; k < 3; ++k)
                out[k] = static_cast<uint8_t>(std::clamp(value[k] / alpha + 0.5f, 0.0f, 255.0f));
            out[3] = static_cast<uint8_t>(value[3] + 0.5f);
        }
    // The blurred copy: the whole cover in kArtBlur texels a side, then a wide Gaussian.
    std::vector<float> small(static_cast<size_t>(kArtBlur) * kArtBlur * 3), pass(small.size());
    for (int y = 0; y < kArtBlur; ++y)
        for (int x = 0; x < kArtBlur; ++x) {
            float value[4]{};
            mean(x * width / kArtBlur, y * height / kArtBlur, (x + 1) * width / kArtBlur, (y + 1) * height / kArtBlur,
                 value);
            for (int k = 0; k < 3; ++k) small[(static_cast<size_t>(y) * kArtBlur + static_cast<size_t>(x)) * 3 + k] = value[k];
        }
    constexpr int kReach = 12;
    constexpr float kSigma = 4.5f;
    float weights[kReach + 1];
    float total = 0;
    for (int i = 0; i <= kReach; ++i) {
        weights[i] = std::exp(-static_cast<float>(i * i) / (2.0f * kSigma * kSigma));
        total += i ? 2.0f * weights[i] : weights[i];
    }
    const auto blur = [&](const std::vector<float>& from, std::vector<float>& to, bool down) {
        for (int y = 0; y < kArtBlur; ++y)
            for (int x = 0; x < kArtBlur; ++x)
                for (int k = 0; k < 3; ++k) {
                    float sum = 0;
                    for (int i = -kReach; i <= kReach; ++i) {
                        const int sx = down ? x : std::clamp(x + i, 0, kArtBlur - 1);
                        const int sy = down ? std::clamp(y + i, 0, kArtBlur - 1) : y;
                        sum += weights[i < 0 ? -i : i] *
                               from[(static_cast<size_t>(sy) * kArtBlur + static_cast<size_t>(sx)) * 3 + k];
                    }
                    to[(static_cast<size_t>(y) * kArtBlur + static_cast<size_t>(x)) * 3 + k] = sum / total;
                }
    };
    blur(small, pass, false);
    blur(pass, small, true);
    for (int y = 0; y < kArtBlur; ++y)
        for (int x = 0; x < kArtBlur; ++x) {
            uint8_t* out = at(art.cover_width + kArtGap + x, y);
            for (int k = 0; k < 3; ++k)
                out[k] = static_cast<uint8_t>(std::clamp(
                    small[(static_cast<size_t>(y) * kArtBlur + static_cast<size_t>(x)) * 3 + k] + 0.5f, 0.0f, 255.0f));
            out[3] = 255;
        }
    return art;
}

// The picture of the start under way. The renderers copy it when they build the loading screen;
// the state the shader is given (hud.h, Loading::State) says how large the cover in it is.
inline std::mutex& ArtLock() {
    static std::mutex lock;
    return lock;
}
inline Art& ArtStore() {
    static Art art;
    return art;
}
inline Art GetArt() {
    const std::scoped_lock held{ArtLock()};
    return ArtStore();
}
inline void SetArt(Art next) {
    const std::scoped_lock held{ArtLock()};
    art_size.store(static_cast<uint32_t>(next.cover_width) | static_cast<uint32_t>(next.cover_height) << 16,
                   std::memory_order_relaxed);
    ArtStore() = std::move(next);
}
// From the cover the launcher keeps of a game; a game without one has no picture.
inline void SetArtFromCover(const std::string& cover_path) {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    SetArt(ReadCover(cover_path, width, height, rgba) ? MakeArt(rgba.data(), width, height) : Art{});
}
} // namespace Eden::Loading
