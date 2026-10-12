// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"
#include "controller_applet.h"
#include "keyboard_applet.h"
#include <condition_variable>
#include <mutex>
#include "mock_devices.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include "common/fs/path_util.h"
#include "common/input.h"
#include "common/logging.h"
#include "common/settings.h"
#include "audio_core/sink/sink_details.h"
#include "core/core.h"
#include "hid_core/resources/shared_memory_format.h"
#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/renderer/voice/voice_info.h"
#include "audio_core/renderer/voice/voice_channel_resource.h"
#include "audio_core/renderer/mix/mix_info.h"
#include "audio_core/renderer/sink/sink_info_base.h"
#include "audio_core/renderer/performance/performance_manager.h"
#include "audio_core/renderer/memory/pool_mapper.h"
#include "audio_core/renderer/command/resample/resample.h"

// Freestanding renderer fixture layout must match the pinned core's IPC ABI.
static_assert(sizeof(AudioCore::AudioRendererParameterInternal) == 0x34);
using namespace AudioCore::Renderer;
static_assert(sizeof(BehaviorInfo::InParameter) == 0x10);
static_assert(sizeof(BehaviorInfo::OutStatus) == 0xb0);
static_assert(offsetof(BehaviorInfo::OutStatus, error_count) == 0xa0);
static_assert(sizeof(MemoryPoolInfo::InParameter) == 0x20);
static_assert(sizeof(MemoryPoolInfo::OutStatus) == 0x10);
static_assert(sizeof(VoiceChannelResource::InParameter) == 0x70);
static_assert(sizeof(VoiceInfo::InParameter) == 0x170);
static_assert(offsetof(VoiceInfo::InParameter, wave_buffer_internal) == 0x60);
static_assert(offsetof(VoiceInfo::InParameter, channel_resource_ids) == 0x140);
static_assert(offsetof(VoiceInfo::InParameter, flags) == 0x15c);
static_assert(offsetof(VoiceInfo::InParameter, src_quality) == 0x15e);
static_assert(static_cast<unsigned>(AudioCore::SrcQuality::Low) == 2);
static_assert(static_cast<unsigned>(AudioCore::SrcQuality::High) == 1);
static_assert(offsetof(VoiceChannelResource::InParameter, mix_volumes) == 4);
static_assert(offsetof(VoiceChannelResource::InParameter, in_use) == 0x64);
static_assert(offsetof(SinkInfoBase::InParameter, device) == 0x20);
static_assert(offsetof(SinkInfoBase::DeviceInParameter, inputs) == 0x104);
static_assert(static_cast<unsigned>(AudioCore::SampleFormat::PcmInt16) == 2);
static_assert(sizeof(VoiceInfo::OutStatus) == 0x10);
static_assert(sizeof(MixInfo::InParameter) == 0x930);
static_assert(offsetof(MixInfo::InParameter, dest_mix_id) == 0x924);
static_assert(sizeof(SinkInfoBase::InParameter) == 0x140);
static_assert(sizeof(SinkInfoBase::OutStatus) == 0x20);
static_assert(sizeof(PerformanceManager::InParameter) == 0x10);
static_assert(sizeof(PerformanceManager::OutStatus) == 0x10);

// Guest fixture reads this documented shared-memory ABI without linking a guest SDK.
static_assert(offsetof(Service::HID::SharedMemoryFormat, npad) == 0x9a00);
static_assert(offsetof(Service::HID::NpadInternalState, fullkey_lifo) == 0x28);
static_assert(sizeof(Service::HID::NPadGenericState) == 0x28);
static_assert(offsetof(Service::HID::NPadGenericState, npad_buttons) == 8);
static_assert(offsetof(Service::HID::NPadGenericState, connection_status) == 0x20);
using GuestPadRing = Service::HID::Lifo<Service::HID::NPadGenericState, 17>;
static_assert(offsetof(GuestPadRing, entries) == 0x20);
static_assert(sizeof(GuestPadRing::entries[0]) == 0x30);

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using Eden::Mock::state;
using namespace AudioCore::Sink;
using namespace std::chrono_literals;
template<class F> void Reject(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}

// A game's "connect controllers" screen: every request gets an answer for player 1, so no game
// waits on that screen for a controller that never comes.
void CheckControllerStyle() {
    using Core::HID::NpadStyleIndex;
    Core::Frontend::ControllerParameters allowed{};
    // A game's own Controller type goes first when the game takes it, the handheld for player 1
    // only; one the game does not take is left aside.
    {
        Core::Frontend::ControllerParameters taken{};
        taken.allow_pro_controller = taken.allow_handheld = taken.allow_left_joycon = true;
        CHECK(Eden::ControllerStyle(taken, 0, -1) == NpadStyleIndex::Fullkey);
        CHECK(Eden::ControllerStyle(taken, 1, 3) == NpadStyleIndex::JoyconLeft);
        CHECK(Eden::ControllerStyle(taken, 0, 1) == NpadStyleIndex::Handheld);
        CHECK(!Eden::ControllerStyle(taken, 1, 1));
        CHECK(Eden::ControllerStyle(taken, 0, 4) == NpadStyleIndex::Fullkey);  // right Joy-Con: not taken
        CHECK(Eden::ControllerStyle(taken, 0, 2) == NpadStyleIndex::Fullkey);  // pair: not taken
        CHECK(Eden::ControllerSetting(-1) == Settings::ControllerType::ProController &&
              Eden::ControllerSetting(1) == Settings::ControllerType::Handheld &&
              Eden::ControllerSetting(4) == Settings::ControllerType::RightJoycon);
    }
    // A game that names none of the styles: a Pro Controller for every player.
    CHECK(Eden::ControllerStyle(allowed, 0) == NpadStyleIndex::Fullkey);
    CHECK(Eden::ControllerStyle(allowed, 3) == NpadStyleIndex::Fullkey);
    // A handheld-only game: the handheld for player 1 (in either console mode), nobody else.
    allowed.allow_handheld = true;
    CHECK(Eden::ControllerStyle(allowed, 0) == NpadStyleIndex::Handheld);
    CHECK(!Eden::ControllerStyle(allowed, 1));
    allowed.allow_right_joycon = true;
    CHECK(Eden::ControllerStyle(allowed, 0) == NpadStyleIndex::JoyconRight);
    allowed.allow_right_joycon = false; allowed.allow_left_joycon = true;
    CHECK(Eden::ControllerStyle(allowed, 1) == NpadStyleIndex::JoyconLeft);
    allowed.allow_right_joycon = true;
    CHECK(Eden::ControllerStyle(allowed, 0) == NpadStyleIndex::JoyconLeft);
    CHECK(Eden::ControllerStyle(allowed, 1) == NpadStyleIndex::JoyconRight);
    CHECK(Eden::ControllerStyle(allowed, 2) == NpadStyleIndex::JoyconLeft);
    allowed.allow_dual_joycons = true;
    CHECK(Eden::ControllerStyle(allowed, 1) == NpadStyleIndex::JoyconDual);
    allowed.allow_pro_controller = true;
    CHECK(Eden::ControllerStyle(allowed, 3) == NpadStyleIndex::Fullkey);
    std::puts("Controller applet styles: pro, pair, single Joy-Cons, handheld in either mode, and a Pro Controller when none is named PASS");
}

