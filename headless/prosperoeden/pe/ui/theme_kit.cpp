// ProsperoEden - A UI kit theme (ps5-homebrew-ui) as a launcher theme.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit is fetched at a pinned commit when the launcher is built (tools/deps.json); this file
// and the kit's own theme table are the only two that see its headers.

#include "pe/ui/theme.hpp"

#include "ui/theme.hpp" // ps5-homebrew-ui: hui::ui::Theme, hui::ui::themes()

#include <algorithm>

namespace pe::ui
{

namespace
{

Color from(hui::gfx::Color color)
{
    return {color.r, color.g, color.b, color.a};
}

// A translucent colour as it looks laid over an opaque one.
Color over(Color under, Color color)
{
    return {under.r + (color.r - under.r) * color.a, under.g + (color.g - under.g) * color.a,
            under.b + (color.b - under.b) * color.a, 1.0f};
}

Color solid(Color color)
{
    return {color.r, color.g, color.b, 1.0f};
}

} // namespace

bool theme_from_kit(std::string_view kit_id, Theme *theme)
{
    for (const hui::ui::Theme &kit : hui::ui::themes())
    {
        if (kit_id != kit.id)
            continue;
        Theme t = *theme;
        t.kit = kit.id;

        // ---- colour: every role from the kit's ----
        const Color page = solid(from(kit.page));
        const Color surface = over(page, from(kit.surface));
        const Color well = over(surface, from(kit.surface_high));
        const Color text = over(surface, from(kit.text));
        const Color quiet = over(surface, from(kit.text_muted));
        const Color primary = solid(from(kit.primary));
        const Color edge = over(surface, from(kit.outline));
        t.base = gfx::mix(page, Color{0.0f, 0.0f, 0.0f, 1.0f}, kit.dark ? 0.35f : 0.0f);
        t.scrim = gfx::mix(page, Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.75f);
        t.glass = surface;
        t.panel = gfx::mix(page, surface, 0.5f);
        t.panel_edge = edge;
        t.row = well;
        t.row_edge = edge;
        t.row_focus = gfx::mix(well, primary, 0.22f);
        t.accent = primary;
        t.accent_deep = gfx::mix(primary, page, 0.6f);
        t.accent_pale = gfx::mix(primary, text, 0.6f);
        t.on_accent = solid(from(kit.on_primary));
        t.highlight = solid(from(kit.accent));
        t.text = text;
        t.title = text;
        t.value = text;
        t.body = gfx::mix(text, quiet, 0.25f);
        t.copy = gfx::mix(text, quiet, 0.55f);
        t.meta = gfx::mix(text, quiet, 0.80f);
        t.label = quiet;
        t.muted = quiet;
        t.faint = gfx::mix(quiet, surface, 0.25f);
        t.warning = solid(from(kit.warning));
        t.rule = gfx::mix(edge, surface, 0.35f);

        // ---- shape: the kit's corners, at this launcher's larger sizes ----
        const bool pill = kit.radius >= 100.0f;
        t.radius_row = pill ? 100.0f : kit.radius * 2.25f;
        t.radius_card = kit.radius_card * 1.6f;
        t.radius_panel = kit.radius_card * 2.15f;
        t.pill_chips = kit.pill_chips || pill;
        t.border = std::max(1.0f, kit.border);
        t.focus_width = std::max(2.0f, kit.focus_width - 1.0f);
        t.focus_glow = kit.shadow_blur + 2.0f;
        t.glass_panels = kit.style == hui::ui::SurfaceStyle::glass;
        t.shadow_blur = kit.shadow_blur + 20.0f;
        t.shadow_offset = kit.shadow_offset + 10.0f;

        // ---- motion: the kit's springs ----
        t.focus_spring = kit.omega * 1.25f;
        t.cursor_spring = kit.omega * 1.625f;
        t.scroll_spring = kit.omega;

        *theme = t;
        return true;
    }
    return false;
}

} // namespace pe::ui
