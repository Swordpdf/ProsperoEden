// Eden PS5 port: where an ASTC texture that is kept as BC3 gets converted.
//
// The console's GPU has no ASTC, and above 1x such a texture is kept as BC3 (headless/main.cpp):
// decoded and compressed on the CPU. In place, inside the draw that first uses it, that held a
// frame of a large open-world game for 130 to 290 ms whenever new scenery came in. On another
// thread (Eden's asynchronous mode) no frame waits, but the texture is drawn before it is ready:
// in play that is a far texture a moment late, on a loading screen it is the picture itself, and
// wrong.
//
// So the frame decides. A frame with many draws is the game in play: its new textures are
// converted in the background. A frame with few is a loading screen, a menu or a title screen,
// where nothing moves that a wait would spoil: there the texture is converted in place and is
// right the first time it shows. The count is of the frame being made or the one before it,
// whichever is larger, so the first frames of play after a light screen are already "busy".
#pragma once

namespace Eden::AstcPolicy {
// GPU thread only.
inline unsigned draws_now = 0, draws_before = 0;
// From this many draws a frame is the game in play (a large open-world game: 2,000 to 4,500
// in play, 300 to 750 on its loading screen).
inline constexpr unsigned kBusyFrame = 1000;

inline void Draw() noexcept { ++draws_now; }
inline void Frame() noexcept {
    draws_before = draws_now;
    draws_now = 0;
}
inline bool InBackground() noexcept {
    return (draws_now > draws_before ? draws_now : draws_before) >= kBusyFrame;
}
} // namespace Eden::AstcPolicy
