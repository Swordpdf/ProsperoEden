// SPDX-License-Identifier: GPL-3.0-or-later
// Button mapping (Settings > Controls, or a game's own in its settings): which DualSense button
// presses each of the game's buttons. One mapping applies to every controller. It names one
// DualSense button per game button and never the same one twice: choosing a button another game
// button has swaps the two (Assign). The D-pad, the sticks and the shortcuts (touchpad + L1,
// touchpad + R1) are not part of it.
#pragma once
#include <array>

namespace Eden {
enum GameButton : int {
    game_a, game_b, game_x, game_y, game_l, game_r, game_zl, game_zr, game_plus, game_minus,
    game_left_stick, game_right_stick, kGameButtons
};
enum PadButton : int {
    pad_cross, pad_circle, pad_square, pad_triangle, pad_l1, pad_r1, pad_l2, pad_r2, pad_l3, pad_r3,
    pad_options, pad_create, pad_touchpad, kPadButtons
};
// Their names in the settings file.
inline constexpr const char* kGameButtonKeys[kGameButtons] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "left_stick", "right_stick"};
inline constexpr const char* kPadButtonKeys[kPadButtons] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "create",
    "touchpad"};

using ButtonMapping = std::array<int, kGameButtons>;  // game button -> DualSense button
// The game's A, B, X and Y where the DualSense has the same places; the touchpad is Minus (the
// Create button presses it too while it has no game button of its own).
inline constexpr ButtonMapping kDefaultMapping = {
    pad_circle, pad_cross, pad_triangle, pad_square, pad_l1, pad_r1, pad_l2, pad_r2, pad_options,
    pad_touchpad, pad_l3, pad_r3};

inline bool ValidMapping(const ButtonMapping& mapping) {
    std::array<bool, kPadButtons> used{};
    for (const int pad : mapping) {
        if (pad < 0 || pad >= kPadButtons || used[pad]) return false;
        used[pad] = true;
    }
    return true;
}

// The game button a DualSense button presses, or -1.
inline int MappedTo(const ButtonMapping& mapping, int pad) {
    for (int game = 0; game < kGameButtons; ++game)
        if (mapping[game] == pad) return game;
    return -1;
}

// The game button takes the DualSense button; the game button that had it gets this one's old one.
inline ButtonMapping Assign(ButtonMapping mapping, int game, int pad) {
    if (game < 0 || game >= kGameButtons || pad < 0 || pad >= kPadButtons) return mapping;
    const int other = MappedTo(mapping, pad);
    if (other >= 0) mapping[other] = mapping[game];
    mapping[game] = pad;
    return mapping;
}
} // namespace Eden
