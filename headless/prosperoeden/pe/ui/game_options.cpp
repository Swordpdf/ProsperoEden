// ProsperoEden - Launcher: a game's own settings by category (Library > Game settings > Video,
// Performance, Audio, Controls, System, Language) and the button mapping (Settings > Controls,
// or a game's own).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

// Placed as the game settings dialog is (library.cpp): five rows show, the list scrolls.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kRowsTop = 334.0f;
constexpr float kRowPitch = 96.0f;
constexpr float kRowHeight = 94.0f;
constexpr int kRowsShown = 5;
constexpr Rect kWindow{592.0f, kRowsTop, 736.0f, kRowPitch * (kRowsShown - 1) + kRowHeight};
constexpr float kHints = 848.0f;

enum Category : int
{
    category_video,
    category_performance,
    category_audio,
    category_controls,
    category_system,
    category_language,
};
constexpr const char *kCategoryNames[] = {TR("Video"), TR("Performance"), TR("Audio"), TR("Controls"),
                                          TR("System"), TR("Language")};
// The Controls category's rows.
constexpr int kVibrationRow = 0;
constexpr int kMappingRow = 1;
constexpr int kControllerRow = 2;
// The System category's row.
constexpr int kSeedRow = 0;

// The game's buttons and the DualSense buttons, in the mapping's order (services.hpp). The game's
// are its own names; the stick presses and the DualSense's named buttons are translated.
constexpr const char *kGameButtonNames[kGameButtons] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", TR("Left stick press"), TR("Right stick press")};
constexpr const char *kPadButtonNames[kPadButtons] = {
    TR("Cross"), TR("Circle"), TR("Square"), TR("Triangle"), "L1", "R1", "L2", "R2", "L3", "R3",
    TR("Options"), TR("Create"), TR("Touchpad")};

const char *on_off(bool value)
{
    return value ? tr("On") : tr("Off");
}

std::string percent(int value)
{
    return fill(tr("{0}%"), {std::to_string(value)});
}

std::string pick(const std::vector<std::string> &values, int index)
{
    return index >= 0 && index < static_cast<int>(values.size()) ? values[static_cast<std::size_t>(index)] :
                                                                   std::string{"-"};
}

// One setting of a game: its choices, the one the game has (-1: it follows Settings), and what
// Settings has.
struct Option
{
    std::string name;
    std::vector<std::string> values;
    int value = -1;
    std::string usual;
    bool locked = false; // shown greyed out: it cannot be changed now
};

bool *performance_switch(Preferences &prefs, int index)
{
    bool *const switches[] = {&prefs.block_list, &prefs.async_shaders,     &prefs.fast_gpu,
                              &prefs.unsafe_cpu, &prefs.unsafe_dma,        &prefs.reactive_flushing,
                              &prefs.skip_invalidation};
    return switches[std::clamp(index, 0, 6)];
}

