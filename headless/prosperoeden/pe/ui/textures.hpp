// ProsperoEden - The launcher's textures: its own art and lazily loaded covers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/gl_batch.hpp"
#include "pe/ui/services.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pe::ui
{

// A cover as far as it is known.
struct Cover
{
    std::uint32_t texture = 0; // 0 while it is queued, and when there is none
    float age = 0.0f;          // seconds since it loaded, for the fade-in
    bool missing = false;      // the game has no cover, or its file cannot be read
};

// Owns every GL texture of the launcher. Covers load a few per frame, at the
// size they are shown, and the least recently drawn ones are dropped.
class Textures
{
  public:
    Textures(gfx::GlBatch &batch, Services &services) : batch_(batch), services_(services) {}
    Textures(const Textures &) = delete;
    Textures &operator=(const Textures &) = delete;
    ~Textures();

    // The background art, its blurred copy (frosted panels) and the app icon.
    bool load_art(const std::string &directory);
    // The picture the console shows while the app starts (sce_sys/pic0.dds), for the opening:
    // the launcher's first frame is that picture, so the app appears without a cut.
    bool load_splash(const std::string &path);
    // The opening is over: the picture is not needed again.
    void drop_splash();
    // Deletes every texture; the GL context must still be current.
    void release();

    std::uint32_t backdrop() const
    {
        return backdrop_;
    }
    std::uint32_t backdrop_blur() const
    {
        return backdrop_blur_;
    }
    std::uint32_t brand() const
    {
        return brand_;
    }
    // 0 when the launch picture was not loaded.
    std::uint32_t splash() const
    {
        return splash_;
    }
    // A controller in white on transparent (0 when the picture is missing).
    std::uint32_t controller() const
    {
        return controller_;
    }

    // The cover drawn `size` virtual pixels wide. Queues it on first use.
    Cover cover(const std::string &path, float size);
    // Loads up to `budget` queued covers and ages the loaded ones.
    void pump(float dt, int budget = 2);
    // Output pixels per virtual pixel: picks how much detail a cover needs.
    void set_output_scale(float scale)
    {
        output_scale_ = scale;
    }

  private:
    struct Entry
    {
        std::uint32_t texture = 0;
        float age = 0.0f;
        bool loaded = false; // false: queued
        std::uint64_t used = 0;
        std::string path;
        int level = 0;
    };

    std::uint32_t create(const gfx::Image &image);

    gfx::GlBatch &batch_;
    Services &services_;
    std::uint32_t backdrop_ = 0;
    std::uint32_t backdrop_blur_ = 0;
    std::uint32_t brand_ = 0;
    std::uint32_t controller_ = 0;
    std::uint32_t splash_ = 0;
    std::unordered_map<std::string, Entry> covers_;
    std::vector<std::string> queue_;
    std::uint64_t frame_ = 0;
    float output_scale_ = 1.0f;
};

} // namespace pe::ui
