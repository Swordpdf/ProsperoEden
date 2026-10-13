// SPDX-License-Identifier: GPL-3.0-or-later
// A recorded controller session. With gameplay-record.txt in the app's folder the pad writes
// what it is handed to gameplay-test.txt; with gameplay-replay.txt there, the pad is driven from
// that file instead of a controller (replay wins if both are there). The files choose the way
// cost-run.txt and performance-run.txt do (main.cpp:546).
//
// One line per state the pad takes, stamped with the frame the emulator was on when it was
// handed it (Display::gameplay_frame). A replay puts each line back on that frame, so two runs
// of the same recording drive the same frames whatever the wall clock did between them. Both
// start from boot, so a replay also starts from the same place in the game: there is no save
// state here, and a recording taken from a fresh save replays from a fresh save.
//
// Only the buttons, the sticks, the triggers and the connection are recorded. Motion is left out
// deliberately - an accelerometer never stops changing, so it would defeat the dedupe that keeps
// a session down to the states that actually changed, and the game this was written for does not read it. A replayed
// pad therefore reports itself at rest rather than the room it was recorded in. The touchpad is
// worth leaving alone while recording too: its tap and its hold are counted in calls into
// Pad::Consume rather than in time (pad.cpp, kSelectTapPolls), and a replay reaches Consume once
// per recorded state rather than once per poll. Every other button, the sticks and the triggers
// are pure state and replay exactly.
//
// Pad::Consume and Pad::Poll are both the input worker (main.cpp), so nothing here locks.
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include "display_refresh.h"
#include "ps5_pad.hpp"
#include "storage_paths.h"

namespace Eden::GameplayTest {
inline constexpr std::string_view kRecordFile = "gameplay-record.txt";
inline constexpr std::string_view kReplayFile = "gameplay-replay.txt";
inline constexpr std::string_view kSessionFile = "gameplay-test.txt";

enum class Mode : std::uint8_t { off, record, replay };

// Chosen once, from the files present when the pad first asks. The banner is that first ask.
inline Mode ModeOf() {
    static const Mode mode = [] {
        const Mode found = FileExists(AppFile(kReplayFile))   ? Mode::replay
                         : FileExists(AppFile(kRecordFile))   ? Mode::record
                                                              : Mode::off;
        if (found != Mode::off)
            std::printf("EDEN_GAMEPLAY_TEST mode=%s file=%s\n",
                        found == Mode::replay ? "replay" : "record",
                        AppFile(kSessionFile).c_str());
        return found;
    }();
    return mode;
}
inline bool Recording() { return ModeOf() == Mode::record; }
inline bool Replaying() { return ModeOf() == Mode::replay; }

// Everything Pad::Consume reads off a sample, minus the motion (see the header).
struct State {
    std::uint32_t buttons{};
    std::uint8_t left_x{}, left_y{}, right_x{}, right_y{}, l2{}, r2{};
    bool connected{};
    bool operator==(const State&) const = default;
};

inline State Of(const ps5::pad::Data& sample) {
    return {sample.buttons, sample.left_stick.x, sample.left_stick.y, sample.right_stick.x,
            sample.right_stick.y, sample.triggers.l2, sample.triggers.r2, sample.connected != 0};
}

// What the session has done, for the line Pad::Close prints.
inline unsigned long long written = 0;
inline unsigned long long delivered = 0;
inline unsigned delivered_frame = 0;

// The session file, at namespace scope rather than inside Record so Report can reach it. A
// console storage write costs about 25 ms (main.cpp), so a state per write is out of the
// question and the stream buffers instead - which means something has to let go of it, or the
// last states of a session are still in the buffer when the pad closes.
inline std::ofstream session;

// A state the pad took, if it is not the one already written. Called for player 1 from
// Pad::Consume; the line written here is what Take hands back.
inline void Record(const ps5::pad::Data& sample) {
    if (ModeOf() != Mode::record) return;
    static bool opened = false, have_last = false;
    static State last{};
    const State state = Of(sample);
    if (have_last && state == last) return;
    if (!opened) {
        opened = true;
        session.open(AppFile(kSessionFile), std::ios::trunc);
        if (session) session << "# EDEN_GAMEPLAY_TEST v1 <frame> <buttons> <lx> <ly> <rx> <ry> <l2> <r2> <connected>\n";
    }
    if (!session) return;
    session << Display::gameplay_frame.load(std::memory_order_relaxed) << ' ' << state.buttons << ' '
        << unsigned{state.left_x} << ' ' << unsigned{state.left_y} << ' '
        << unsigned{state.right_x} << ' ' << unsigned{state.right_y} << ' '
        << unsigned{state.l2} << ' ' << unsigned{state.r2} << ' '
        << (state.connected ? 1 : 0) << '\n';
    last = state;
    have_last = true;
    ++written;
}

namespace detail {
// The recording a replay reads, one record ahead: a frame with no state of its own keeps the one
// it inherited rather than being served twice.
struct Replay {
    std::ifstream in;
    bool opened = false, have = false, ended = false;
    unsigned frame = 0;
    State state{};