// rates: the refresh rates as the launcher writes them (Launcher::hertz).
std::vector<Option> options(int category, const GameSettings &game, Preferences prefs,
                            Services &services, const std::vector<std::string> &rates)
{
    const std::vector<std::string> off_on = {tr("Off"), tr("On")};
    std::vector<Option> rows;
    switch (category)
    {
    case category_video:
    {
        const std::vector<std::string> renderers = {"OpenGL", "Vulkan"};
        const auto &resolutions = services.resolution_labels();
        // A resolution's short name is how its label starts: "0.5x (faster, softer)" is "0.5x".
        const std::string usual_resolution = pick(resolutions, prefs.resolution);
        rows.push_back({tr("Renderer"), renderers, game.renderer, pick(renderers, prefs.renderer)});
        rows.push_back({tr("Resolution"), resolutions, game.resolution,
                        usual_resolution.substr(0, usual_resolution.find(' '))});
        rows.push_back({tr("Upscaling filter"), services.filter_labels(), game.filter,
                        pick(services.filter_labels(), prefs.filter)});
        rows.push_back({tr("Refresh rate"), rates, game.refresh, pick(rates, prefs.refresh)});
        rows.push_back({tr("FPS overlay"), off_on, game.hud, on_off(prefs.hud)});
        // Appended last on purpose: the rows above keep the indexes with_option() and the
        // saved game settings use. Vulkan only -- the OpenGL renderer has no frame generation.
        // Not shown in a build without it; greyed out while the user's Lossless.dll is missing.
        const int frame_gen = services.frame_gen_state();
        if (frame_gen == 0)
            break;
        const bool locked = frame_gen == 1;
        rows.push_back({tr("Frame generation"), off_on, game.frame_gen, on_off(prefs.frame_gen), locked});
        // Indexes match the settings layer's tables (settings_store.h). Eden's two controls work the
        // way Eden defines them: a target rate makes the pacer add as many frames as that rate
        // needs, up to Eden's own maximum; "Auto" asks for none and leaves the multiplier in charge.
        const std::vector<std::string> multipliers = {"2x", "3x", "4x"};
        const auto hz = [](const char *value) { return fill(tr("{0} Hz"), {value}); };
        const std::vector<std::string> frames = {tr("Auto"), hz("60"), hz("90"), hz("120"), hz("144"),
                                                 hz("240")};
        rows.push_back({tr("Frame gen target"), frames, game.frame_gen_target,
                        pick(frames, prefs.frame_gen_target), locked});
        rows.push_back({tr("Frame gen multiplier"), multipliers, game.frame_gen_multiplier,
                        pick(multipliers, prefs.frame_gen_multiplier), locked});
        break;
    }
    case category_performance:
    {
        static constexpr const char *kNames[] = {
            TR("Compile ahead"),        TR("Asynchronous shaders"), TR("Faster GPU emulation"),
            TR("Faster CPU emulation"), TR("Faster DMA"),           TR("Reactive flushing"),
            TR("Skip CPU invalidation")};
        for (int i = 0; i < 7; ++i)
        {
            // Asynchronous shaders act with Vulkan only (settings.cpp).
            rows.push_back({std::string(tr(kNames[i])) + (i == 1 ? " (Vulkan)" : ""), off_on,
                            game.performance[static_cast<std::size_t>(i)],
                            on_off(*performance_switch(prefs, i))});
        }
        break;
    }
    case category_audio:
    {
        std::vector<std::string> levels;
        for (int level = 0; level <= 100; level += 10)
            levels.push_back(percent(level));
        rows.push_back({tr("Game volume"), levels, game.volume < 0 ? -1 : game.volume / 10,
                        percent(prefs.volume)});
        rows.push_back({tr("Mute"), off_on, game.mute, on_off(prefs.mute)});
        break;
    }
    case category_controls:
        rows.push_back({tr("Vibration"), off_on, game.vibration, on_off(prefs.vibration)});
        rows.push_back({tr("Button mapping"), {tr("This game")}, game.own_mapping ? 0 : -1,
                        prefs.mapping == kDefaultMapping ? tr("As usual") : tr("Changed")});
        // The controller the game is given: without a choice, what it takes, a Pro Controller
        // first. Some games take one and then only work with another.
        rows.push_back({tr("Controller type"),
                        {tr("Pro Controller"), tr("Handheld"), tr("Dual Joy-Cons"), tr("Left Joy-Con"),
                         tr("Right Joy-Con")},
                        game.controller, tr("Automatic")});
        break;
    case category_system:
        // The seed is a number: the row's one choice is the game's (0 until one is typed, with
        // Cross). Without a seed the game is loaded at a new random address at every start.
        rows.push_back({tr("RNG seed"), {std::to_string(std::max<std::int64_t>(game.rng_seed, 0))},
                        game.rng_seed >= 0 ? 0 : -1, tr("Random")});
        break;
    default:
        rows.push_back({tr("Language"), services.language_labels(), game.language,
                        pick(services.language_labels(), prefs.language)});
        break;
    }
    return rows;
}

