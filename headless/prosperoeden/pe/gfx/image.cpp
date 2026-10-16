// ProsperoEden - RGBA images for textures: image files, TGA files and box downsampling.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/gfx/image.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// The wallpaper's image files (docs/WALLPAPERS.md). The loader's implementation stays local to
// this translation unit: the shared library also ships stb_image, and its symbols stay global.
// The third-party code is quiet under the build's -Werror.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 16384
#include "stb/stb_image.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec/bcdec.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace pe::gfx
{

bool load_dds_bc7(const std::string &path, Blocks *blocks)
{
    *blocks = Blocks{};
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    // "DDS ", a header of 124 bytes, then the newer header of 20 that names the format.
    unsigned char header[148]{};
    const auto word = [&header](int at)
    {
        return static_cast<std::uint32_t>(header[at]) | static_cast<std::uint32_t>(header[at + 1]) << 8 |
               static_cast<std::uint32_t>(header[at + 2]) << 16 | static_cast<std::uint32_t>(header[at + 3]) << 24;
    };
    bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
              std::memcmp(header, "DDS ", 4) == 0 && std::memcmp(header + 84, "DX10", 4) == 0;
    const std::uint32_t height = word(12);
    const std::uint32_t width = word(16);
    const std::uint32_t format = word(128); // 98: BC7, 99: BC7 with the sRGB curve named
    ok = ok && (format == 98 || format == 99) && width > 0 && height > 0 && width <= 8192 && height <= 8192;
    if (ok)
    {
        blocks->bc7.resize(static_cast<std::size_t>((width + 3) / 4) * static_cast<std::size_t>((height + 3) / 4) * 16);
        ok = std::fread(blocks->bc7.data(), 1, blocks->bc7.size(), file) == blocks->bc7.size();
    }
    std::fclose(file);
    if (!ok)
    {
        *blocks = Blocks{};
        return false;
    }
    blocks->width = static_cast<int>(width);
    blocks->height = static_cast<int>(height);
    return true;
}

bool load_tga(const std::string &path, Image *image)
{
    *image = Image{};
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    unsigned char header[18]{};
    bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    const int width = header[12] | (header[13] << 8);
    const int height = header[14] | (header[15] << 8);
    const int bytes = header[16] / 8;
    // No colour map, uncompressed true colour, at most 8192 pixels a side.
    ok = ok && header[1] == 0 && header[2] == 2 && (bytes == 3 || bytes == 4) && width > 0 &&
         height > 0 && width <= 8192 && height <= 8192;
    ok = ok && std::fseek(file, header[0], SEEK_CUR) == 0; // image ID field
    std::vector<std::uint8_t> raw;
    if (ok)
    {
        raw.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                   static_cast<std::size_t>(bytes));
        ok = std::fread(raw.data(), 1, raw.size(), file) == raw.size();
    }
    std::fclose(file);
    if (!ok)
        return false;

    const bool top_first = (header[17] & 0x20) != 0;
    image->width = width;
    image->height = height;
    image->rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y)
    {
        const int source_row = top_first ? y : height - 1 - y;
        const std::uint8_t *in =
            raw.data() + static_cast<std::size_t>(source_row) * static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(bytes);
        std::uint8_t *out =
            image->rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x, in += bytes, out += 4)
        {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = bytes == 4 ? in[3] : 255;
        }
    }
    return true;
}

namespace
{

// The file's extension in lower case (".png"), or none.
std::string Extension(const std::string &path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 == path.size())
        return {};
    std::string extension = path.substr(dot);
    for (char &c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

// A BC7 DDS through bcdec: the blocks the header reader took, one tile at a time.
bool LoadDdsImage(const std::string &path, Image *image)
{
    Blocks blocks;
    if (!load_dds_bc7(path, &blocks))
        return false;
    const int width = blocks.width;
    const int height = blocks.height;
    const int columns = (width + 3) / 4;
    image->width = width;
    image->height = height;
    image->rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    std::uint8_t tile[64];
    for (int y = 0; y < height; y += 4)
    {
        for (int x = 0; x < width; x += 4)
        {
            const std::size_t block = (static_cast<std::size_t>(y / 4) * static_cast<std::size_t>(columns) +
                                       static_cast<std::size_t>(x / 4)) *
                                      16;
            bcdec_bc7(blocks.bc7.data() + block, tile, 16);
            for (int in_y = 0; in_y < 4 && y + in_y < height; ++in_y)
                for (int in_x = 0; in_x < 4 && x + in_x < width; ++in_x)
                {
                    std::uint8_t *out =
                        image->rgba.data() +
                        (static_cast<std::size_t>(y + in_y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x + in_x)) *
                            4;
                    std::memcpy(out, tile + (in_y * 4 + in_x) * 4, 4);
                }
        }
    }
    return true;
}

} // namespace

bool load_image(const std::string &path, Image *image)
{
    *image = Image{};
    if (path.empty())
        return false;
    if (Extension(path) == ".dds")
        return LoadDdsImage(path, image);
    int width = 0;
    int height = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, nullptr, 4);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        stbi_image_free(pixels);
        return false;
    }
    image->width = width;
    image->height = height;
    image->rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(pixels);
    while (image->width > 8192 || image->height > 8192)
        *image = halve(*image);
    return !image->empty();
}

