// ProsperoEden - RGBA images for textures: TGA files and box downsampling.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pe::gfx
{

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // straight alpha, top row first
    bool empty() const
    {
        return width <= 0 || height <= 0;
    }
};

// Reads an uncompressed true-colour TGA (24 or 32 bit, either row order): the
// format of the launcher's own art and of the covers extracted from games.
bool load_tga(const std::string &path, Image *image);

// A picture kept compressed, as the graphics card reads it: BC7 blocks, top row first.
struct Blocks
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bc7;
};

// Reads a DDS file holding one BC7 picture (the format of the app's sce_sys pictures) without
// decoding it. False for anything else.
bool load_dds_bc7(const std::string &path, Blocks *blocks);

// Half the size in each direction, averaging 2x2 blocks (alpha-weighted).
Image halve(const Image &image);

} // namespace pe::gfx