// The game's settings with one choice changed (-1: back to Settings).
GameSettings with_option(GameSettings game, int category, int row, int value, const Preferences &prefs)
{
    switch (category)
    {
    case category_video:
        (row == 0 ? game.renderer : row == 1 ? game.resolution : row == 2 ? game.filter :
         row == 3 ? game.refresh : row == 4 ? game.hud : row == 5 ? game.frame_gen :
         row == 6 ? game.frame_gen_target : game.frame_gen_multiplier) = value;
        break;
    case category_performance:
        game.performance[static_cast<std::size_t>(std::clamp(row, 0, 6))] = value;
        break;
    case category_audio:
        if (row == 0)
            game.volume = value < 0 ? -1 : value * 10;
        else
            game.mute = value;
        break;
    case category_controls:
        if (row == kVibrationRow)
            game.vibration = value;
        else if (row == kControllerRow)
            game.controller = value;
        else
        {
            // A mapping of its own starts as Settings' one.
            if (!game.own_mapping && value >= 0)
                game.mapping = prefs.mapping;
            game.own_mapping = value >= 0;
        }
        break;
    case category_system:
        game.rng_seed = value < 0 ? -1 : std::max<std::int64_t>(game.rng_seed, 0);
        break;
    default:
        game.language = value;
        break;
    }
    return game;
}

} // namespace

int Launcher::game_overrides(int category) const
{
    const GameSettings &game = game_settings_;
    switch (category)
    {
    case category_video:
        return (game.renderer >= 0) + (game.resolution >= 0) + (game.filter >= 0) + (game.refresh >= 0) +
               (game.hud >= 0) + (game.frame_gen >= 0) + (game.frame_gen_target >= 0) +
               (game.frame_gen_multiplier >= 0);
    case category_performance:
        return static_cast<int>(
            std::count_if(game.performance.begin(), game.performance.end(), [](int v) { return v >= 0; }));
    case category_audio:
        return (game.volume >= 0) + (game.mute >= 0);
    case category_controls:
        return (game.vibration >= 0) + game.own_mapping + (game.controller >= 0);
    case category_system:
        return game.rng_seed >= 0;
    default:
        return game.language >= 0;
    }
}

void Launcher::open_game_options(int category)
{
    game_options_ = std::clamp(category, 0, kGameOptionCategories - 1);
    option_rows_.visible = kRowsShown;
    option_rows_.pitch = kRowPitch;
    option_rows_.reset(
        static_cast<int>(options(game_options_, game_settings_, prefs_, services_, {hertz(0), hertz(1)}).size()), 0);
    option_ = 0;
    modal_ = modal_shown_ = Modal::game_options;
    message_.clear();
    cue(Cue::open);
}

void Launcher::edit_seed()
{
    seed_edit_stop_ = false;
    try
    {
        seed_edit_ = std::async(std::launch::async, [this, current = game_settings_.rng_seed] {
            return services_.ask_seed(current, seed_edit_stop_);
        });
    }
    catch (const std::exception &)
    {
        say(tr("The keyboard could not be opened."), true);
        cue(Cue::error);
    }
}

