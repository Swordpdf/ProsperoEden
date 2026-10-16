// ProsperoEden - The launcher's textures: its own art and lazily loaded covers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/gl_batch.hpp"
#include "pe/ui/services.hpp"

#include <cstdint>
#include <future>
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
        return game_backdrop_ ? game_backdrop_ : wallpaper_ ? wallpaper_ : backdrop_;
    }
    std::uint32_t backdrop_blur() const
    {
        return game_backdrop_blur_ ? game_backdrop_blur_ :
               wallpaper_blur_ ? wallpaper_blur_ :
                                 backdrop_blur_;
    }
    // A picture of the user's own is showing: the wallpaper or a game's cover. The shade over the
    // artwork eases off for these so the picture stays visible behind the screens.
    bool custom_backdrop() const { return wallpaper_ != 0 || game_backdrop_ != 0; }
    // The wallpaper's pixel size while it alone is the backdrop: Backdrop resizes the whole of it
    // to the screen. A game's cover still fills the art rect, as it did. Zero when none shows.
    int wallpaper_width() const { return !game_backdrop_ && wallpaper_ ? wallpaper_w_ : 0; }
    int wallpaper_height() const { return wallpaper_width() ? wallpaper_h_ : 0; }

    // The wallpaper behind the screens (Settings > Appearance > Wallpaper): an image from the
    // wallpapers folder, over the theme's own art and under a game's cover. `blurred` shows the
    // image itself blurred. It is read and prepared on a thread of its own, so picking a file
    // never holds the interface; the textures appear once it was read.
    void load_wallpaper(const std::string &path, bool blurred);
    void clear_wallpaper();

    void load_game_backdrop(const std::string &cover_path);
    void clear_game_backdrop();
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

    // One wallpaper read in the background: the file it came from, the settings asked for, and
    // the two copies made from it (empty images when the file could not be read).
    struct WallpaperImage
    {
        std::string path;
        bool blurred = false;
        gfx::Image image;   // the wallpaper itself
        gfx::Image frosted; // its softer copy for the frosted panels
    };

    std::uint32_t create(const gfx::Image &image);

    gfx::GlBatch &batch_;
    Services &services_;
    std::uint32_t backdrop_ = 0;
    std::uint32_t backdrop_blur_ = 0;
    std::uint32_t wallpaper_ = 0;
    std::uint32_t wallpaper_blur_ = 0;     // the wallpaper's frosted-glass copy
    int wallpaper_w_ = 0;                  // its pixel size while shown (Backdrop fits it)
    int wallpaper_h_ = 0;
    std::string wallpaper_path_;           // the file shown (kept when the read failed)
    bool wallpaper_shown_blurred_ = false; // the wallpaper itself shown blurred
    std::future<WallpaperImage> wallpaper_task_; // the read running for it, one at a time
    std::uint32_t game_backdrop_ = 0;
    std::uint32_t game_backdrop_blur_ = 0;
    std::string game_backdrop_path_;
    std::uint32_t brand_ = 0;
    std::uint32_t controller_ = 0;
    std::uint32_t splash_ = 0;
    std::unordered_map<std::string, Entry> covers_;
    std::vector<std::string> queue_;
    std::uint64_t frame_ = 0;
    float output_scale_ = 1.0f;
};

} // namespace pe::ui