// A game's text entry (keyboard_applet.h) with a keyboard that answers as told.
void CheckKeyboard() {
    using Eden::TextOutcome;
    using Result = Service::AM::Frontend::SwkbdResult;
    using Reply = Service::AM::Frontend::SwkbdReplyType;
    std::mutex mutex;
    std::condition_variable changed;
    int answers_given = 0;
    Result result{};
    std::u16string text;
    bool confirmed = false;
    std::vector<Reply> replies;
    const auto wait = [&](int count) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 5s, [&] { return answers_given >= count; }));
    };
    const auto normal = [&](Result result_, std::u16string text_, bool confirmed_) {
        const std::lock_guard lock(mutex);
        result = result_; text = std::move(text_); confirmed = confirmed_;
        ++answers_given;
        changed.notify_all();
    };
    std::vector<Eden::TextAnswer> script;
    std::atomic<int> asked{0};
    std::atomic<bool> hold{false};
    Eden::TextRequest last;
    Eden::SystemKeyboardApplet applet([&](const Eden::TextRequest& request, const std::atomic<bool>& stop) {
        last = request;
        while (hold && !stop) std::this_thread::sleep_for(5ms);
        if (hold) return Eden::TextAnswer{TextOutcome::cancelled, {}};
        return script[std::min<std::size_t>(asked++, script.size() - 1)];
    });
    Core::Frontend::KeyboardInitializeParameters wanted{};
    wanted.header_text = u"Name"; wanted.initial_text = u"Alex"; wanted.max_text_length = 8; wanted.min_text_length = 2;
    const auto ask = [&](std::vector<Eden::TextAnswer> answers) {
        script = std::move(answers); asked = 0;
        applet.InitializeKeyboard(false, wanted, normal, {});
        applet.ShowNormalKeyboard();
    };
    // What the player enters is the game's to check.
    ask({{TextOutcome::accepted, u"Robin"}}); wait(1);
    CHECK(result == Result::Ok && text == u"Robin" && !confirmed);
    CHECK(last.title == u"Name" && last.initial == u"Alex" && last.max_length == 8 && !last.numbers && !last.password);
    // A text shorter than the game asks for is asked for again, from what was entered.
    ask({{TextOutcome::accepted, u"R"}, {TextOutcome::accepted, u"Ro"}}); wait(2);
    CHECK(result == Result::Ok && text == u"Ro" && asked == 2 && last.initial == u"R");
    // Closing the keyboard cancels the entry.
    ask({{TextOutcome::cancelled, {}}}); wait(3);
    CHECK(result == Result::Cancel && asked == 1);
    // A game without a cancel button is asked again, then gets the text it started from.
    wanted.disable_cancel_button = true;
    ask({{TextOutcome::cancelled, {}}}); wait(4);
    CHECK(result == Result::Ok && text == u"Alex" && confirmed && asked == Eden::SystemKeyboardApplet::kAttempts);
    wanted.disable_cancel_button = false;
    // No keyboard: the starting text, or "Eden" in the lengths the game asks for.
    ask({{TextOutcome::unavailable, {}}}); wait(5);
    CHECK(result == Result::Ok && text == u"Alex" && confirmed);
    wanted.initial_text.clear(); wanted.max_text_length = 3;
    ask({{TextOutcome::unavailable, {}}}); wait(6);
    CHECK(result == Result::Ok && text == u"Ede" && confirmed);
    wanted.max_text_length = 8; wanted.min_text_length = 6;
    ask({{TextOutcome::unavailable, {}}}); wait(7);
    CHECK(text == u"Eden00");
    wanted.min_text_length = 2;
    // The game refuses a text: asked again; and again: the entry is cancelled.
    ask({{TextOutcome::accepted, u"Robin"}}); wait(8);
    applet.ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult::Failure, u"Not this one"); wait(9);
    CHECK(result == Result::Ok && last.initial == u"Robin");
    applet.ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult::Failure, u"Not this one"); wait(10);
    applet.ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult::Failure, u"Not this one"); wait(11);
    CHECK(result == Result::Cancel && confirmed);
    // A text the game wants confirmed is confirmed.
    ask({{TextOutcome::accepted, u"Robin"}}); wait(12);
    applet.ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult::Confirm, u"Sure?"); wait(13);
    CHECK(result == Result::Ok && text == u"Robin" && confirmed);
    // A request closed while its keyboard is open: the keyboard is told to close, no answer comes.
    hold = true;
    applet.InitializeKeyboard(false, wanted, normal, {});
    applet.ShowNormalKeyboard();
    std::this_thread::sleep_for(50ms);
    applet.ExitKeyboard();
    std::this_thread::sleep_for(200ms);
    hold = false;
    { const std::lock_guard lock(mutex); CHECK(answers_given == 13); }
    // A keyboard inside the game's own screen: the text, then the decision.
    std::u16string inline_text;
    const auto inline_reply = [&](Reply reply, std::u16string text_, s32) {
        const std::lock_guard lock(mutex);
        replies.push_back(reply); inline_text = std::move(text_);
        ++answers_given;
        changed.notify_all();
    };
    script = {{TextOutcome::accepted, u"Hi"}}; asked = 0;
    applet.InitializeKeyboard(true, wanted, {}, inline_reply);
    applet.ShowInlineKeyboard({});
    wait(15);
    CHECK(replies.size() == 2 && replies[0] == Reply::ChangedString && replies[1] == Reply::DecidedEnter && inline_text == u"Hi");
    script = {{TextOutcome::cancelled, {}}}; asked = 0;
    applet.ShowInlineKeyboard({});
    wait(16);
    CHECK(replies.back() == Reply::DecidedCancel && inline_text == u"Hi");
    std::puts("Text entry: entered, too short, cancelled, no cancel button, no keyboard, refused, confirmed, closed while open, inline PASS");
}