void Launcher::update_game_options()
{
    if (!seed_edit_.valid() || seed_edit_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        return;
    std::optional<std::int64_t> answer;
    try
    {
        answer = seed_edit_.get();
    }
    catch (const std::exception &)
    {
        say(tr("The keyboard could not be opened."), true);
        cue(Cue::error);
        return;
    }
    // Cancelled, or the dialog it was for is gone.
    if (!answer || seed_edit_stop_ || modal_ != Modal::game_options || game_options_ != category_system ||
        games_.empty())
        return;
    if (*answer < 0)
    {
        say(tr("Enter a number between 0 and 4294967295."), true);
        cue(Cue::error);
        return;
    }
    GameSettings next = game_settings_;
    next.rng_seed = *answer;
    const bool saved =
        services_.set_game_settings(games_[static_cast<std::size_t>(library_.selected)].title_id, next);
    if (saved)
        game_settings_ = next;
    say(saved ? tr("Saved for this game. Applies on next launch.") : tr("Could not save. Please try again."),
        !saved);
    cue(saved ? Cue::toggle : Cue::error);
}

void Launcher::press_game_options(Key key)
{
    // The keyboard is open: Circle takes it away, nothing else is for this dialog.
    if (seed_edit_.valid())
    {
        if (key == Key::circle)
            seed_edit_stop_ = true;
        return;
    }
    const Game &game = games_[static_cast<std::size_t>(library_.selected)];
    const std::vector<Option> rows = options(game_options_, game_settings_, prefs_, services_, {hertz(0), hertz(1)});
    switch (key)
    {
    case Key::circle:
        // Back to the game's settings, on this category's row.
        modal_ = modal_shown_ = Modal::game;
        option_ = game_rows_.selected;
        message_.clear();
        cue(Cue::back);
        return;
    case Key::up:
    case Key::down:
        if (option_rows_.move(key == Key::down ? 1 : -1))
        {
            option_ = option_rows_.selected;
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::left:
    case Key::right:
    case Key::cross:
        break;
    default:
        return;
    }
    const int row = std::clamp(option_rows_.selected, 0, static_cast<int>(rows.size()) - 1);
    const Option &option = rows[static_cast<std::size_t>(row)];
    // The game's own mapping is edited in its own list.
    if (game_options_ == category_controls && row == kMappingRow && key == Key::cross && option.value >= 0)
    {
        open_mapping(true);
        return;
    }
    // The seed is typed.
    if (game_options_ == category_system && row == kSeedRow && key == Key::cross)
    {
        edit_seed();
        return;
    }
    if (option.locked)
    {
        say(tr("Frame generation needs Lossless.dll, from Lossless Scaling, in the lossless folder."), true);
        cue(Cue::error);
        return;
    }
    // Settings' value (-1), then each of the game's choices.
    const int count = static_cast<int>(option.values.size());
    const int step = key == Key::left ? -1 : 1;
    const int value = (option.value + 1 + step + count + 1) % (count + 1) - 1;
    const GameSettings next = with_option(game_settings_, game_options_, row, value, prefs_);
    const bool saved = services_.set_game_settings(game.title_id, next);
    if (saved)
        game_settings_ = next;
    // 120 Hz is a request: the display has the last word.
    const bool fast = saved && game_options_ == category_video && row == 3 &&
                      (game_settings_.refresh >= 0 ? game_settings_.refresh : prefs_.refresh) == 1;
    say(fast ? tr("Saved. A display that cannot show 120 Hz stays at 60 Hz.") :
        saved ? tr("Saved for this game. Applies on next launch.") : tr("Could not save. Please try again."),
        !saved);
    cue(saved ? Cue::toggle : Cue::error);
}

void Launcher::draw_game_options(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, 26.0f, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr(kCategoryNames[game_options_]), 592.0f, baseline(218.0f, 62.0f, theme::kDisplay),
                theme::kDisplay, theme::kTitle, 736.0f);
    const Game *game = games_.empty() ? nullptr : &games_[static_cast<std::size_t>(library_.selected)];
    text_fit(c, game != nullptr ? game->name : std::string{}, 592.0f, baseline(291.0f, 32.0f, theme::kSmall),
             theme::kSmall, Color::rgb(0xbecbb9), 736.0f);

    const std::vector<Option> rows = options(game_options_, game_settings_, prefs_, services_, {hertz(0), hertz(1)});
    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - option_rows_.scroll(); };
    for (int row = option_rows_.first_row(); row <= option_rows_.last_row(); ++row)
    {
        list.push_opacity(option_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, kRowPlate, {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, kRowPlate, {592.0f, kRowsTop + option_rows_.cursor() - option_rows_.scroll(), 736.0f, kRowHeight},
                1.0f);
    for (int row = option_rows_.first_row(); row <= option_rows_.last_row(); ++row)
    {
        if (row >= static_cast<int>(rows.size()))
            break;
        const Option &option = rows[static_cast<std::size_t>(row)];
        const float top = row_top(row);
        list.push_opacity(option_rows_.row_alpha(row, kRowHeight));
        // The game's own choice, or "Default (what Settings has)".
        const std::string value = option.locked ? std::string{tr("Unavailable")} :
            option.value >= 0 ? pick(option.values, option.value) : fill(tr("Default ({0})"), {option.usual});
        if (option.locked)
            list.push_opacity(0.38f);
        const float taken = chooser(c, value, 1296.0f, baseline(top, kRowHeight, theme::kText24),
                                    row == option_rows_.selected ? 1.0f : 0.0f,
                                    option.value >= 0 ? theme::kLime : theme::kLimePale);
        text_shrink(c, option.name, 628.0f, baseline(top, kRowHeight, theme::kText24), theme::kText24,
                    theme::kValue, 664.0f - taken - 28.0f);
        if (option.locked)
            list.pop_opacity();
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, option_rows_, 1340.0f, kWindow.y, kWindow.h);

    const bool opens = game_options_ == category_controls && option_rows_.selected == kMappingRow &&
                       game_settings_.own_mapping;
    const bool types = game_options_ == category_system && option_rows_.selected == kSeedRow;
    if (!message_.empty())
    {
        notice_block(c, message_, 592.0f, kHints - 6.0f, theme::kSmall, 26.0f,
                     message_warning_ ? theme::kWarning : theme::kLimePale, 736.0f, 2, message_warning_);
    }
    else if (opens)
    {
        static constexpr Hint kOpen[] = {
            {Pad::cross, TR("Open")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kOpen, 3, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    else if (types)
    {
        static constexpr Hint kType[] = {
            {Pad::cross, TR("Edit")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kType, 3, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    else
    {
        static constexpr Hint kChange[] = {
            {Pad::updown, TR("Select")}, {Pad::leftright, TR("Change")}, {Pad::circle, TR("Back")}};
        draw_hints(c, kChange, 3, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

// ---------------------------------------------------------------- the button mapping

void Launcher::open_mapping(bool for_game)
{
    mapping_for_game_ = for_game;
    mapping_rows_.visible = kRowsShown;
    mapping_rows_.pitch = kRowPitch;
    mapping_rows_.reset(kGameButtons, 0);
    modal_ = modal_shown_ = Modal::mapping;
    message_.clear();
    cue(Cue::open);
}

void Launcher::press_mapping(Key key)
{
    ButtonMapping mapping = mapping_for_game_ ? game_settings_.mapping : prefs_.mapping;
    const int row = std::clamp(mapping_rows_.selected, 0, kGameButtons - 1);
    switch (key)
    {
    case Key::circle:
        // Back to where the mapping was opened: the game's Controls, or Settings > Controls.
        if (mapping_for_game_)
        {
            modal_ = modal_shown_ = Modal::game_options;
            option_ = option_rows_.selected;
        }
        else
        {
            modal_ = modal_shown_ = Modal::controls;
            option_ = 1;
            option_cursor_.snap(dialog_row_top(Modal::controls, option_));
        }
        message_.clear();
        cue(Cue::back);
        return;
    case Key::up:
    case Key::down:
        if (mapping_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::square:
        // Every game button back on its usual DualSense button.
        mapping = kDefaultMapping;
        break;
    case Key::left:
    case Key::right:
    case Key::cross:
    {
        // The next DualSense button; the game button that had it takes this one's.
        const int step = key == Key::left ? -1 : 1;
        const int pad = (mapping[static_cast<std::size_t>(row)] + step + kPadButtons) % kPadButtons;
        mapping = assign_button(mapping, row, pad);
        break;
    }
    default:
        return;
    }
    bool saved = false;
    if (mapping_for_game_)
    {
        GameSettings next = game_settings_;
        next.own_mapping = true;
        next.mapping = mapping;
        const Game &game = games_[static_cast<std::size_t>(library_.selected)];
        saved = services_.set_game_settings(game.title_id, next);
        if (saved)
            game_settings_ = next;
        say(saved ? tr("Saved for this game. Applies on next launch.") : tr("Could not save. Please try again."),
            !saved);
    }
    else
    {
        const Preferences before = prefs_;
        prefs_.mapping = mapping;
        saved = save_preferences();
        if (!saved)
            prefs_ = before;
    }
    cue(saved ? Cue::toggle : Cue::error);
}

void Launcher::draw_mapping(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, 26.0f, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Button mapping"), 592.0f, baseline(218.0f, 62.0f, theme::kDisplay), theme::kDisplay,
                theme::kTitle, 736.0f);
    const Game *game = mapping_for_game_ && !games_.empty() ?
                           &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
    text_fit(c, game != nullptr ? game->name : std::string{tr("For every controller. A game can have its own.")},
             592.0f, baseline(291.0f, 32.0f, theme::kSmall), theme::kSmall, Color::rgb(0xbecbb9), 736.0f);

    const ButtonMapping &mapping = mapping_for_game_ ? game_settings_.mapping : prefs_.mapping;
    list.push_clip({kWindow.x - 24.0f, kWindow.y - 6.0f, kWindow.w + 48.0f, kWindow.h + 12.0f});
    const auto row_top = [&](int row)
    { return kRowsTop + static_cast<float>(row) * kRowPitch - mapping_rows_.scroll(); };
    for (int row = mapping_rows_.first_row(); row <= mapping_rows_.last_row(); ++row)
    {
        list.push_opacity(mapping_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, kRowPlate, {592.0f, row_top(row), 736.0f, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, kRowPlate, {592.0f, kRowsTop + mapping_rows_.cursor() - mapping_rows_.scroll(), 736.0f, kRowHeight},
                1.0f);
    for (int row = mapping_rows_.first_row(); row <= mapping_rows_.last_row(); ++row)
    {
        const float top = row_top(row);
        const int pad = mapping[static_cast<std::size_t>(row)];
        const bool usual = pad == kDefaultMapping[static_cast<std::size_t>(row)];
        list.push_opacity(mapping_rows_.row_alpha(row, kRowHeight));
        // The game's button, and the DualSense button that presses it (brighter when moved).
        const float taken = chooser(c, tr(kPadButtonNames[std::clamp(pad, 0, kPadButtons - 1)]), 1296.0f,
                                    baseline(top, kRowHeight, theme::kText24),
                                    row == mapping_rows_.selected ? 1.0f : 0.0f,
                                    usual ? theme::kLimePale : theme::kLime);
        text_shrink(c, tr(kGameButtonNames[row]), 628.0f, baseline(top, kRowHeight, theme::kText24),
                    theme::kText24, theme::kValue, 664.0f - taken - 28.0f);
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, mapping_rows_, 1340.0f, kWindow.y, kWindow.h);

    if (!message_.empty())
    {
        notice_block(c, message_, 592.0f, kHints - 6.0f, theme::kSmall, 26.0f,
                     message_warning_ ? theme::kWarning : theme::kLimePale, 736.0f, 2, message_warning_);
    }
    else
    {
        static constexpr Hint kHintsRow[] = {{Pad::updown, TR("Select")},
                                             {Pad::leftright, TR("Change")},
                                             {Pad::square, TR("Reset")},
                                             {Pad::circle, TR("Back")}};
        draw_hints(c, kHintsRow, 4, 592.0f, kHints, theme::kCopy, 736.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
