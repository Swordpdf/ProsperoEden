// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include "common/input.h"
#include "common/logging.h"
#include "input_common/input_poller.h"
#include <utility>

namespace {
// Signed-in users; unused entries are -1.
struct LoginUserIdList {
    std::int32_t user_id[4];
};
}
extern "C" int sceUserServiceGetForegroundUser(int* user);
extern "C" int sceUserServiceGetLoginUserIdList(LoginUserIdList* list);

namespace Eden {
// Look for newly signed-in (or signed-out) users about once a second at the 4 ms poll interval.
constexpr unsigned kPollsPerScan = 250;
// The touchpad is a game button (the guest's Minus unless the mapping says otherwise) as well as
// the shortcut key. So that a shortcut never reaches the game as a press, a tap presses the button
// when the touchpad is released (for about 100 ms), and a press held this long (about a quarter of
// a second) holds it.
constexpr unsigned kSelectTapPolls = 25;
constexpr unsigned kSelectHoldPolls = 60;
// Eden's virtual gamepad numbers the right Joy-Con's SL and SR after its named buttons
// (hid_core, emulated_controller.cpp: virtual_button_params).
constexpr int kRightSL = 20;
constexpr int kRightSR = 21;

PadEngine::PadEngine(std::string name) : InputEngine(std::move(name)) {
    for (std::size_t player = 0; player < kPlayers; ++player) PreSetController(Identifier(player));
}
PadIdentifier PadEngine::Identifier(std::size_t player) const {
    return {.guid = Common::UUID{}, .port = player, .pad = 0};
}
// Eden's handheld controller is a controller of its own (port 8), played on player 1's DualSense
// while it is the one in use (handheld_in_use, devices.h): what player 1 sends then goes to both.
constexpr std::size_t kHandheld = 8;

void PadEngine::SetButtonState(std::size_t player, int button, bool value) {
    if (player < kPlayers) SetButton(Identifier(player), button, value);
    if (player == 0 && handheld_in_use.load()) SetButton(Identifier(kHandheld), button, value);
}
void PadEngine::SetButtonState(std::size_t player, VirtualButton button, bool value) {
    SetButtonState(player, static_cast<int>(button), value);
}
void PadEngine::SetStickPosition(std::size_t player, int axis, float x, float y) {
    if (player >= kPlayers) return;
    SetAxis(Identifier(player), axis * 2, x);
    SetAxis(Identifier(player), axis * 2 + 1, y);
    if (player != 0 || !handheld_in_use.load()) return;
    SetAxis(Identifier(kHandheld), axis * 2, x);
    SetAxis(Identifier(kHandheld), axis * 2 + 1, y);
}
void PadEngine::SetMotionState(std::size_t player, u64 delta_us, float gyro_x, float gyro_y, float gyro_z,
                               float accel_x, float accel_y, float accel_z) {
    if (player >= kPlayers) return;
    const BasicMotion motion{.gyro_x = gyro_x, .gyro_y = gyro_y, .gyro_z = gyro_z,
                             .accel_x = accel_x, .accel_y = accel_y, .accel_z = accel_z,
                             .delta_timestamp = delta_us};
    SetMotion(Identifier(player), 0, motion);
    if (player == 0 && handheld_in_use.load()) SetMotion(Identifier(kHandheld), 0, motion);
}
void PadEngine::ResetControllers() {
    for (std::size_t player = 0; player < kPlayers; ++player) {
        SetStickPosition(player, 0, 0.0f, 0.0f);
        SetStickPosition(player, 1, 0.0f, 0.0f);
        for (int button = 0; button <= kRightSR; ++button)
            SetButtonState(player, button, false);
        SetMotionAtRest(player);
    }
}
void PadEngine::SetMotionAtRest(std::size_t player) {
    // Level and still: gravity along -Z.
    SetMotionState(player, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
}
Common::Input::DriverResult PadEngine::SetVibration(const PadIdentifier& identifier,
                                                    const Common::Input::VibrationStatus& vibration) {
    // The handheld controller (index 8) is played on player 1's DualSense.
    const std::size_t player = identifier.port == 8 ? 0 : identifier.port;
    if (player >= kPlayers) return Common::Input::DriverResult::InvalidParameters;
    std::scoped_lock lock(rumble_mutex);
    auto& sides = rumble[player];
    (identifier.pad == 2 ? sides.right : sides.left) = vibration;
    sides.changed = true;
    return Common::Input::DriverResult::Success;
}
bool PadEngine::TakeRumble(std::size_t player, Rumble& out) {
    if (player >= kPlayers) return false;
    std::scoped_lock lock(rumble_mutex);
    auto& sides = rumble[player];
    if (!sides.changed) return false;
    sides.changed = false;
    // The console's HD rumble per side -> one DualSense: the low band drives the large motor and the
    // high band the small one, with the amplitude curves of Eden's SDL driver.
    const auto level = [](float amplitude, Common::Input::VibrationAmplificationType type) {
        const float factor = type == Common::Input::VibrationAmplificationType::Linear ? 0.5f : 0.35f;
        amplitude = std::clamp(amplitude, 0.0f, 1.0f);
        return static_cast<u8>(std::lround((amplitude + std::pow(amplitude, factor)) * 0.5f * 255.0f));
    };
    out.large = std::max(level(sides.left.low_amplitude, sides.left.type),
                         level(sides.right.low_amplitude, sides.right.type));
    out.small = std::max(level(sides.left.high_amplitude, sides.left.type),
                         level(sides.right.high_amplitude, sides.right.type));
    return true;
}

Pad::Pad(float deadzone_, float threshold)
    : engine{std::make_shared<PadEngine>("virtual_gamepad")},
      deadzone{deadzone_}, trigger_threshold{threshold} {
    if (!std::isfinite(deadzone) || deadzone < 0 || deadzone >= 1 ||
        !std::isfinite(threshold) || threshold <= 0 || threshold > 1)
        throw std::invalid_argument("Invalid pad calibration");
    Common::Input::RegisterInputFactory("virtual_gamepad",
        std::make_shared<InputCommon::InputFactory>(engine));
    Common::Input::RegisterOutputFactory("virtual_gamepad",
        std::make_shared<InputCommon::OutputFactory>(engine));
}
Pad::~Pad() {
    Close();
    Common::Input::UnregisterInputFactory("virtual_gamepad");
    Common::Input::UnregisterOutputFactory("virtual_gamepad");
}
bool Pad::Open() {
    if (slots[0].handle >= 0) return true;
    owns_user_service = sceUserServiceInitialize(nullptr) == 0;
    int user = -1;
    // Player 1 is the application user, as for the title-aware launch controller.
    if (sceUserServiceGetForegroundUser(&user) < 0 || user < 0 || scePadInit() < 0) {
        Close();
        return false;
    }
    OpenSlot(0, user);
    if (slots[0].handle < 0) { Close(); return false; }
    int initial_user = -1;
    const int initial_result = sceUserServiceGetInitialUser(&initial_user);
    LOG_INFO(Input, "EDEN_PAD_USERS foreground={} initial={} initial_rc={}", user, initial_user, initial_result);
    Rescan();
    return true;
}
void Pad::OpenSlot(std::size_t player, int user) {
    int handle = scePadOpen(user, ps5::pad::kPortTypeStandard, 0, nullptr);
    // A handle the process already holds for this user (e.g. from the launcher) is reused.
    if (handle < 0) handle = scePadGetHandle(user, ps5::pad::kPortTypeStandard, 0);
    if (handle < 0) {
        LOG_WARNING(Input, "EDEN_PAD_OPEN_FAILED player={} user={} result={:#x}", player + 1, user,
                    static_cast<unsigned>(handle));
        return;
    }
    slots[player] = {user, handle, 0, 0};
    // Classic two-motor rumble, and motion reports (on by default; failures only lose the feature).
    const int vibration_mode = scePadSetVibrationMode(handle, ps5::pad::kVibrationModeCompatible);
    const int motion = scePadSetMotionSensorState(handle, true);
    if (vibration_mode < 0 || motion < 0)
        LOG_INFO(Input, "EDEN_PAD_FEEDBACK player={} vibration_mode={:#x} motion={:#x}", player + 1,
                 static_cast<unsigned>(vibration_mode), static_cast<unsigned>(motion));
    connected_players |= 1u << player;
    connection_changes |= 1u << player;
    LOG_INFO(Input, "EDEN_PAD_OPEN player={} user={} handle={}", player + 1, user, handle);
}
void Pad::CloseSlot(std::size_t player) {
    auto& slot = slots[player];
    if (slot.handle < 0) return;
    const auto neutral = ps5::pad::neutral_data();
    Consume(player, {&neutral, 1});
    const ps5::pad::Vibration stop{};
    (void)scePadSetVibration(slot.handle, &stop);
    const int closed = scePadClose(slot.handle);
    LOG_INFO(Input, "EDEN_PAD_CLOSE player={} user={} close={}", player + 1, slot.user, closed);
    slot = {};
    connected_players &= ~(1u << player);
    connection_changes |= 1u << player;
}
void Pad::Rescan() {
    LoginUserIdList list;
    std::fill(std::begin(list.user_id), std::end(list.user_id), -1);
    if (sceUserServiceGetLoginUserIdList(&list) < 0) return;
    const auto signed_in = [&](int user) {
        return std::find(std::begin(list.user_id), std::end(list.user_id), user) != std::end(list.user_id);
    };
    // Player 1 stays with the launching user; other players leave when their user signs out.
    for (std::size_t player = 1; player < kMaxPlayers; ++player) {
        if (slots[player].handle >= 0 && !signed_in(slots[player].user)) CloseSlot(player);
    }
    for (const int user : list.user_id) {
        if (user < 0 || std::any_of(slots.begin(), slots.end(), [user](const Slot& slot) {
                return slot.handle >= 0 && slot.user == user; }))
            continue;
        for (std::size_t player = 1; player < kMaxPlayers; ++player) {
            if (slots[player].handle < 0) {
                OpenSlot(player, user);
                break;
            }
        }
    }
}
void Pad::Close() {
    engine->ResetControllers();
    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
        if (slots[player].handle < 0) continue;
        const ps5::pad::Vibration stop{};
        (void)scePadSetVibration(slots[player].handle, &stop);
        const int closed = scePadClose(slots[player].handle);
        if (player == 0) {
            LOG_INFO(Input, "EDEN_PAD_CLOSED polls={} samples={} usable={} intercepted={} circle={} errors={} last={} close={} "
                     "rumble={} rumble_errors={} rumble_last={:#x}",
                     polls, samples_read, usable_samples, intercepted_samples, circle_samples, read_errors, last_result, closed,
                     rumble_updates, rumble_errors, static_cast<unsigned>(rumble_last_error));
        }
        slots[player] = {};
    }
    connected_players = 0;
    connection_changes = 0;
    if (owns_user_service) { sceUserServiceTerminate(); owns_user_service = false; }
}
bool Pad::Poll() {
    if (slots[0].handle >= 0 && ++polls_since_scan >= kPollsPerScan) {
        polls_since_scan = 0;
        Rescan();
    }
    ++polls;
    bool primary = false;
    std::array<ps5::pad::Data, ps5::pad::kMaxSamples> samples{};
    for (std::size_t player = 0; player < kMaxPlayers; ++player) {
        const int handle = slots[player].handle;
        if (handle < 0) continue;
        PadEngine::Rumble rumble;
        if (engine->TakeRumble(player, rumble)) {
            const ps5::pad::Vibration vibration{.large_motor = rumble.large, .small_motor = rumble.small};
            const int result = scePadSetVibration(handle, &vibration);
            ++rumble_updates;
            if (result < 0) { ++rumble_errors; rumble_last_error = result; }
        }
        const int count = scePadRead(handle, samples.data(), samples.size());
        if (player == 0) last_result = count;
        if (count < 0 || count > static_cast<int>(samples.size())) {
            ++read_errors;
            const auto neutral = ps5::pad::neutral_data();
            Consume(player, {&neutral, 1});
            continue;
        }
        for (int i = 0; i < count; ++i) {
            ++samples_read;
            usable_samples += ps5::pad::is_usable(samples[i]);
            intercepted_samples += (samples[i].buttons & ps5::pad::kButtonIntercepted) != 0;
            circle_samples += ps5::pad::is_usable(samples[i]) && (samples[i].buttons & ps5::pad::kButtonCircle);
        }
        Consume(player, {samples.data(), static_cast<std::size_t>(count)});
        primary |= player == 0;
    }
    return primary;
}
void Pad::Consume(std::size_t player, std::span<const ps5::pad::Data> samples) {
    using namespace ps5::pad;
    using Button = InputCommon::VirtualGamepad::VirtualButton;
    // The game's buttons and the DualSense buttons, in the order of button_mapping.h.
    static constexpr Button kGame[kGameButtons] = {
        Button::ButtonA, Button::ButtonB, Button::ButtonX, Button::ButtonY, Button::TriggerL,
        Button::TriggerR, Button::TriggerZL, Button::TriggerZR, Button::ButtonPlus, Button::ButtonMinus,
        Button::StickL, Button::StickR};
    static constexpr ButtonMask kPad[kPadButtons] = {
        kButtonCross, kButtonCircle, kButtonSquare, kButtonTriangle, kButtonL1, kButtonR1, kButtonL2,
        kButtonR2, kButtonL3, kButtonR3, kButtonOptions, kButtonCreate, kButtonTouchPad};
    static constexpr std::pair<ButtonMask, Button> fixed[] = {
        {kButtonLeft, Button::ButtonLeft}, {kButtonUp, Button::ButtonUp},
        {kButtonRight, Button::ButtonRight}, {kButtonDown, Button::ButtonDown},
        // SL and SR, the shoulder buttons of a single Joy-Con held sideways: L1 and R1 press them
        // too. A game that takes single Joy-Cons asks for SL + SR on its controller screen, and
        // nothing pressed them before. Eden passes them to the game only for that controller
        // style (left Joy-Con: virtual buttons 16 and 17, right Joy-Con: kRightSL and kRightSR).
        {kButtonL1, Button::ButtonSL}, {kButtonR1, Button::ButtonSR},
        {kButtonL1, static_cast<Button>(kRightSL)}, {kButtonR1, static_cast<Button>(kRightSR)},
    };
    auto& slot = slots[player];
    const Grip grip = grips[player].load();
    // The game button the touchpad presses (tap or hold, see kSelectTapPolls); the Create button
    // presses it too while it has no game button of its own.
    const int touch_button = MappedTo(mapping, pad_touchpad);
    const bool create_free = MappedTo(mapping, pad_create) < 0;
    const auto set_touch = [&](u32 pressed) {
        if (touch_button < 0) return;
        engine->SetButtonState(player, kGame[touch_button],
            (create_free && (pressed & kButtonCreate) != 0) || slot.select_held || slot.select_pulse > 0);
    };
    // Whether the DualSense button mapped to a game button is down (the analog triggers count
    // from the threshold; the touchpad as set_touch has it).
    const auto down = [&](int game, u32 buttons, bool left, bool right) {
        const int pad = mapping[game];
        if (pad == pad_touchpad)
            return (create_free && (buttons & kButtonCreate) != 0) || slot.select_held || slot.select_pulse > 0;
        return (buttons & kPad[pad]) != 0 || (pad == pad_l2 && left) || (pad == pad_r2 && right);
    };
    // A single Joy-Con held upright has Minus or Plus, not both: what presses either presses
    // the one it has. Applied after every set_touch, which sets the touchpad's button alone.
    const auto set_menu = [&](u32 buttons, bool left, bool right) {
        if (grip != Grip::upright_left && grip != Grip::upright_right) return;
        engine->SetButtonState(player, grip == Grip::upright_left ? Button::ButtonMinus : Button::ButtonPlus,
                               down(game_minus, buttons, left, right) || down(game_plus, buttons, left, right));
    };
    bool last_left = false, last_right = false;  // the triggers of the poll's last sample
    const auto axis = [this](u8 raw) {
        const float x = (static_cast<int>(raw) - 128) / (raw < 128 ? 128.0f : 127.0f);
        return std::abs(x) <= deadzone ? 0.0f :
            std::copysign((std::abs(x) - deadzone) / (1.0f - deadzone), x);
    };
    auto& last_buttons = slots[player].last_buttons;
    for (const auto& raw : samples) {
        auto sample = is_usable(raw) ? raw : neutral_data(raw.timestamp_us);
        // The shortcuts work from every controller.
        constexpr ButtonMask menu_chord = kButtonTouchPad | kButtonL1;
        constexpr ButtonMask hud_chord = kButtonTouchPad | kButtonR1;
        const auto pressed = sample.buttons;
        if ((pressed & menu_chord) == menu_chord &&
            (last_buttons & menu_chord) != menu_chord)
            return_to_menu = true;
        if ((pressed & hud_chord) == hud_chord &&
            (last_buttons & hud_chord) != hud_chord)
            hud_toggle = true;
        if ((pressed & menu_chord) == menu_chord || (pressed & hud_chord) == hud_chord)
            sample.buttons &= ~(kButtonTouchPad | kButtonL1 | kButtonR1);
        // The touchpad on its own: see kSelectTapPolls.
        const bool touch = (pressed & kButtonTouchPad) != 0;
        const bool touched = (last_buttons & kButtonTouchPad) != 0;
        if (touch && !touched) {
            slot.touch_chord = false;
            slot.touch_polls = 0;
        }
        if (touch && (pressed & (kButtonL1 | kButtonR1)) != 0) {
            slot.touch_chord = true;
            slot.select_held = false;
        }
        if (!touch && touched) {
            if (!slot.touch_chord && !slot.select_held) slot.select_pulse = kSelectTapPolls + 1;
            slot.select_held = false;
        }
        if (!is_usable(raw)) {
            // A controller that went away neither tapped nor holds anything.
            slot.select_pulse = 0;
            slot.select_held = false;
        }
        last_buttons = pressed;
        for (const auto [mask, button] : fixed)
            engine->SetButtonState(player, button, (sample.buttons & mask) != 0);
        // The game's buttons are digital: the analog triggers count from the threshold.
        const bool left = sample.triggers.l2 / 255.0f >= trigger_threshold;
        const bool right = sample.triggers.r2 / 255.0f >= trigger_threshold;
        for (int game = 0; game < kGameButtons; ++game) {
            const int pad = mapping[game];
            if (pad == pad_touchpad) continue;  // set_touch
            engine->SetButtonState(player, kGame[game], (sample.buttons & kPad[pad]) != 0 ||
                                                        (pad == pad_l2 && left) || (pad == pad_r2 && right));
        }
        set_touch(sample.buttons);
        last_left = left;
        last_right = right;
        float left_x = axis(sample.left_stick.x), left_y = -axis(sample.left_stick.y);
        float right_x = axis(sample.right_stick.x), right_y = -axis(sample.right_stick.y);
        if (grip == Grip::upright_left || grip == Grip::upright_right) {
            // A single Joy-Con held upright. The game reads only what that Joy-Con has: one
            // stick, four buttons (a left Joy-Con's arrows, where Right is the game's A, Down
            // its B, Up its X and Left its Y; a right Joy-Con's A, B, X and Y), two shoulder
            // buttons, Minus or Plus (set_menu), and the stick's click. On the DualSense the
            // buttons mapped to A, B, X and Y are those four, and the arrows move the stick as
            // either stick does: a Joy-Con has no arrows besides its four buttons. L and R,
            // ZL and ZR, Plus and Minus, L3 and R3 each press the one the Joy-Con has.
            const auto held = [&](int game) { return down(game, sample.buttons, left, right); };
            const auto arrow = [&](ButtonMask mask) { return (sample.buttons & mask) != 0; };
            const bool upright_left = grip == Grip::upright_left;
            if (upright_left) {
                engine->SetButtonState(player, Button::ButtonUp, held(game_x));
                engine->SetButtonState(player, Button::ButtonRight, held(game_a));
                engine->SetButtonState(player, Button::ButtonDown, held(game_b));
                engine->SetButtonState(player, Button::ButtonLeft, held(game_y));
            }
            engine->SetButtonState(player, upright_left ? Button::TriggerL : Button::TriggerR,
                                   held(game_l) || held(game_r));
            engine->SetButtonState(player, upright_left ? Button::TriggerZL : Button::TriggerZR,
                                   held(game_zl) || held(game_zr));
            set_menu(sample.buttons, left, right);
            engine->SetButtonState(player, upright_left ? Button::StickL : Button::StickR,
                                   held(game_left_stick) || held(game_right_stick));
            const float arrows_x = static_cast<float>(arrow(kButtonRight)) - static_cast<float>(arrow(kButtonLeft));
            const float arrows_y = static_cast<float>(arrow(kButtonUp)) - static_cast<float>(arrow(kButtonDown));
            if (arrows_x != 0.0f || arrows_y != 0.0f) {
                // Full tilt, also on a diagonal.
                const float tilt = arrows_x != 0.0f && arrows_y != 0.0f ? std::numbers::sqrt2_v<float> / 2.0f : 1.0f;
                left_x = right_x = arrows_x * tilt;
                left_y = right_y = arrows_y * tilt;
            } else if (right_x * right_x + right_y * right_y > left_x * left_x + left_y * left_y) {
                left_x = right_x;
                left_y = right_y;
            } else {
                right_x = left_x;
                right_y = left_y;
            }
        } else if (grip != Grip::usual) {
            // A single Joy-Con held sideways: its game reads the stick and the four buttons by
            // where they are in that grip. The left one lies with its top to the left, the right
            // one with its top to the right; the DualSense is held as usual, so what it sends is
            // turned by a quarter the other way. A place (top, right, bottom, left) is pressed by
            // the arrow or the shape there, and either stick is the Joy-Con's.
            const auto at = [&](ButtonMask arrow, ButtonMask shape) {
                return (sample.buttons & (arrow | shape)) != 0;
            };
            const bool top = at(kButtonUp, kButtonTriangle), east = at(kButtonRight, kButtonCircle);
            const bool bottom = at(kButtonDown, kButtonCross), west = at(kButtonLeft, kButtonSquare);
            const bool sideways_left = grip == Grip::sideways_left;
            engine->SetButtonState(player, sideways_left ? Button::ButtonRight : Button::ButtonY, top);
            engine->SetButtonState(player, sideways_left ? Button::ButtonDown : Button::ButtonX, east);
            engine->SetButtonState(player, sideways_left ? Button::ButtonLeft : Button::ButtonA, bottom);
            engine->SetButtonState(player, sideways_left ? Button::ButtonUp : Button::ButtonB, west);
            const bool use_right = right_x * right_x + right_y * right_y > left_x * left_x + left_y * left_y;
            const float x = use_right ? right_x : left_x, y = use_right ? right_y : left_y;
            left_x = right_x = sideways_left ? y : -y;
            left_y = right_y = sideways_left ? -x : x;
        }
        engine->SetStickPosition(player, 0, left_x, left_y);
        engine->SetStickPosition(player, 1, right_x, right_y);
        // Motion: acceleration in G and angular velocity in rad/s, mapped to Eden's axes and units
        // the way its SDL driver maps a DualSense.
        auto& last_motion = slots[player].last_motion_us;
        if (!is_usable(raw)) {
            // Away (disconnected, or input taken by the system): stop, rather than keep turning.
            if (last_motion) engine->SetMotionAtRest(player);
            last_motion = 0;
        } else if (raw.timestamp_us > last_motion) {
            const u64 delta = last_motion ? raw.timestamp_us - last_motion : 0;
            last_motion = raw.timestamp_us;
            if (delta > 0 && delta < 1'000'000) {
                constexpr float turn = 2.0f * std::numbers::pi_v<float>;
                const auto& a = raw.acceleration;
                const auto& w = raw.angular_velocity;
                // Eden's axes: X to the right, Y away from the player, Z up. For a Joy-Con held
                // sideways they turn about Z with the stick and the buttons (see above).
                float gyro_x = w.x / turn, gyro_y = -w.z / turn, accel_x = -a.x, accel_y = a.z;
                if (grip == Grip::sideways_left) {
                    gyro_x = std::exchange(gyro_y, -gyro_x);
                    accel_x = std::exchange(accel_y, -accel_x);
                } else if (grip == Grip::sideways_right) {
                    gyro_y = std::exchange(gyro_x, -gyro_y);
                    accel_y = std::exchange(accel_x, -accel_y);
                }
                engine->SetMotionState(player, delta, gyro_x, gyro_y, w.y / turn, accel_x, accel_y, -a.y);
            }
        }
    }
    // Once per poll: a touchpad held long enough becomes a held press, and a tap's press runs out.
    if ((last_buttons & kButtonTouchPad) != 0 && !slot.touch_chord && !slot.select_held &&
        ++slot.touch_polls >= kSelectHoldPolls)
        slot.select_held = true;
    if (slot.select_pulse > 0) --slot.select_pulse;
    set_touch(last_buttons);
    set_menu(last_buttons, last_left, last_right);
}
}