void CheckPad() {
    using namespace ps5::pad;
    Reject([] { Eden::Pad pad(1.0f); });
    {
        Eden::Pad pad;
        CHECK(pad.Open()); CHECK(pad.Open()); CHECK(state.pad_opens == 1);
        CHECK(state.last_pad_user == 84); // Foreground user differs from initial login (42).
        auto sample = neutral_data(); sample.connected = 1;
        const auto consume = [&] { pad.Consume({&sample, 1}); };
        const std::pair<ButtonMask, int> mapping[] = {
            {kButtonCircle, 0}, {kButtonCross, 1}, {kButtonTriangle, 2}, {kButtonSquare, 3},
            {kButtonL3, 4}, {kButtonR3, 5}, {kButtonL1, 6}, {kButtonR1, 7},
            {kButtonL2, 8}, {kButtonR2, 9}, {kButtonOptions, 10}, {kButtonCreate, 11},
            {kButtonLeft, 12}, {kButtonUp, 13}, {kButtonRight, 14}, {kButtonDown, 15},
        };
        for (auto [mask, button] : mapping) {
            sample.buttons = mask; consume();
            // L1 and R1 are also SL and SR of a single Joy-Con (left: 16 and 17, right: 20 and 21).
            const int sl_sr = mask == kButtonL1 ? 16 : mask == kButtonR1 ? 17 : -1;
            for (int i = 0; i < 22; ++i)
                CHECK(pad.Engine().GetButton({}, i) == (i == button || (sl_sr >= 0 && (i == sl_sr || i == sl_sr + 4))));
        }
        // Player 1's DualSense plays Eden's handheld controller (port 8) only while that is the
        // controller in use: a button that reached it at any other time would connect it.
        sample.buttons = kButtonCircle; sample.left_stick = {0, 255}; consume();
        CHECK(pad.Engine().GetButton({}, 0)); CHECK(!pad.Engine().GetButton({.port = 8}, 0));
        CHECK(pad.Engine().GetAxis({.port = 8}, 0) == 0);
        sample.buttons = 0; sample.left_stick = {128, 128}; consume();
        Eden::handheld_in_use = true;
        sample.buttons = kButtonCircle; consume();
        CHECK(pad.Engine().GetButton({.port = 8}, 0)); CHECK(!pad.Engine().GetButton({.port = 8}, 1));
        sample.buttons = 0;
        sample.left_stick = {0, 255}; sample.right_stick = {255, 0}; consume();
        CHECK(!pad.Engine().GetButton({.port = 8}, 0));
        CHECK(pad.Engine().GetAxis({.port = 8}, 0) == -1); CHECK(pad.Engine().GetAxis({.port = 8}, 3) == 1);
        Eden::handheld_in_use = false;
        CHECK(pad.Engine().GetAxis({}, 0) == -1); CHECK(pad.Engine().GetAxis({}, 1) == -1);
        CHECK(pad.Engine().GetAxis({}, 2) == 1); CHECK(pad.Engine().GetAxis({}, 3) == 1);
        sample.left_stick = {128, 132}; sample.triggers = {127, 128}; consume();
        CHECK(pad.Engine().GetAxis({}, 0) == 0); CHECK(pad.Engine().GetAxis({}, 1) == 0);
        CHECK(!pad.Engine().GetButton({}, 8)); CHECK(pad.Engine().GetButton({}, 9));

        const auto none_pressed = [&] {
            for (int i = 0; i < 22; ++i) if (pad.Engine().GetButton({}, i)) return false;
            return true;
        };
        sample.triggers = {};
        sample.buttons = kButtonTouchPad | kButtonL1; consume();
        CHECK(pad.TakeReturnToMenu()); CHECK(!pad.TakeReturnToMenu());
        CHECK(none_pressed());
        consume(); CHECK(!pad.TakeReturnToMenu());
        sample.buttons = 0; consume();
        sample.buttons = kButtonTouchPad | kButtonR1; consume();
        CHECK(pad.TakeHudToggle()); CHECK(!pad.TakeHudToggle());
        CHECK(none_pressed());
        sample.buttons = 0; consume();
        // A shortcut is never a press of Minus, however long its keys take to come up.
        for (int i = 0; i < 80; ++i) { consume(); CHECK(none_pressed()); }

        // The touchpad as Minus (the usual mapping): a tap presses it (11) on release, for about 100 ms...
        sample.buttons = kButtonTouchPad; consume(); CHECK(none_pressed());
        for (int i = 0; i < 20; ++i) { consume(); CHECK(none_pressed()); }
        sample.buttons = 0; consume();
        for (int i = 0; i < 24; ++i) { CHECK(pad.Engine().GetButton({}, 11)); consume(); }
        consume(); CHECK(none_pressed());
        // ...a long press holds it until the release...
        sample.buttons = kButtonTouchPad;
        for (int i = 0; i < 59; ++i) { consume(); CHECK(none_pressed()); }
        consume(); CHECK(pad.Engine().GetButton({}, 11));
        for (int i = 0; i < 30; ++i) { consume(); CHECK(pad.Engine().GetButton({}, 11)); }
        sample.buttons = 0; consume(); CHECK(none_pressed());
        for (int i = 0; i < 30; ++i) { consume(); CHECK(none_pressed()); }
        // ...a shortcut started slowly gives none, and a controller that goes away is not a tap.
        sample.buttons = kButtonTouchPad; consume(); consume();
        sample.buttons = kButtonTouchPad | kButtonR1; consume(); CHECK(pad.TakeHudToggle());
        sample.buttons = kButtonTouchPad; consume();
        sample.buttons = 0; consume(); CHECK(none_pressed());
        sample.buttons = kButtonTouchPad; consume();
        sample.connected = 0; consume(); CHECK(none_pressed());
        sample.connected = 1; sample.buttons = 0; consume(); CHECK(none_pressed());
        // Create is Minus directly, also while the touchpad is down.
        sample.buttons = kButtonCreate | kButtonTouchPad; consume(); CHECK(pad.Engine().GetButton({}, 11));
        sample.buttons = kButtonTouchPad; consume(); CHECK(none_pressed());
        sample.buttons = kButtonTouchPad | kButtonL1; consume(); CHECK(pad.TakeReturnToMenu());
        sample.buttons = 0; consume(); CHECK(none_pressed());

        // A button mapping (button_mapping.h): A on Cross and B on Circle, ZL on the touchpad
        // (tap and hold as for Minus) and Minus on L2 with its analog trigger; Create, which has
        // no game button of its own, presses the touchpad's.
        auto custom = Eden::Assign(Eden::kDefaultMapping, Eden::game_a, Eden::pad_cross);
        custom = Eden::Assign(custom, Eden::game_zl, Eden::pad_touchpad);
        CHECK(custom[Eden::game_minus] == Eden::pad_l2 && custom[Eden::game_b] == Eden::pad_circle);
        pad.SetMapping(custom);
        sample.buttons = kButtonCross; consume();
        CHECK(pad.Engine().GetButton({}, 0)); CHECK(!pad.Engine().GetButton({}, 1));
        sample.buttons = kButtonCircle; consume();
        CHECK(pad.Engine().GetButton({}, 1)); CHECK(!pad.Engine().GetButton({}, 0));
        sample.buttons = 0; sample.triggers = {200, 0}; consume();
        CHECK(pad.Engine().GetButton({}, 11)); CHECK(!pad.Engine().GetButton({}, 8));
        sample.triggers = {};
        sample.buttons = kButtonTouchPad; consume(); CHECK(none_pressed());
        sample.buttons = 0; consume(); CHECK(pad.Engine().GetButton({}, 8)); CHECK(!pad.Engine().GetButton({}, 11));
        for (int i = 0; i < 30; ++i) consume();
        CHECK(none_pressed());
        sample.buttons = kButtonCreate; consume(); CHECK(pad.Engine().GetButton({}, 8));
        sample.buttons = kButtonTouchPad | kButtonL1; consume(); CHECK(pad.TakeReturnToMenu());
        sample.buttons = 0; consume(); CHECK(none_pressed());
        // A mapping that names a button twice is not taken: the usual one stays.
        auto twice = Eden::kDefaultMapping;
        twice[Eden::game_x] = twice[Eden::game_y];
        pad.SetMapping(twice);
        sample.buttons = kButtonTriangle; consume(); CHECK(pad.Engine().GetButton({}, 2));
        sample.buttons = 0; consume();
        pad.SetMapping(Eden::kDefaultMapping);

        // Motion: Eden's SDL mapping of a DualSense (G, turns per second, microsecond deltas).
        constexpr float pi = std::numbers::pi_v<float>;
        sample.acceleration = {0.25f, 1.0f, -0.5f};
        sample.angular_velocity = {pi, 0.0f, -2.0f * pi};
        auto motion_device = Common::Input::CreateInputDeviceFromString("engine:virtual_gamepad,port:0,motion:0");
        int motion_updates = 0;
        motion_device->SetCallback({[&](const auto&) { ++motion_updates; }});
        sample.timestamp_us = 1000; consume(); // The first sample only starts the clock.
        CHECK(motion_updates == 0);
        sample.timestamp_us = 5000; consume();
        CHECK(motion_updates == 1);
        auto motion = pad.Engine().GetMotion({}, 0);
        CHECK(motion.delta_timestamp == 4000);
        CHECK(motion.accel_x == -0.25f); CHECK(motion.accel_y == -0.5f); CHECK(motion.accel_z == -1.0f);
        CHECK(motion.gyro_x == 0.5f); CHECK(motion.gyro_y == 1.0f); CHECK(motion.gyro_z == 0.0f);
        consume(); CHECK(motion_updates == 1); // A repeated sample is not new motion.
        sample.connected = 0; consume(); // Away: at rest instead of turning on.
        CHECK(motion_updates == 2);
        motion = pad.Engine().GetMotion({}, 0);
        CHECK(motion.gyro_x == 0.0f); CHECK(motion.gyro_y == 0.0f); CHECK(motion.accel_z == -1.0f);
        sample.connected = 1; sample.acceleration = {}; sample.angular_velocity = {}; sample.timestamp_us = 0;

        // A single Joy-Con held sideways, on a DualSense held as usual: the places of the arrows
        // and shapes, either stick and the motion are turned by a quarter (left Joy-Con: top to
        // the left; right Joy-Con: top to the right).
        {
            using Grip = Eden::Pad::Grip;
            const auto pressed = [&](ButtonMask mask, std::initializer_list<int> buttons) {
                sample.buttons = mask; consume();
                for (const int button : {0, 1, 2, 3, 12, 13, 14, 15})
                    if (pad.Engine().GetButton({}, button) !=
                        (std::find(buttons.begin(), buttons.end(), button) != buttons.end())) return false;
                return true;
            };
            pad.SetGrip(0, Grip::sideways_left);
            // Top is the Joy-Con's Right (14), right its Down (15), bottom its Left (12), left its
            // Up (13); the shapes still press the game's own buttons, which a left Joy-Con has not.
            CHECK(pressed(kButtonUp, {14})); CHECK(pressed(kButtonRight, {15}));
            CHECK(pressed(kButtonDown, {12})); CHECK(pressed(kButtonLeft, {13}));
            CHECK(pressed(kButtonTriangle, {14, 2})); CHECK(pressed(kButtonCircle, {15, 0}));
            CHECK(pressed(kButtonCross, {12, 1})); CHECK(pressed(kButtonSquare, {13, 3}));
            sample.buttons = 0;
            sample.left_stick = {128, 0}; sample.right_stick = {128, 128}; consume(); // up
            CHECK(pad.Engine().GetAxis({}, 0) == 1); CHECK(pad.Engine().GetAxis({}, 1) == 0);
            sample.left_stick = {128, 128}; sample.right_stick = {255, 128}; consume(); // right, other stick
            CHECK(pad.Engine().GetAxis({}, 0) == 0); CHECK(pad.Engine().GetAxis({}, 1) == -1);
            sample.right_stick = {128, 128};
            sample.acceleration = {0.25f, 1.0f, -0.5f};
            sample.angular_velocity = {pi, 0.0f, -2.0f * pi};
            sample.timestamp_us = 1000; consume(); sample.timestamp_us = 5000; consume();
            motion = pad.Engine().GetMotion({}, 0);
            CHECK(motion.accel_x == -0.5f); CHECK(motion.accel_y == 0.25f); CHECK(motion.accel_z == -1.0f);
            CHECK(motion.gyro_x == 1.0f); CHECK(motion.gyro_y == -0.5f); CHECK(motion.gyro_z == 0.0f);

            pad.SetGrip(0, Grip::sideways_right);
            // Top is Y (3), right X (2), bottom A (0), left B (1); the arrows also stay the arrows,
            // which a right Joy-Con has not.
            CHECK(pressed(kButtonTriangle, {3})); CHECK(pressed(kButtonCircle, {2}));
            CHECK(pressed(kButtonCross, {0})); CHECK(pressed(kButtonSquare, {1}));
            CHECK(pressed(kButtonUp, {3, 13})); CHECK(pressed(kButtonRight, {2, 14}));
            CHECK(pressed(kButtonDown, {0, 15})); CHECK(pressed(kButtonLeft, {1, 12}));
            sample.buttons = 0;
            sample.left_stick = {128, 0}; consume(); // up
            CHECK(pad.Engine().GetAxis({}, 2) == -1); CHECK(pad.Engine().GetAxis({}, 3) == 0);
            sample.left_stick = {255, 128}; consume(); // right
            CHECK(pad.Engine().GetAxis({}, 2) == 0); CHECK(pad.Engine().GetAxis({}, 3) == 1);
            sample.left_stick = {128, 128};
            sample.timestamp_us = 9000; consume();
            motion = pad.Engine().GetMotion({}, 0);
            CHECK(motion.accel_x == 0.5f); CHECK(motion.accel_y == -0.25f); CHECK(motion.accel_z == -1.0f);
            CHECK(motion.gyro_x == -1.0f); CHECK(motion.gyro_y == 0.5f); CHECK(motion.gyro_z == 0.0f);

            pad.SetGrip(0, Grip::usual);
            sample.acceleration = {}; sample.angular_velocity = {}; sample.timestamp_us = 0;
            sample.buttons = 0; sample.connected = 0; consume(); sample.connected = 1; consume();
            std::puts("Single Joy-Con held sideways: button places, either stick and motion turned for both sides PASS");

            // A single Joy-Con held upright: nothing is turned, and the whole DualSense is that
            // Joy-Con. A left one's four buttons are its arrows (13 Up, 14 Right, 15 Down,
            // 12 Left), pressed by the shapes at the same places; the shapes still press A, B,
            // X and Y, which a left Joy-Con has not. The DualSense's arrows press none of them:
            // they move the stick.
            const auto near = [](float value, float wanted) { return std::abs(value - wanted) < 0.001f; };
            const auto tilted = [&](ButtonMask mask, int axis_x, float x, float y) {
                sample.buttons = mask; consume();
                return near(pad.Engine().GetAxis({}, axis_x), x) && near(pad.Engine().GetAxis({}, axis_x + 1), y);
            };
            const auto others = [&](ButtonMask mask, int button) {
                sample.buttons = mask; consume();
                return pad.Engine().GetButton({}, button);
            };
            pad.SetGrip(0, Grip::upright_left);
            CHECK(pressed(kButtonUp, {})); CHECK(pressed(kButtonRight, {}));
            CHECK(pressed(kButtonDown, {})); CHECK(pressed(kButtonLeft, {}));
            CHECK(pressed(kButtonTriangle, {13, 2})); CHECK(pressed(kButtonCircle, {14, 0}));
            CHECK(pressed(kButtonCross, {15, 1})); CHECK(pressed(kButtonSquare, {12, 3}));
            CHECK(tilted(kButtonUp, 0, 0, 1)); CHECK(tilted(kButtonRight, 0, 1, 0));
            CHECK(tilted(kButtonDown, 0, 0, -1)); CHECK(tilted(kButtonLeft, 0, -1, 0));
            CHECK(tilted(kButtonUp | kButtonRight, 0, 0.7071f, 0.7071f)); CHECK(tilted(0, 0, 0, 0));
            // Its L (6), ZL (8), Minus (11) and stick click (4) from either side of the DualSense.
            CHECK(others(kButtonR1, 6)); CHECK(others(kButtonL1, 6)); CHECK(others(kButtonOptions, 11));
            CHECK(others(kButtonR3, 4)); CHECK(others(kButtonL3, 4)); CHECK(!others(0, 6)); CHECK(!others(0, 11));
            sample.buttons = 0; sample.triggers.r2 = 255; consume();
            CHECK(pad.Engine().GetButton({}, 8));
            sample.triggers.r2 = 0; consume();
            CHECK(!pad.Engine().GetButton({}, 8));
            sample.left_stick = {128, 128}; sample.right_stick = {255, 128}; consume(); // right, other stick
            CHECK(pad.Engine().GetAxis({}, 0) == 1); CHECK(pad.Engine().GetAxis({}, 1) == 0);
            sample.left_stick = {128, 0}; sample.right_stick = {128, 128}; consume(); // up
            CHECK(pad.Engine().GetAxis({}, 0) == 0); CHECK(pad.Engine().GetAxis({}, 1) == 1);
            sample.left_stick = {128, 128};

            // A right one's four buttons are A (0), B (1), X (2) and Y (3), pressed by the
            // shapes and by the arrows at the same places; its stick is the right one.
            pad.SetGrip(0, Grip::upright_right);
            CHECK(pressed(kButtonTriangle, {2})); CHECK(pressed(kButtonCircle, {0}));
            CHECK(pressed(kButtonCross, {1})); CHECK(pressed(kButtonSquare, {3}));
            // The arrows stay the arrows, which a right Joy-Con has not, and move its stick.
            CHECK(pressed(kButtonUp, {13})); CHECK(pressed(kButtonRight, {14}));
            CHECK(pressed(kButtonDown, {15})); CHECK(pressed(kButtonLeft, {12}));
            CHECK(tilted(kButtonUp, 2, 0, 1)); CHECK(tilted(kButtonLeft, 2, -1, 0));
            CHECK(tilted(kButtonDown | kButtonLeft, 2, -0.7071f, -0.7071f)); CHECK(tilted(0, 2, 0, 0));
            // Its R (7), ZR (9), Plus (10) and stick click (5).
            CHECK(others(kButtonL1, 7)); CHECK(others(kButtonR1, 7)); CHECK(others(kButtonCreate, 10));
            CHECK(others(kButtonOptions, 10)); CHECK(others(kButtonL3, 5)); CHECK(!others(0, 7)); CHECK(!others(0, 10));
            sample.buttons = 0; sample.triggers.l2 = 255; consume();
            CHECK(pad.Engine().GetButton({}, 9));
            sample.triggers.l2 = 0; consume();
            sample.left_stick = {128, 0}; consume(); // up, on the stick a right Joy-Con has not
            CHECK(pad.Engine().GetAxis({}, 2) == 0); CHECK(pad.Engine().GetAxis({}, 3) == 1);
            sample.left_stick = {255, 128}; consume(); // right
            CHECK(pad.Engine().GetAxis({}, 2) == 1); CHECK(pad.Engine().GetAxis({}, 3) == 0);
            sample.left_stick = {128, 128};

            pad.SetGrip(0, Grip::usual);
            sample.buttons = 0; sample.connected = 0; consume(); sample.connected = 1; consume();
            std::puts("Single Joy-Con held upright: its four buttons from the shapes, its stick from the arrows and either stick, shoulder buttons PASS");
        }

        // Rumble: both guest sides on one DualSense; low band -> large motor, high band -> small.
        {
            using Common::Input::DriverResult;
            using Type = Common::Input::VibrationAmplificationType;
            auto left = Common::Input::CreateOutputDeviceFromString("engine:virtual_gamepad,port:0,pad:1");
            auto right = Common::Input::CreateOutputDeviceFromString("engine:virtual_gamepad,port:0,pad:2");
            auto handheld = Common::Input::CreateOutputDeviceFromString("engine:virtual_gamepad,port:8,pad:1");
            CHECK(left->IsVibrationEnabled());
            state.vibrations.clear();
            CHECK(pad.Poll()); CHECK(state.vibrations.empty());
            CHECK(left->SetVibration({.low_amplitude = 1.0f, .type = Type::Exponential}) == DriverResult::Success);
            CHECK(right->SetVibration({.high_amplitude = 0.5f, .type = Type::Linear}) == DriverResult::Success);
            CHECK(pad.Poll()); CHECK(state.vibrations.size() == 1);
            CHECK(state.vibrations[0].large_motor == 255); CHECK(state.vibrations[0].small_motor == 154);
            CHECK(pad.Poll()); CHECK(state.vibrations.size() == 1); // Unchanged levels are not resent.
            CHECK(left->SetVibration({}) == DriverResult::Success);
            CHECK(right->SetVibration({}) == DriverResult::Success);
            CHECK(pad.Poll()); CHECK(state.vibrations.size() == 2);
            CHECK(state.vibrations[1].large_motor == 0); CHECK(state.vibrations[1].small_motor == 0);
            CHECK(handheld->SetVibration({.high_amplitude = 1.0f, .type = Type::Exponential}) == DriverResult::Success);
            CHECK(pad.Poll()); CHECK(state.vibrations.size() == 3); CHECK(state.vibrations[2].small_motor == 255);
        }

        auto button = Common::Input::CreateInputDeviceFromString("engine:virtual_gamepad,port:0,button:0");
        std::vector<bool> changes;
        button->SetCallback({[&](const auto& status) { changes.push_back(status.button_status.value); }});
        for (int i = 0; i < 64; ++i) {
            sample.buttons = i % 2 == 0 ? kButtonCircle : 0;
            state.pad_samples.push_back(sample);
        }
        CHECK(pad.Poll()); CHECK(changes.size() == 64);
        for (int i = 0; i < 64; ++i) CHECK(changes[i] == (i % 2 == 0));
        sample.buttons = kButtonCircle; consume(); CHECK(pad.Poll());
        CHECK(pad.Engine().GetButton({}, 0)); // Empty reads preserve the last sample.
        sample.connected = 0; consume(); CHECK(!pad.Engine().GetButton({}, 0));
        sample.connected = 1; consume(); CHECK(pad.Engine().GetButton({}, 0));
        sample.buttons |= kButtonIntercepted; consume(); CHECK(!pad.Engine().GetButton({}, 0));
        sample.buttons = kButtonCircle;
        for (int error : {-1, 65}) {
            consume(); state.read_result = error; CHECK(!pad.Poll());
            CHECK(!pad.Engine().GetButton({}, 0)); CHECK(!pad.Engine().GetButton({}, 9));
        }
        state.read_result = 0; consume(); pad.Close(); pad.Close();
        CHECK(!pad.Engine().GetButton({}, 9));
        CHECK(state.vibrations.size() == 4); // Closing stops the motors.
        CHECK(state.vibrations[3].large_motor == 0); CHECK(state.vibrations[3].small_motor == 0);
        CHECK(state.pad_closes == 1); CHECK(state.user_terminations == 1);
    }
    state.user_init_result = -1; // A service owned by the embedding application.
    { Eden::Pad pad; CHECK(pad.Open()); }
    CHECK(state.pad_closes == 2); CHECK(state.user_terminations == 1);
    state.user_init_result = 0; state.user_result = -1;
    { Eden::Pad pad; CHECK(!pad.Open()); }
    CHECK(state.user_terminations == 2);
    state.user_result = 0; state.pad_open_result = -1;
    { Eden::Pad pad; CHECK(!pad.Open()); }
    CHECK(state.user_terminations == 3); CHECK(state.pad_closes == 2);
    state.pad_open_result = 7;
    std::puts("Pad mappings, a changed button mapping, calibration, motion, rumble, 64-sample edges, disconnect and ownership PASS");
}