    bool Pull() {
        if (!opened) {
            opened = true;
            in.open(AppFile(kSessionFile));
            if (!in) {
                ended = true;
                std::printf("EDEN_GAMEPLAY_TEST missing file=%s\n", AppFile(kSessionFile).c_str());
            }
        }
        if (ended) return false;
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line.front() == '#') continue;
            std::istringstream fields(line);
            unsigned buttons = 0, left_x = 0, left_y = 0, right_x = 0, right_y = 0, l2 = 0, r2 = 0;
            unsigned connected = 0;
            if (!(fields >> frame >> buttons >> left_x >> left_y >> right_x >> right_y >> l2 >>
                  r2 >> connected))
                continue;
            state = {buttons, static_cast<std::uint8_t>(left_x), static_cast<std::uint8_t>(left_y),
                     static_cast<std::uint8_t>(right_x), static_cast<std::uint8_t>(right_y),
                     static_cast<std::uint8_t>(l2), static_cast<std::uint8_t>(r2),
                     connected != 0};
            have = true;
            return true;
        }
        ended = true;
        std::printf("EDEN_GAMEPLAY_TEST replay_done frame=%u delivered=%llu\n", delivered_frame,
                    delivered);
        return false;
    }
};
}  // namespace detail

// Hand back the next recorded state whose frame has come round; false when there is none yet,
// or when the recording has run out. Called from Pad::Poll in place of reading the controller.
inline bool Take(ps5::pad::Data& sample) {
    static detail::Replay replay;
    if (ModeOf() != Mode::replay) return false;
    const unsigned now = Display::gameplay_frame.load(std::memory_order_relaxed);
    if (!replay.have && !replay.Pull()) return false;
    if (replay.frame > now) return false;
    sample = ps5::pad::Data{};
    sample.orientation.w = 1.0f;
    sample.buttons = replay.state.buttons;
    sample.left_stick = {replay.state.left_x, replay.state.left_y};
    sample.right_stick = {replay.state.right_x, replay.state.right_y};
    sample.triggers.l2 = replay.state.l2;
    sample.triggers.r2 = replay.state.r2;
    sample.connected = replay.state.connected ? 1 : 0;
    // timestamp_us stays 0: motion is not recorded, and a stand-in timestamp would only make
    // Pad::Consume compute a delta for vectors that are all zero (pad.cpp:375).
    replay.have = false;
    delivered_frame = replay.frame;
    ++delivered;
    return true;
}

// One line about the session, printed from Pad::Close.
inline void Report() {
    static bool printed = false;
    if (printed || ModeOf() == Mode::off) return;
    printed = true;
    if (session.is_open()) session.flush();
    std::printf("EDEN_GAMEPLAY_TEST session written=%llu delivered=%llu last_frame=%u\n", written,
                delivered, delivered_frame);
}
}  // namespace Eden::GameplayTest
