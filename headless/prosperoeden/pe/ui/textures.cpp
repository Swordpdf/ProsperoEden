// ProsperoEden - The launcher's textures: its own art and lazily loaded covers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/textures.hpp"

#include "pe/ui/theme.hpp"

#include "pe/core/log.hpp"

#include <algorithm>

namespace pe::ui
{

namespace
{

constexpr std::size_t kMaxCovers = 160;

} // namespace

Textures::~Textures()
{
    release();
}

std::uint32_t Textures::create(const gfx::Image &image)
{
    return image.empty() ? 0 : batch_.create_texture(image.width, image.height, image.rgba.data());
}

bool Textures::load_art(const std::string &directory)
{
    const auto load = [&](const std::string &name, std::uint32_t *texture)
    {
        gfx::Image image;
        if (!gfx::load_tga(directory + "/" + name, &image))
        {
            sys::log("art %s could not be read", name.c_str());
            return false;
        }
        *texture = create(image);
        return *texture != 0;
    };
    // The theme names its artwork (theme.hpp).
    const std::string art = "art/";
    const bool backdrop = load(art + th().backdrop, &backdrop_);
    const bool blur = load(art + th().backdrop_blur, &backdrop_blur_);
    const bool brand = load(art + th().brand, &brand_);
    // Only the home screen's controller display needs this one.
    (void)load("art/controller.tga", &controller_);
    return backdrop && blur && brand;
}

bool Textures::load_splash(const std::string &path)
{
    drop_splash();
    gfx::Blocks blocks;
    if (!gfx::load_dds_bc7(path, &blocks))
    {
        sys::log("the launch picture %s could not be read", path.c_str());
        return false;
    }
    splash_ = batch_.create_bc7_texture(blocks.width, blocks.height, blocks.bc7.data(), blocks.bc7.size());
    if (splash_ == 0)
        sys::log("the launch picture was not taken as a texture");
    return splash_ != 0;
}

void Textures::drop_splash()
{
    batch_.delete_texture(splash_);
    splash_ = 0;
}

void Textures::release()
{
    drop_splash();
    for (std::uint32_t *texture : {&backdrop_, &backdrop_blur_, &brand_, &controller_})
    {
        batch_.delete_texture(*texture);
        *texture = 0;
    }
    for (auto &[key, entry] : covers_)
        batch_.delete_texture(entry.texture);
    covers_.clear();
    queue_.clear();
}

Cover Textures::cover(const std::string &path, float size)
{
    if (path.empty())
        return {0, 0.0f, true};
    // Covers are 256 pixels a side: draw a small one from a halved copy, which
    // a single-level texture cannot do for itself.
    const float pixels = size * output_scale_;
    const int level = pixels > 128.0f ? 0 : pixels > 64.0f ? 1 : 2;
    const std::string key = path + '#' + static_cast<char>('0' + level);
    auto it = covers_.find(key);
    if (it == covers_.end())
    {
        Entry entry;
        entry.path = path;
        entry.level = level;
        it = covers_.emplace(key, std::move(entry)).first;
        queue_.push_back(key);
    }
    it->second.used = frame_;
    return {it->second.texture, it->second.age, it->second.loaded && it->second.texture == 0};
}

void Textures::pump(float dt, int budget)
{
    ++frame_;
    for (auto &[key, entry] : covers_)
        if (entry.loaded)
            entry.age += dt;
    while (budget > 0 && !queue_.empty())
    {
        const std::string key = queue_.front();
        queue_.erase(queue_.begin());
        const auto it = covers_.find(key);
        if (it == covers_.end() || it->second.loaded)
            continue;
        Entry &entry = it->second;
        entry.loaded = true;
        gfx::Image image;
        if (!services_.load_image(entry.path, &image))
            continue; // stays without a texture: the placeholder is drawn
        for (int level = 0; level < entry.level && image.width > 64; ++level)
            image = gfx::halve(image);
        entry.texture = create(image);
        --budget;
    }
    if (covers_.size() > kMaxCovers)
    {
        // Drop the covers not drawn for the longest time.
        std::vector<std::pair<std::uint64_t, std::string>> order;
        for (const auto &[key, entry] : covers_)
            if (entry.loaded && entry.used + 2 < frame_)
                order.emplace_back(entry.used, key);
        std::sort(order.begin(), order.end());
        for (std::size_t index = 0; index < order.size() && covers_.size() > kMaxCovers * 3 / 4;
             ++index)
        {
            batch_.delete_texture(covers_[order[index].second].texture);
            covers_.erase(order[index].second);
        }
    }
}

} // namespace pe::ui
