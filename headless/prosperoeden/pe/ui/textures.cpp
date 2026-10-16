// ProsperoEden - The launcher's textures: its own art and lazily loaded covers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/textures.hpp"

#include "pe/ui/theme.hpp"

#include "pe/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <utility>

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

void Textures::load_game_backdrop(const std::string &cover_path)
{
    if (cover_path == game_backdrop_path_)
        return;
    clear_game_backdrop();
    game_backdrop_path_ = cover_path;
    if (cover_path.empty())
        return;
    gfx::Image image;
    if (!services_.load_image(cover_path, &image))
        return;
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4)
    {
        image.rgba[i + 0] = static_cast<std::uint8_t>(image.rgba[i + 0] * 0.35f);
        image.rgba[i + 1] = static_cast<std::uint8_t>(image.rgba[i + 1] * 0.35f);
        image.rgba[i + 2] = static_cast<std::uint8_t>(image.rgba[i + 2] * 0.35f);
    }
    gfx::blur(image, image.width / 32);
    game_backdrop_ = create(image);
    gfx::Image blurred = gfx::halve(image);
    game_backdrop_blur_ = create(blurred);
}

void Textures::clear_game_backdrop()
{
    batch_.delete_texture(game_backdrop_);
    batch_.delete_texture(game_backdrop_blur_);
    game_backdrop_ = 0;
    game_backdrop_blur_ = 0;
    game_backdrop_path_.clear();
}

void Textures::load_wallpaper(const std::string &path, bool blurred)
{
    if (path == wallpaper_path_ && blurred == wallpaper_shown_blurred_)
        return;
    if (path.empty())
    {
        clear_wallpaper();
        return;
    }
    // A finished read: taken when it is the wanted one, and dropped when the choice moved on
    // while it was being prepared. The wait never blocks: it only asks whether it is done.
    if (wallpaper_task_.valid() &&
        wallpaper_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        WallpaperImage done = wallpaper_task_.get();
        if (done.path == path && done.blurred == blurred)
        {
            wallpaper_ = create(done.image);
            wallpaper_blur_ = create(done.frosted);
            wallpaper_w_ = done.image.width;
            wallpaper_h_ = done.image.height;
            wallpaper_path_ = std::move(done.path);
            wallpaper_shown_blurred_ = done.blurred;
            return;
        }
    }
    // One read at a time: a running one is replaced only after it finished and turned out
    // stale, so flipping through the list never queues up decodes.
    if (!wallpaper_task_.valid())
        wallpaper_task_ = std::async(std::launch::async, [path, blurred]
        {
            WallpaperImage done;
            done.path = path;
            done.blurred = blurred;
            gfx::Image image;
            if (!gfx::load_image(path, &image))
            {
                sys::log("the wallpaper %s could not be read", path.c_str());
                return done;
            }
            // The screens show at most 2048 pixels a side of it; Backdrop resizes the whole of
            // it to the screen, so no part is cut.
            while (image.width > 2048 || image.height > 2048)
                image = gfx::halve(image);
            if (blurred)
            {
                // The blur hides the detail: a smaller copy reads the same and softens faster.
                while (image.width > 1024)
                    image = gfx::halve(image);
                gfx::blur(image, std::max(4, image.width / 64));
                done.frosted = gfx::halve(image);
            }
            else
            {
                done.frosted = gfx::halve(image);
                gfx::blur(done.frosted, std::max(4, done.frosted.width / 12));
            }
            done.image = std::move(image);
            return done;
        });
}

void Textures::clear_wallpaper()
{
    batch_.delete_texture(wallpaper_);
    batch_.delete_texture(wallpaper_blur_);
    wallpaper_ = 0;
    wallpaper_blur_ = 0;
    wallpaper_w_ = 0;
    wallpaper_h_ = 0;
    wallpaper_path_.clear();
    wallpaper_shown_blurred_ = false;
}

void Textures::release()
{
    drop_splash();
    clear_game_backdrop();
    clear_wallpaper();
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
