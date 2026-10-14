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

// ---- accent presets: named colour sets a user can pick per theme ----

constexpr AccentPreset kSlateAccents[] = {
    {"silver",  "Silver",  Color::rgb(0xc0bbb6), Color::rgb(0x3a3836), Color::rgb(0xdedbd8), Color::rgb(0x101012), Color::rgb(0xe8e4e0)},
    {"ice",     "Ice",     Color::rgb(0x7ec8e3), Color::rgb(0x1c4966), Color::rgb(0xb8e0f0), Color::rgb(0x0a1418), Color::rgb(0xd0ecf8)},
    {"rose",    "Rose",    Color::rgb(0xe88ca0), Color::rgb(0x5c2232), Color::rgb(0xf2c0cc), Color::rgb(0x120a0c), Color::rgb(0xf5d4dc)},
    {"amber",   "Amber",   Color::rgb(0xe0a848), Color::rgb(0x5a4018), Color::rgb(0xf0d090), Color::rgb(0x121008), Color::rgb(0xf5dca0)},
    {"mint",    "Mint",    Color::rgb(0x6ecaa0), Color::rgb(0x1e4a38), Color::rgb(0xb0e8cc), Color::rgb(0x0a1410), Color::rgb(0xc8f0d8)},
    {"lilac",   "Lilac",   Color::rgb(0xb098d8), Color::rgb(0x3c2868), Color::rgb(0xd4c4ec), Color::rgb(0x0e0a14), Color::rgb(0xe0d4f0)},
    {"coral",   "Coral",   Color::rgb(0xe88070), Color::rgb(0x5c2820), Color::rgb(0xf0b8ac), Color::rgb(0x140c0a), Color::rgb(0xf5c8c0)},
    {"sky",     "Sky",     Color::rgb(0x60a8e0), Color::rgb(0x1a3a5c), Color::rgb(0xa8d0f0), Color::rgb(0x0a1018), Color::rgb(0xc0dcf5)},
};

constexpr AccentPreset kCanopyAccents[] = {
    {"lime",     "Lime",     Color::rgb(0xa9db63), Color::rgb(0x245d4a), Color::rgb(0xdfe8a6), Color::rgb(0x0b1713), Color::rgb(0xffd76a)},
    {"gold",     "Gold",     Color::rgb(0xe0c050), Color::rgb(0x4a3a18), Color::rgb(0xf0dda0), Color::rgb(0x0b1410), Color::rgb(0xffeaa0)},
    {"ember",    "Ember",    Color::rgb(0xe08850), Color::rgb(0x4a2818), Color::rgb(0xf0c4a0), Color::rgb(0x0e100a), Color::rgb(0xffd4a0)},
    {"teal",     "Teal",     Color::rgb(0x50c8b0), Color::rgb(0x184a40), Color::rgb(0xa0e8d8), Color::rgb(0x0a1210), Color::rgb(0xb0f0e0)},
    {"sakura",   "Sakura",   Color::rgb(0xe890a8), Color::rgb(0x4a2030), Color::rgb(0xf0c8d4), Color::rgb(0x100a0c), Color::rgb(0xf8d8e0)},
    {"ocean",    "Ocean",    Color::rgb(0x58a8d8), Color::rgb(0x183a58), Color::rgb(0xa0d0f0), Color::rgb(0x0a1018), Color::rgb(0xb8e0f8)},
    {"violet",   "Violet",   Color::rgb(0xa080d0), Color::rgb(0x342060), Color::rgb(0xccb8e8), Color::rgb(0x0c0a14), Color::rgb(0xdcc8f0)},
    {"copper",   "Copper",   Color::rgb(0xd0905c), Color::rgb(0x4a2818), Color::rgb(0xe8c4a0), Color::rgb(0x100c08), Color::rgb(0xf0d0a8)},
};

struct Store
{
    std::array<Theme, 2> all{canopy(), slate()};
    int index = 0;
    int accent = -1;
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
            all.accent = -1;
            return true;
        }
    }
    return false;
}

static std::span<const AccentPreset> presets_for(int theme_index)
{
    switch (theme_index)
    {
    case 0: return kCanopyAccents;
    case 1: return kSlateAccents;
    default: return {};
    }
}

std::span<const AccentPreset> accent_presets()
{
    return presets_for(store().index);
}

int accent_index()
{
    return store().accent;
}

void use_accent(std::string_view id)
{
    Store &s = store();
    if (id.empty())
    {
        s.accent = -1;
        s.active = s.all[static_cast<std::size_t>(s.index)];
        return;
    }
    const auto presets = presets_for(s.index);
    for (std::size_t i = 0; i < presets.size(); ++i)
    {
        if (id == presets[i].id)
        {
            s.accent = static_cast<int>(i);
            s.active.accent = presets[i].accent;
            s.active.accent_deep = presets[i].accent_deep;
            s.active.accent_pale = presets[i].accent_pale;
            s.active.on_accent = presets[i].on_accent;
            s.active.highlight = presets[i].highlight;
            return;
        }
    }
}

} // namespace pe::ui
