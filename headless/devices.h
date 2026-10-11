// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#include "audio_core/sink/sink.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/input_engine.h"
#include "button_mapping.h"
#include "ps5_pad.hpp"

namespace Eden {
// The PS5 controllers as the "virtual_gamepad" input engine that Eden binds every guest
// controller to (buttons, sticks, motion), plus DualSense rumble: Eden's controllers send each
// player's vibration here (hid_core output params, headless/CMakeLists.txt), one device per side.
// Whether Eden's handheld controller is the one in use this session: chosen for the game
// (Game settings > Controls > Controller type) or given to a game that takes nothing else
// (controller_applet.h). Only then does player 1's DualSense play it as well (pad.cpp). Eden
// connects its handheld controller by itself as soon as a button reaches it, so fed all the time
// it appeared beside the player's own controller at the first press, in every game.
inline std::atomic<bool> handheld_in_use{false};

class PadEngine final : public InputCommon::InputEngine {
public:
    using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
    static constexpr std::size_t kPlayers = 10;
    explicit PadEngine(std::string name);
    void SetButtonState(std::size_t player, int button, bool value);
    void SetButtonState(std::size_t player, VirtualButton button, bool value);
    void SetStickPosition(std::size_t player, int axis, float x, float y);
    // Gyro in turns per second, acceleration in G (Eden's convention), delta in microseconds.
    void SetMotionState(std::size_t player, u64 delta_us, float gyro_x, float gyro_y, float gyro_z,
                        float accel_x, float accel_y, float accel_z);
    void SetMotionAtRest(std::size_t player);
    void ResetControllers();
    Common::Input::DriverResult SetVibration(const PadIdentifier& identifier,
                                             const Common::Input::VibrationStatus& vibration) override;
    bool IsVibrationEnabled(const PadIdentifier&) override { return true; }
    // The DualSense motor levels for a player when they changed since the last call: large (low
    // frequency) and small (high frequency), 0-255.
    struct Rumble {
        u8 large = 0;
        u8 small = 0;
    };
    bool TakeRumble(std::size_t player, Rumble& rumble);
private:
    PadIdentifier Identifier(std::size_t player) const;
    struct Sides {
        Common::Input::VibrationStatus left{};
        Common::Input::VibrationStatus right{};
        bool changed = false;
    };
    std::mutex rumble_mutex;
    std::array<Sides, kPlayers> rumble{};
};

// DualSense controllers as guest Pro Controllers. Player 1 is the controller of the user who
// launched the game; controllers of other signed-in users become players 2-4 as they appear.
class Pad final {
public:
    static constexpr std::size_t kMaxPlayers = 4;
    explicit Pad(float deadzone = 0.08f, float trigger_threshold = 0.5f);
    ~Pad();
    Pad(const Pad&) = delete;
    Pad& operator=(const Pad&) = delete;
    bool Open();
    bool Poll();
    bool TakeReturnToMenu() { return return_to_menu.exchange(false); }
    bool TakeHudToggle() { return hud_toggle.exchange(false); }
    // Players with a controller (bit per player), and those whose controller came or went.
    unsigned ConnectedPlayers() const { return connected_players.load(); }
    unsigned TakeConnectionChanges() { return connection_changes.exchange(0); }
    void Close();
    // Which DualSense button presses each of the game's buttons (button_mapping.h), for every
    // controller. Set before the first poll of a session.
    void SetMapping(const ButtonMapping& value) { mapping = ValidMapping(value) ? value : kDefaultMapping; }
    // How a player's controller is used: as usual, or in place of a single left or right Joy-Con.
    // A single Joy-Con has one stick and one cluster of four buttons, and its game reads nothing
    // else. Held upright, Consume makes the whole DualSense that Joy-Con: the shapes are its four
    // buttons, the arrows and either stick its stick, both pairs of shoulder buttons its two.
    // Held sideways (the game says so), the
    // DualSense is still held as usual, and Consume also turns what it sends by a quarter
    // (stick, button places, motion).
    enum class Grip : u8 { usual, sideways_left, sideways_right, upright_left, upright_right };
    void SetGrip(std::size_t player, Grip grip) {
        if (player < kMaxPlayers) grips[player].store(grip);
    }
    void Consume(std::span<const ps5::pad::Data> samples) { Consume(0, samples); }
    void Consume(std::size_t player, std::span<const ps5::pad::Data> samples);
    PadEngine& Engine() { return *engine; }
private:
    struct Slot {
        int user = -1;
        int handle = -1;
        u32 last_buttons = 0;
        u64 last_motion_us = 0;
        // The touchpad as a game button (the guest's Minus unless mapped otherwise), see Consume.
        bool touch_chord = false;  // this press was part of a shortcut
        bool select_held = false;  // a long press: its button stays down until the release
        unsigned touch_polls = 0;  // polls the touchpad has been down
        unsigned select_pulse = 0; // polls left of a tap's press
    };
    void Rescan();
    void OpenSlot(std::size_t player, int user);
    void CloseSlot(std::size_t player);
    std::shared_ptr<PadEngine> engine;
    float deadzone;
    float trigger_threshold;
    ButtonMapping mapping = kDefaultMapping;
    std::array<Slot, kMaxPlayers> slots{};
    std::array<std::atomic<Grip>, kMaxPlayers> grips{};
    bool owns_user_service = false;
    std::atomic<bool> return_to_menu = false;
    std::atomic<bool> hud_toggle = false;
    std::atomic<unsigned> connected_players = 0;
    std::atomic<unsigned> connection_changes = 0;
    unsigned polls_since_scan = 0;
    u64 polls = 0, samples_read = 0, usable_samples = 0, intercepted_samples = 0, circle_samples = 0, read_errors = 0;
    u64 rumble_updates = 0, rumble_errors = 0;
    int rumble_last_error = 0;
    int last_result = 0;
};

class AudioStream final : public AudioCore::Sink::SinkStream {
public:
    AudioStream(Core::System&, u32 channels, const std::string&, AudioCore::Sink::StreamType);
    ~AudioStream() override;
    void Start(bool resume = false) override;
    void Stop() override;
    void Finalize() override;
    void AppendBuffer(AudioCore::Sink::SinkBuffer&, std::span<s16>) override;
    bool Healthy() const { return !failed; }
private:
    int handle = -1;
    std::atomic<bool> failed = false;
    bool in_flight = false;
    u64 output_frames = 0;
    u64 nonzero_frames = 0;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread worker;
};

class AudioSink final : public AudioCore::Sink::Sink {
public:
    AudioCore::Sink::SinkStream* AcquireSinkStream(Core::System&, u32, const std::string&,
                                                  AudioCore::Sink::StreamType) override;
    void CloseStream(AudioCore::Sink::SinkStream*) override;
    void CloseStreams() override { streams.clear(); }
    f32 GetDeviceVolume() const override { return device_volume; }
    void SetDeviceVolume(f32) override;
    void SetSystemVolume(f32) override;
private:
    std::vector<AudioCore::Sink::SinkStreamPtr> streams;
    float device_volume = 1.0f;
    float system_volume = 1.0f;
};
}
