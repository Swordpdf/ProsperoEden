// ProsperoEden - Launcher themes: colour roles, type, shape, motion, artwork, ambient light, sound.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/gfx/draw_list.hpp"

#include <algorithm>
#include <span>
#include <string_view>

namespace pe::ui
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

// A theme is data: what the launcher's screens are painted with. The screens name roles (accent,
// panel, title), never a hue, and a theme fills every role. It does not move anything: where the
// rail, the hero and the lists are is the launcher's. docs/THEMES.md is the contract.
//
// A theme starts from a recipe of the UI kit (ps5-homebrew-ui, `kit`): its surface style, shadows,
// focus ring and motion, and colours for every role derived from the kit's. It then sets what it
// wants of its own over that.
struct Theme
{
    // ---- identity ----
    const char *id = "";      // in the settings file, and the folder of its files
    const char *name = "";    // as Settings > Theme shows it (a name: not translated)
    const char *summary = ""; // one line about it (English; marked with TR() where it is set)
    const char *kit = "";     // the kit theme its recipe starts from
    int contract = 1;         // the version of this structure it was made for

    // ---- colour: surfaces ----
    Color base;       // behind everything, where the artwork does not reach
    Color scrim;      // what dims the artwork behind screens and dialogs
    Color glass;      // panels over the artwork
    Color panel;      // screens' panels and dialogs
    Color panel_edge; // their hairline
    Color row;        // a control at rest
    Color row_edge;
    Color row_focus;  // under the highlight
    // ---- colour: the accent (one meaning a screen: where you are) ----
    Color accent;
    Color accent_deep; // the far end of its gradients
    Color accent_pale; // values, kickers, the focus ring
    Color on_accent;   // text on an accent fill
    Color highlight;   // light in the artwork: the sun, the motes
    // ---- colour: text, from the brightest to the faintest ----
    Color text;
    Color title;
    Color value;
    Color body;
    Color copy;
    Color meta;
    Color label;
    Color muted;
    Color faint;
    Color warning; // what needs the player (always with a mark, never the colour alone)
    Color rule;    // dividing lines

    // ---- type: one family, sizes in virtual pixels (the screen is 1920 by 1080) ----
    float size_hero = 88.0f;    // the home screen's game
    float size_display = 48.0f; // a screen's title
    float size_lead = 40.0f;
    float size_brand = 36.0f;
    float size_heading = 32.0f;
    float size_clock = 28.0f;
    float size_text = 24.0f;  // rows, actions
    float size_small = 20.0f; // values, hints, captions
    float weight_title = 0.9f; // how much heavier titles are drawn (pixels of outline at 48)
    float tracking_kicker = 4.0f; // between the letters of the small capital headings

    // ---- shape ----
    float radius_row = 18.0f;   // rows, buttons
    float radius_card = 22.0f;  // cards, tiles, covers
    float radius_panel = 30.0f; // panels, dialogs
    bool pill_chips = true;     // chips and state tags are pills
    float border = 1.0f;        // hairlines
    float focus_width = 2.0f;   // the focus ring's stroke
    float focus_glow = 30.0f;   // how far its light reaches
    bool glass_panels = true;   // panels show the blurred artwork behind them
    float shadow_blur = 48.0f;
    float shadow_offset = 20.0f;

    // ---- motion (springs in rad/s, times in seconds) ----
    float focus_spring = 20.0f;
    float cursor_spring = 26.0f;
    float scroll_spring = 16.0f;
    float screen_seconds = 0.30f;
    float launch_seconds = 0.95f;
    float stagger = 0.07f; // between the parts of a screen as it arrives
    float breathe = 2.6f;  // the focus glow's slow pulse (rad/s)

    // ---- artwork (files in the theme's art folder) ----
    const char *backdrop = "backdrop.tga";
    const char *backdrop_blur = "backdrop-blur.tga";
    const char *brand = "brand.tga";
    float light_x = 0.710f; // where the artwork's light is, as a share of its width and height
    float light_y = 0.391f;

    // ---- ambient light over the artwork ----
    int motes = 26;     // 0: none
    float drift = 1.0f; // how far the artwork wanders (0: it stands still)

    // ---- layout variants ----
    bool list_settings = false;  // settings shows a list instead of tiles
    bool game_backdrop = false;  // the selected game's cover art fills the background
    bool minimal_home = false;   // home screen: clean layout, no controller display
    bool top_nav = false;        // horizontal section tabs at the top instead of the sidebar rail
    float scrim_opacity = 0.0f;  // >0: flat scrim at this alpha; 0: directional gradient scrim
    float game_page_offset = 0.0f; // px to push the game settings columns below their base position

    // ---- sound: a folder of ui/sounds with its own cues ("": the launcher's) ----
    const char *sounds = "";
};

// The theme in use. Read where things are drawn, so a change of theme shows in the next frame.
const Theme &th();
// Every theme this build has, the first being the one a new install uses.
std::span<const Theme> themes();
// The place of the one in use in themes().
int theme_index();
// Puts a theme in use by its id; false when this build has none of that name (the one in use stays).
bool use_theme(std::string_view id);

// A kit theme (ps5-homebrew-ui) as a launcher theme: every role filled from the kit's colours, its
// corners, focus ring and motion. False when the kit has no theme of that id (theme_kit.cpp).
bool theme_from_kit(std::string_view kit_id, Theme *theme);

// How the launcher is shown: Settings > Accessibility. It lies over whatever theme is in use.
struct Look
{
    bool large_text = false;    // small text is drawn larger
    bool high_contrast = false; // solid panels, brighter text, an outlined highlight
    bool reduce_motion = false; // nothing drifts, slides or zooms: screens fade
};
inline Look &look()
{
    static Look value;
    return value;
}
// 1 normally, 0 with reduced motion: every slide, zoom and drift is multiplied by it.
inline float motion()
{
    return look().reduce_motion ? 0.0f : 1.0f;
}
// The size text of `size` is drawn at. "Larger text" raises everything under 30 to between 26
// and 30 (26 is the least the console accessibility guidelines ask of text on a 1080p TV).
inline float text_size(float size)
{
    return look().large_text && size < 30.0f ? std::clamp(size * 1.3f, 26.0f, 30.0f) : size;
}

// The baseline that centres text of `size` in a line box of height `line` starting at `top`.
inline float baseline(float top, float line, float size)
{
    return top + line * 0.5f + size * 0.35f;
}

inline bool inside(const Rect &r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// ---- where the launcher's parts are (the same in every theme) ----
namespace layout
{
constexpr float kRail = 120.0f;    // the section rail's width
constexpr float kLeft = 184.0f;    // content from here ...
constexpr float kRight = 1824.0f;  // ... to here
constexpr float kWidth = kRight - kLeft;
constexpr float kTop = 44.0f;      // the top bar's line box: 64 high
constexpr float kFooter = 1026.0f; // the hints' centre line
// A list of settings beside others (a game's page): its rows, and how many show.
constexpr float kPaneRow = 84.0f;
constexpr float kPanePitch = 94.0f;
constexpr int kPaneRows = 7;
} // namespace layout

} // namespace pe::ui
