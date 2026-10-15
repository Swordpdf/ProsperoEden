// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_devices.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <stdexcept>
#include "native_audio.hpp"
#include "audio_core/renderer/command/resample/resample.h"
#include "audio_core/renderer/command/mix/mix_ramp.h"
namespace Eden::Mock {
State state;
std::array<std::int16_t, 960> HighRendererReference() {
    using namespace AudioCore::Renderer;
    std::array<s32, 480> left{}, right{};
    for (unsigned voice = 0; voice < 2; ++voice) {
        std::array<s16, 256> input{};
        for (unsigned i = 0; i < input.size(); ++i)
            input[i] = voice == 0 ? (i % 48 < 24 ? 1024 : -1024) : (i % 24 < 12 ? 512 : -512);
        std::array<s32, 480> output{};
        Common::FixedPoint<49, 15> fraction{0};
        Resample(output, input, Common::FixedPoint<49, 15>{0.5}, fraction, 480, AudioCore::SrcQuality::High);
        ApplyMixRamp<15>(left, output, voice == 0 ? 0.5f : 0.25f, 0, 480);
        ApplyMixRamp<15>(right, output, voice == 0 ? 0.25f : 0.5f, 0, 480);
    }
    std::array<std::int16_t, 960> result{};
    for (unsigned i = 0; i < 480; ++i) { result[2*i] = left[i]; result[2*i+1] = right[i]; }
    return result;
}
bool RendererPcmMatches(std::span<const std::int16_t> pcm, bool high) {
    if (pcm.size() % 2) return false;
    for (std::size_t i = 0; i < pcm.size(); i += 2) {
        if (std::abs(pcm[i]) > (high ? 1024 : 640) || std::abs(pcm[i + 1]) > (high ? 1024 : 512)) return false;
    }
    std::array<std::int16_t, 960> expected{};
    for (unsigned frame = 0; frame < 480; ++frame) {
        const auto sign = frame % 96 < 48 ? 1 : -1;
        const auto second = frame % 48 < 24 ? 1 : -1;
        expected[frame * 2] = sign * 512 + second * 128;
        expected[frame * 2 + 1] = sign * 256 + second * 256;
    }
    if (high) expected = HighRendererReference();
    const auto found = std::search(pcm.begin(), pcm.end(), expected.begin(), expected.end());
    return found != pcm.end() && (found - pcm.begin()) % 2 == 0;
}
void BeginGuestCycle() {
    std::scoped_lock lock(state.mutex);
    state.audio.clear();
    state.audio_opens = state.audio_closes = state.audio_drains = 0;
    state.guest_input = true;
    state.guest_start = std::chrono::steady_clock::now();
}
void CheckGuestCycle() {
    std::scoped_lock lock(state.mutex);
    state.guest_input = false;
    if (!state.audio_opens || state.audio_opens != state.audio_closes || state.audio_drains != state.audio_closes)
        throw std::runtime_error("Guest audio ports were not balanced");
    unsigned matches = 0;
    unsigned renderer_matches = 0;
    for (const auto& [handle, blocks] : state.audio) {
        std::vector<std::int16_t> pcm;
        for (const auto& block : blocks) pcm.insert(pcm.end(), block.begin(), block.end());
        auto start = std::find_if(pcm.begin(), pcm.end(), [](auto value) { return value != 0; });
        if (start == pcm.end()) continue;
        if (*start != 1024) {
            if (!RendererPcmMatches(pcm, true)) throw std::runtime_error("Renderer PCM waveform or channel gains differ");
            ++renderer_matches;
            continue;
        }
        if (pcm.end() - start < 24000) throw std::runtime_error("Guest PCM tail missing");
        for (unsigned frame = 0; frame < 12000; ++frame) {
            const auto value = frame == 11999 ? 0 : frame % 96 < 48 ? 1024 : -1024;
            if (start[frame * 2] != value || start[frame * 2 + 1] != value)
                throw std::runtime_error("Guest PCM samples differ from AudioOut output");
        }
        if (!std::all_of(start + 24000, pcm.end(), [](auto value) { return value == 0; }))
            throw std::runtime_error("Unexpected PCM after guest tone");
        ++matches;
    }
    if (matches != 1) throw std::runtime_error("Expected one guest PCM output stream");
    if (renderer_matches != 1) throw std::runtime_error("Expected one rendered PCM voice stream");
    state.audio.clear();
}
}
using Eden::Mock::state;
extern "C" {
int sceUserServiceInitialize(void*) { return state.user_init_result; }
int sceUserServiceGetInitialUser(int* user) { *user = 42; return 0; }
int sceUserServiceGetForegroundUser(int* user) { *user = state.foreground_user; return state.user_result; }
int sceUserServiceTerminate() { ++state.user_terminations; return 0; }
// Only the foreground user is signed in on the host.
int sceUserServiceGetLoginUserIdList(std::int32_t* users) {
    users[0] = state.foreground_user;
    users[1] = users[2] = users[3] = -1;
    return state.user_result;
}
int scePadInit() { return 0; }
int scePadOpen(int user, int type, int index, const void*) {
    state.last_pad_user = user;
    if (user != state.foreground_user || type != 0 || index != 0) return -1;
    ++state.pad_opens;
    return state.pad_open_result;
}
int scePadGetHandle(int, int, int) { return -1; }
int scePadGetControllerInformation(int, ps5::pad::ControllerInformation* info) {
    *info = {}; info->touchpad.resolution_x = 1920; return 0;
}
int scePadClose(int) { ++state.pad_closes; return 0; }
int scePadSetVibrationMode(int handle, int mode) {
    std::scoped_lock lock(state.mutex);
    state.vibration_modes.emplace_back(handle, mode);
    return mode == 1 ? state.vibration_mode_1_result : 0;
}
int scePadSetMotionSensorState(int, bool) { return 0; }
int scePadSetVibration(int, const ps5::pad::Vibration* vibration) {
    std::scoped_lock lock(state.mutex);
    state.vibrations.push_back(*vibration);
    return 0;
}
int scePadRead(int, ps5::pad::Data* data, int capacity) {
    std::scoped_lock lock(state.mutex);
    if (state.read_result) return state.read_result;
    if (state.guest_input && capacity > 0) {
        auto sample = ps5::pad::neutral_data(); sample.connected = 1;
        const auto phase = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state.guest_start).count() % 1500;
        if (phase >= 500 && phase < 1000) {
            sample.buttons = ps5::pad::kButtonCircle;
            sample.triggers.l2 = 255;
            sample.left_stick.x = 255;
            sample.right_stick.y = 0;
        }
        data[0] = sample;
        return 1;
    }
    const auto count = std::min<std::size_t>(capacity, state.pad_samples.size());
    for (std::size_t i = 0; i < count; ++i) {
        data[i] = state.pad_samples.front();
        state.pad_samples.pop_front();
    }
    return static_cast<int>(count);
}
int sceAudioOutInit() { return 0; }
int sceAudioOutOpen(int user, int type, int index, std::uint32_t grain,
                    std::uint32_t rate, std::uint32_t format) {
    std::scoped_lock lock(state.mutex);
    if (type == 10) {
        if (state.fail_haptic_open || user < 0 || index != 0 || grain != 256 || rate != 48000 ||
            (format != 1 && format != 4) || static_cast<int>(format) == state.haptic_fail_format)
            return -1;
        const auto handle = 300 + ++state.haptic_opens;
        state.haptic_audio[handle];
        state.haptic_open_users.push_back(user);
        state.haptic_open_formats.push_back(static_cast<int>(format));
        return handle;
    }
    if (state.fail_audio_open || user != 0xff || type != 0 || index != 0 ||
        grain != 256 || rate != 48000 || format != 1) return -1;
    const auto handle = 100 + ++state.audio_opens;
    state.audio[handle];
    return handle;
}
int sceAudioOutSetVolume(int, int flags, const int* volume) {
    return state.fail_volume || flags != 3 || volume[0] != 0x8000 || volume[1] != 0x8000 ? -1 : 0;
}
int sceAudioOutOutput(int handle, const void* samples) {
    {
        std::scoped_lock lock(state.mutex);
        if (const auto haptic = state.haptic_audio.find(handle); haptic != state.haptic_audio.end()) {
            if (!samples) { ++state.haptic_drains; return 0; }
            if (state.fail_haptic_output) return -1;
            auto& blocks = haptic->second;
            if (blocks.size() < 64) {
                const std::size_t opened = static_cast<std::size_t>(handle - 300 - 1);
                const std::size_t bytes = opened < state.haptic_open_formats.size() &&
                                                  state.haptic_open_formats[opened] == 4
                                              ? 256 * 2 * sizeof(float)
                                              : 256 * 2 * sizeof(std::int16_t);
                const auto* first = static_cast<const std::uint8_t*>(samples);
                blocks.emplace_back(first, first + bytes);
            }
            state.wake.notify_all();
        } else {
            if (!samples) { ++state.audio_drains; return 0; }
            if (state.fail_output) return -1;
            // Bound capture memory during whole-core offline runs.
            auto& blocks = state.audio[handle];
            if (blocks.size() < 256) {
                auto& block = blocks.emplace_back();
                std::copy_n(static_cast<const std::int16_t*>(samples), block.size(), block.begin());
            }
            state.wake.notify_all();
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(5333));
    return 0;
}
int sceAudioOutClose(int handle) {
    std::chrono::milliseconds delay{0};
    {
        std::scoped_lock lock(state.mutex);
        if (state.haptic_audio.contains(handle)) delay = state.haptic_close_delay;
    }
    // Delay outside the lock so tests must wait for close completion.
    std::this_thread::sleep_for(delay);
    std::scoped_lock lock(state.mutex);
    if (state.haptic_audio.contains(handle)) {
        ++state.haptic_closes;
        state.wake.notify_all();
        return 0;
    }
    ++state.audio_closes;
    return 0;
}
}