Image halve(const Image &image)
{
    Image result;
    result.width = image.width / 2;
    result.height = image.height / 2;
    if (result.empty())
        return Image{};
    result.rgba.resize(static_cast<std::size_t>(result.width) *
                       static_cast<std::size_t>(result.height) * 4);
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    for (int y = 0; y < result.height; ++y)
    {
        const std::uint8_t *row = image.rgba.data() + static_cast<std::size_t>(y) * 2 * stride;
        std::uint8_t *out =
            result.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(result.width) * 4;
        for (int x = 0; x < result.width; ++x, out += 4)
        {
            const std::uint8_t *block[4] = {row + static_cast<std::size_t>(x) * 8,
                                            row + static_cast<std::size_t>(x) * 8 + 4,
                                            row + stride + static_cast<std::size_t>(x) * 8,
                                            row + stride + static_cast<std::size_t>(x) * 8 + 4};
            unsigned alpha = 0;
            unsigned color[3] = {0, 0, 0};
            for (const std::uint8_t *pixel : block)
            {
                alpha += pixel[3];
                for (int channel = 0; channel < 3; ++channel)
                    color[channel] += static_cast<unsigned>(pixel[channel]) * pixel[3];
            }
            for (int channel = 0; channel < 3; ++channel)
                out[channel] =
                    static_cast<std::uint8_t>(alpha != 0 ? (color[channel] + alpha / 2) / alpha : 0);
            out[3] = static_cast<std::uint8_t>((alpha + 2) / 4);
        }
    }
    return result;
}

// Horizontal box blur: for each pixel, average the clamped [x-radius, x+radius] window. The
// window's sum slides along the row, so a big radius costs no more than a small one.
static void blur_row(const std::uint8_t *in, std::uint8_t *out, int width, int radius)
{
    if (width <= 0)
        return;
    int lo = 0;
    int hi = std::min(width - 1, radius);
    unsigned sum[3] = {0, 0, 0};
    for (int k = lo; k <= hi; ++k)
        for (int ch = 0; ch < 3; ++ch)
            sum[ch] += in[k * 4 + ch];
    for (int x = 0; x < width; ++x)
    {
        const int count = hi - lo + 1;
        for (int ch = 0; ch < 3; ++ch)
            out[x * 4 + ch] = static_cast<std::uint8_t>(sum[ch] / static_cast<unsigned>(count));
        out[x * 4 + 3] = in[x * 4 + 3];
        const int next_lo = std::max(0, x + 1 - radius);
        const int next_hi = std::min(width - 1, x + 1 + radius);
        for (; lo < next_lo; ++lo)
            for (int ch = 0; ch < 3; ++ch)
                sum[ch] -= in[lo * 4 + ch];
        for (int k = hi + 1; k <= next_hi; ++k)
            for (int ch = 0; ch < 3; ++ch)
                sum[ch] += in[k * 4 + ch];
        hi = next_hi;
    }
}

void blur(Image &image, int radius)
{
    if (image.empty() || radius <= 0)
        return;
    radius = std::min(radius, std::min(image.width, image.height) / 2);
    if (radius <= 0)
        return;
    const int w = image.width;
    const int h = image.height;
    std::vector<std::uint8_t> tmp(image.rgba.size());
    std::vector<std::uint8_t> col(static_cast<std::size_t>(h) * 4);
    std::vector<std::uint8_t> col_out(col.size());
    auto row_ptr = [&](auto &buf, int y) { return buf.data() + static_cast<std::size_t>(y) * w * 4; };
    for (int pass = 0; pass < 3; ++pass)
    {
        for (int y = 0; y < h; ++y)
            blur_row(row_ptr(image.rgba, y), row_ptr(tmp, y), w, radius);
        for (int x = 0; x < w; ++x)
        {
            for (int y = 0; y < h; ++y)
                std::memcpy(col.data() + y * 4, row_ptr(tmp, y) + x * 4, 4);
            blur_row(col.data(), col_out.data(), h, radius);
            for (int y = 0; y < h; ++y)
                std::memcpy(row_ptr(image.rgba, y) + x * 4, col_out.data() + y * 4, 4);
        }
    }
}

} // namespace pe::gfx