void CheckAudio() {
    Core::System system;
    Settings::values.volume = 100;
    auto selected_sink = CreateSinkFromID(Settings::AudioEngine::Null, "ps5");
    auto& sink = *selected_sink;
    Reject([&] { sink.AcquireSinkStream(system, 3, "invalid", StreamType::Out); });
    state.fail_audio_open = true;
    Reject([&] { sink.AcquireSinkStream(system, 2, "open failure", StreamType::Out); });
    state.fail_audio_open = false; state.fail_volume = true;
    Reject([&] { sink.AcquireSinkStream(system, 2, "volume failure", StreamType::Out); });
    state.fail_volume = false; CHECK(state.audio_closes == state.audio_opens);
    for (u32 channels : {1u, 2u, 6u}) {
        sink.SetDeviceVolume(0.5f); sink.SetSystemVolume(1);
        auto* stream = sink.AcquireSinkStream(system, channels, "PCM check", StreamType::Out);
        const int handle = 100 + state.audio_opens;
        CHECK(stream->IsPaused()); CHECK(stream->GetDeviceChannels() == 2);
        std::vector<s16> pcm(256 * channels, 1000);
        SinkBuffer buffer{256, 0, channels, false};
        SinkBuffer invalid{257, 0, 0, false};
        Reject([&] { stream->AppendBuffer(invalid, pcm); });
        stream->AppendBuffer(buffer, pcm);
        CHECK(stream->GetQueueSize() == 1);
        stream->Start();
        {
            std::unique_lock lock(state.mutex);
            CHECK(state.wake.wait_for(lock, 2s, [&] { return state.audio[handle].size() >= 2; }));
        }
        stream->Stop(); CHECK(stream->IsPaused()); CHECK(stream->GetQueueSize() == 0);
        {
            std::scoped_lock lock(state.mutex);
            const s16 expected = channels == 6 ? 1328 : 500;
            for (auto value : state.audio[handle][0]) CHECK(std::abs(value - expected) <= 1);
            // Existing Eden underrun policy repeats the last complete frame.
            CHECK(state.audio[handle][1] == state.audio[handle][0]);
        }
        pcm.assign(256 * channels, 2000); buffer.tag += 10;
        stream->AppendBuffer(buffer, pcm); CHECK(stream->GetQueueSize() == 1);
        std::size_t count;
        { std::scoped_lock lock(state.mutex); count = state.audio[handle].size(); }
        stream->Start(true);
        {
            std::unique_lock lock(state.mutex);
            CHECK(state.wake.wait_for(lock, 2s, [&] { return state.audio[handle].size() > count; }));
        }
        stream->Stop(); CHECK(stream->GetQueueSize() == 0);
        stream->Finalize(); stream->Finalize();
        Reject([&] { stream->AppendBuffer(buffer, pcm); });
        sink.CloseStream(stream);
        CHECK(state.audio_closes == state.audio_opens);
    }
    {
        auto* stream = sink.AcquireSinkStream(system, 2, "queue capacity", StreamType::Out);
        const int handle = 100 + state.audio_opens;
        sink.SetDeviceVolume(1);
        for (int i = 1; i <= 4; ++i) {
            std::vector<s16> pcm(512, i * 1000);
            SinkBuffer buffer{256, 0, static_cast<u64>(i), false};
            stream->AppendBuffer(buffer, pcm);
        }
        CHECK(stream->GetQueueSize() == 4); stream->Start();
        {
            std::unique_lock lock(state.mutex);
            CHECK(state.wake.wait_for(lock, 2s, [&] { return state.audio[handle].size() >= 4; }));
        }
        stream->Stop();
        for (int i = 0; i < 4; ++i)
            for (auto value : state.audio[handle][i]) CHECK(value == (i + 1) * 1000);
        stream->ClearQueue();
        std::vector<s16> full(65536, 1234); SinkBuffer large{32768, 0, 10, false};
        stream->AppendBuffer(large, full);
        std::array<s16, 2> extra{}; SinkBuffer one{1, 0, 11, false};
        Reject([&] { stream->AppendBuffer(one, extra); });
        CHECK(stream->GetQueueSize() == 1); stream->ClearQueue();
        CHECK(stream->GetQueueSize() == 0); sink.CloseStream(stream);
    }
    {
        auto* stream = sink.AcquireSinkStream(system, 2, "partial block tail", StreamType::Out);
        const int handle = 100 + state.audio_opens;
        std::vector<s16> pcm(257 * 2, 1000);
        pcm[512] = pcm[513] = 0;
        SinkBuffer buffer{257, 0, 98, false};
        stream->AppendBuffer(buffer, pcm); stream->Start();
        {
            std::unique_lock lock(state.mutex);
            CHECK(state.wake.wait_for(lock, 2s, [&] { return state.audio[handle].size() >= 3; }));
        }
        stream->Stop();
        for (auto value : state.audio[handle][0]) CHECK(value == 1000);
        for (int block : {1, 2})
            for (auto value : state.audio[handle][block]) CHECK(value == 0);
        sink.CloseStream(stream);
    }
    auto* stream = static_cast<Eden::AudioStream*>(sink.AcquireSinkStream(system, 2, "failure", StreamType::Out));
    { std::scoped_lock lock(state.mutex); state.fail_output = true; }
    stream->Start();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (stream->Healthy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(!stream->Healthy()); stream->Stop();
    std::array<s16, 512> pcm{}; SinkBuffer buffer{256, 0, 99, false};
    Reject([&] { stream->AppendBuffer(buffer, pcm); });
    sink.CloseStreams(); state.fail_output = false;
    const int opened = state.audio_opens;
    auto* input = sink.AcquireSinkStream(system, 2, "input null", StreamType::In);
    const auto released = input->ReleaseBuffer(0xf00 / sizeof(s16));
    CHECK(released.size() == 0xf00 / sizeof(s16));
    CHECK(std::ranges::all_of(released, [](s16 sample) { return sample == 0; }));
    sink.CloseStreams(); CHECK(state.audio_opens == opened);
    CHECK(state.audio_opens == state.audio_closes); CHECK(state.audio_drains == 6);
    std::puts("Audio PCM mono/stereo/6-channel, volume, queue, underrun, pause/resume and failure cleanup PASS");
}
void CheckVoiceFlags() {
    VoiceInfo voice;
    VoiceInfo::InParameter parameter{};
    PoolMapper mapper(nullptr, false);
    BehaviorInfo behavior;
    BehaviorInfo::ErrorInfo error{};
    behavior.SetUserLibRevision(0x35564552);
    for (unsigned mask : {0, 1, 2, 3, 0}) {
        parameter.flags.IsVoicePlayedSampleCountResetAtLoopPointSupported = mask & 1;
        parameter.flags.IsVoicePitchAndSrcSkippedSupported = (mask >> 1) & 1;
        voice.UpdateParameters(error, parameter, mapper, behavior);
        CHECK(error.error_code.IsSuccess()); CHECK(voice.flags == mask);
    }
    behavior.SetUserLibRevision(0x34564552);
    parameter.flags = {1, 1};
    voice.UpdateParameters(error, parameter, mapper, behavior);
    CHECK(voice.flags == 0);
    std::vector<std::int16_t> pcm(960);
    for (unsigned i = 0; i < 480; ++i) {
        const auto sign = i % 96 < 48 ? 1 : -1;
        const auto second = i % 48 < 24 ? 1 : -1;
        pcm[i * 2] = sign * 512 + second * 128;
        pcm[i * 2 + 1] = sign * 256 + second * 256;
    }
    CHECK(Eden::Mock::RendererPcmMatches(pcm));
    CHECK(!Eden::Mock::RendererPcmMatches(pcm, true)); // High filter must not silently use Low.
    auto high = Eden::Mock::HighRendererReference();
    CHECK(Eden::Mock::RendererPcmMatches(high, true));
    CHECK(!Eden::Mock::RendererPcmMatches(high));
    high[100] += 1; CHECK(!Eden::Mock::RendererPcmMatches(high, true));
    high = Eden::Mock::HighRendererReference();
    for (unsigned i = 0; i < 480; ++i) std::swap(high[2*i], high[2*i+1]);
    CHECK(!Eden::Mock::RendererPcmMatches(high, true));
    high.fill(0); CHECK(!Eden::Mock::RendererPcmMatches(high, true));
    const auto valid = pcm;
    for (unsigned i = 0; i < 480; ++i) std::swap(pcm[i * 2], pcm[i * 2 + 1]);
    CHECK(!Eden::Mock::RendererPcmMatches(pcm));
    pcm = valid; pcm[100] = 0; CHECK(!Eden::Mock::RendererPcmMatches(pcm));
    for (unsigned missing = 0; missing < 2; ++missing) {
        for (unsigned i = 0; i < 480; ++i) {
            const auto sign = i % (missing ? 96 : 48) < (missing ? 48 : 24) ? 1 : -1;
            pcm[i * 2] = sign * (missing ? 512 : 128);
            pcm[i * 2 + 1] = sign * 256;
        }
        CHECK(!Eden::Mock::RendererPcmMatches(pcm));
    }
    for (unsigned i = 0; i < 480; ++i) {
        const auto first = i % 48 < 24 ? 1 : -1;
        const auto second = i % 24 < 12 ? 1 : -1;
        pcm[i * 2] = first * 512 + second * 128;
        pcm[i * 2 + 1] = first * 256 + second * 256;
    }
    CHECK(!Eden::Mock::RendererPcmMatches(pcm)); // Bypassing SRC doubles both frequencies.
    pcm.assign(960, 0); CHECK(!Eden::Mock::RendererPcmMatches(pcm));
    CHECK(!Eden::Mock::RendererPcmMatches({}));
    std::puts("Two-voice PCM window rejects missing voices, silence, swapped gains and corruption PASS");
    std::puts("Renderer voice flags remain independent and revision-gated PASS");
}
void CheckResampling() {
    using Fixed = Common::FixedPoint<49, 15>;
    constexpr unsigned count = 2400, split = 137;
    constexpr s32 sentinel = 123456789;
    for (auto quality : {AudioCore::SrcQuality::Low, AudioCore::SrcQuality::Medium,
                         AudioCore::SrcQuality::High}) {
        for (unsigned rate : {24000, 32000, 44100, 48000, 57600, 96000}) {
            const std::int64_t step = std::int64_t(rate) * 32768 / 48000;
            const auto ratio = Fixed::from_base(step);
            for (bool tone : {false, true}) {
                std::vector<s16> input(8192);
                if (tone) for (unsigned i = 0; i < input.size(); ++i)
                    input[i] = static_cast<s16>(std::lround(10000 * std::sin(
                        2 * std::numbers::pi * 500 * i / rate)));
                std::vector<s32> whole(count + 2, sentinel), chunks(count + 2, sentinel);
                auto fraction = Fixed::from_base(8192); // Start at one quarter sample.
                Resample(std::span{whole}.subspan(1, count), input, ratio, fraction, count, quality);
                CHECK(whole.front() == sentinel && whole.back() == sentinel);
                CHECK(fraction.get_frac() == (8192 + step * count) % 32768);
                auto partial = Fixed::from_base(8192);
                Resample(std::span{chunks}.subspan(1, split), input, ratio, partial, split, quality);
                const auto consumed = (8192 + step * split) / 32768;
                Resample(std::span{chunks}.subspan(1 + split, count - split),
                         std::span<const s16>{input}.subspan(consumed), ratio, partial,
                         count - split, quality);
                CHECK(chunks == whole && partial == fraction);
                // An empty output request must preserve phase and surrounding samples.
                Resample(std::span{chunks}.subspan(1, 0), input, ratio, partial, 0, quality);
                CHECK(chunks == whole && partial == fraction);
                if (!tone) {
                    CHECK(std::all_of(whole.begin() + 1, whole.end() - 1,
                                      [](auto value) { return value == 0; }));
                    continue;
                }
                if (quality == AudioCore::SrcQuality::Low) {
                    for (unsigned i = 0; i < count; ++i)
                        CHECK(whole[i + 1] == input[(8192 + step * i + 16384) / 32768]);
                }
                unsigned crossings = 0;
                double energy = 0;
                for (unsigned i = 1; i <= count; ++i) {
                    energy += double(whole[i]) * whole[i];
                    if (i > 1 && (whole[i] >= 0) != (whole[i - 1] >= 0)) ++crossings;
                }
                // 50 ms of a 500 Hz tone: approximately 25 cycles, unity RMS gain.
                CHECK(crossings >= 49 && crossings <= 51);
                CHECK(energy / count >= 45000000 && energy / count <= 55000000);
            }
        }
    }
    std::puts("SRC 3 qualities x 6 rates: silence, 500 Hz level/frequency, exact low-quality samples, split continuity and phase PASS");
}
int main() {
    Common::FS::CreateEdenPaths(); Common::Log::Initialize(); Common::Log::Start();
    int status = 0;
    try { CheckResampling(); CheckVoiceFlags(); CheckControllerStyle(); CheckKeyboard(); CheckPad(); CheckAudio(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); status = 1; }
    Common::Log::Stop();
    return status;
}
