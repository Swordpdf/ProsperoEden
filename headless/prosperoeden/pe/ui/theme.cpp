// ProsperoEden - The launcher's themes, and which one is in use.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/theme.hpp"

#include "pe/core/strings.hpp"

#include <array>
#include <cstdint>

namespace pe::ui
{

namespace
{

Color rgb(std::uint32_t hex, float alpha = 1.0f)
{
    return Color::rgb(hex, alpha);
}

// Slate: neutral glass, clean and still.
Theme slate()
{
    Theme t;
    theme_from_kit("acrylic", &t);
    t.id = "slate";
    t.name = "Slate";
    t.summary = TR("Neutral glass, clean and still.");

    t.base = rgb(0x101012);
    t.scrim = rgb(0x080809);
    t.glass = rgb(0x1c1c20);
    t.panel = rgb(0x141416);
    t.panel_edge = rgb(0x3a3a3e);
    t.row = rgb(0x1e1e22);
    t.row_edge = rgb(0x34343a);
    t.row_focus = rgb(0x2c2c32);

    t.accent = rgb(0xc0bbb6);
    t.accent_deep = rgb(0x3a3836);
    t.accent_pale = rgb(0xdedbd8);
    t.on_accent = rgb(0x101012);
    t.highlight = rgb(0xe8e4e0);

    t.text = rgb(0xeae8e6);
    t.title = rgb(0xf2f0ee);
    t.value = rgb(0xe6e4e2);
    t.body = rgb(0xd0cecc);
    t.copy = rgb(0xb0aeac);
    t.meta = rgb(0x98969a);
    t.label = rgb(0x88868a);
    t.muted = rgb(0x78767a);
    t.faint = rgb(0x585660);
    t.warning = rgb(0xe0685e);
    t.rule = rgb(0x34343a);

    t.size_hero = 64.0f;
    t.size_display = 38.0f;
    t.size_heading = 28.0f;

    t.focus_glow = 18.0f;
    t.shadow_blur = 36.0f;
    t.shadow_offset = 14.0f;
    t.pill_chips = true;
    t.motes = 0;
    t.drift = 0.0f;

    t.list_settings = true;
    t.game_backdrop = false;
    t.minimal_home = true;
    t.top_nav = true;
    t.scrim_opacity = 0.92f;
    t.game_page_offset = 60.0f;

    t.backdrop = "slate-backdrop.tga";
    t.backdrop_blur = "slate-backdrop-blur.tga";
    t.brand = "brand.tga";
    t.light_x = 0.5f;
    t.light_y = 0.4f;
    return t;
}

// Canopy: a tropical dusk, dark green glass and lime. ProsperoEden's own look, and the one a
// part missing from any other theme falls back to.
Theme canopy()
{
    Theme t;
    // The kit's frosted glass: translucent panels over the artwork, soft shadows, a ring of light
    // for the focus, movement that settles without a bounce.
    theme_from_kit("acrylic", &t);
    t.id = "canopy";
    t.name = "Canopy";
    t.summary = TR("Tropical dusk, dark green glass and lime.");

    t.base = rgb(0x06090a);
    t.scrim = rgb(0x010806);
    t.glass = rgb(0x16221d);
    t.panel = rgb(0x0b1713);
    t.panel_edge = rgb(0x768e75);
    t.row = rgb(0x17241e);
    t.row_edge = rgb(0x6a8267);
    t.row_focus = rgb(0x294433);

    t.accent = rgb(0xa9db63);
    t.accent_deep = rgb(0x245d4a);
    t.accent_pale = rgb(0xdfe8a6);
    t.on_accent = rgb(0x0b1713);
    t.highlight = rgb(0xffd76a);

    t.text = rgb(0xf0f5f2);
    t.title = rgb(0xfffcef);
    t.value = rgb(0xf2f4e9);
    t.body = rgb(0xe2e8d7);
    t.copy = rgb(0xc5d0bd);
    t.meta = rgb(0xb7c6b5);
    t.label = rgb(0xaebdaa);
    t.muted = rgb(0xaab8ac);
    t.faint = rgb(0x8c9e90);
    t.warning = rgb(0xe8b39a);
    t.rule = rgb(0x536b55);

    t.radius_row = 18.0f;
    t.radius_card = 22.0f;
    t.radius_panel = 30.0f;
    t.pill_chips = true;
    t.focus_width = 2.0f;
    t.focus_glow = 30.0f;
    t.glass_panels = true;

    t.focus_spring = 20.0f;
    t.cursor_spring = 26.0f;
    t.scroll_spring = 16.0f;

    t.backdrop = "backdrop.tga";
    t.backdrop_blur = "backdrop-blur.tga";
    t.brand = "brand.tga";
    t.light_x = 0.710f;
    t.light_y = 0.391f;
    t.motes = 26;
    t.drift = 1.0f;
    return t;
}

struct Store
{
    std::array<Theme, 2> all{canopy(), slate()};
    int index = 0;
    Theme active = all[0];
};

Store &store()
{
    static Store value;
    return value;
}

} // namespace

const Theme &th()
{
    return store().active;
}

std::span<const Theme> themes()
{
    return store().all;
}

int theme_index()
{
    return store().index;
}

bool use_theme(std::string_view id)
{
    Store &all = store();
    for (std::size_t i = 0; i < all.all.size(); ++i)
    {
        if (id == all.all[i].id)
        {
            all.index = static_cast<int>(i);
            all.active = all.all[i];
            return true;
        }
    }
    return false;
}

} // namespace pe::ui
