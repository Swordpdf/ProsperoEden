// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe/gfx/image.hpp"
#include <cstdio>
#include <stdexcept>

namespace pe::ui {
// Check decoded size before the shared decoder allocates either pixel buffer.
inline gfx::Image load_runtime_image(const std::string& path, int max_width, int max_height,
                                     std::size_t remaining) {
    unsigned char header[18]{};
    std::FILE* file = std::fopen(path.c_str(), "rb");
    const bool readable = file && std::fread(header, 1, sizeof(header), file) == sizeof(header);
    if (file) std::fclose(file);
    const int width = header[12] | (header[13] << 8);
    const int height = header[14] | (header[15] << 8);
    if (!readable || width < 1 || height < 1 || width > max_width || height > max_height ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4 > remaining)
        throw std::runtime_error("Launcher image exceeds resource limits");
    gfx::Image image;
    if (!gfx::load_tga(path, &image)) throw std::runtime_error("Invalid launcher image");
    return image;
}
}
