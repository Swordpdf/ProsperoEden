// ProsperoEden - RGBA images for textures: TGA files and box downsampling.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/gfx/image.hpp"

#include <cstdio>
#include <cstring>

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

} // namespace pe::gfx
