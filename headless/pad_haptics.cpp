// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_haptics.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "audio_out_init.h"
#include "native_audio.hpp"
#include "ps5_pad.hpp"

namespace Eden {
namespace {
// The pad's vibration audio port: PCM written here reaches the haptic actuators
// rather than the compatible-mode motor emulation
constexpr int kHapticPortType = 10;
// sceAudioOut format low byte: 1 is S16 stereo, 4 is F32 stereo.
constexpr std::uint32_t kHapticFormatF32Stereo = 4;
constexpr int kOutputFailuresToFallback = 40;
constexpr auto kRetryInterval = std::chrono::seconds(2);
// The synthesis follows per-band sine plus low-passed decorrelated noise, 
// envelopes so level steps do not click, and the same headroom below full scale.
constexpr float kEnvelopeRate = 0.02f;
constexpr float kNoiseLowPass = 0.08f;
constexpr float kOutputHeadroom = 26000.0f;
constexpr float kDefaultLargeHz = 55.0f;
constexpr float kDefaultSmallHz = 190.0f;

float SanitizeAmplitude(float amplitude) {
    if (!std::isfinite(amplitude)) return 0.0f;
    return std::clamp(amplitude, 0.0f, 1.0f);
}

float SanitizeHz(float hz, float fallback_hz) {
    if (!std::isfinite(hz) || hz <= 0.0f) return fallback_hz;
    return std::clamp(hz, 20.0f, 1200.0f);
}

float HapticNoise(std::uint32_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>((rng >> 9) & 0xffffu) / 32768.0f - 1.0f;
}

std::int16_t ToS16(float sample) {
    const float scaled = sample * kOutputHeadroom;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return static_cast<std::int16_t>(scaled);
}
} // namespace

void RenderHapticBlock(HapticVoice& voice, float large_amplitude, float large_hz,
                       float small_amplitude, float small_hz, std::uint32_t frames,
                       std::int16_t* s16_out, float* f32_out) {
    using ps5::native_audio::kAudioOutRate;
    large_amplitude = SanitizeAmplitude(large_amplitude);
    small_amplitude = SanitizeAmplitude(small_amplitude);
    large_hz = SanitizeHz(large_hz, kDefaultLargeHz);
    small_hz = SanitizeHz(small_hz, kDefaultSmallHz);
    for (std::uint32_t i = 0; i < frames; ++i) {
        voice.envelope_large += (large_amplitude - voice.envelope_large) * kEnvelopeRate;
        voice.envelope_small += (small_amplitude - voice.envelope_small) * kEnvelopeRate;
        const float random_left = HapticNoise(voice.rng);
        const float random_right = HapticNoise(voice.rng);
        voice.noise_left += kNoiseLowPass * (random_left - voice.noise_left);
        voice.noise_right += kNoiseLowPass * (random_right - voice.noise_right);
        voice.phase_large += large_hz / kAudioOutRate;
        voice.phase_small += small_hz / kAudioOutRate;
        if (voice.phase_large >= 1.0) voice.phase_large -= 1.0;
        if (voice.phase_small >= 1.0) voice.phase_small -= 1.0;
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        const auto large_sine = static_cast<float>(std::sin(kTwoPi * voice.phase_large));
        const auto small_sine = static_cast<float>(std::sin(kTwoPi * voice.phase_small));
        // Both actuators carry both bands; the decorrelated noise keeps textures from
        // reading as one monotone buzz.
        const float left = voice.envelope_large * (0.70f * large_sine + 0.50f * voice.noise_left) +
                           voice.envelope_small * (0.55f * small_sine + 0.45f * voice.noise_right);
        const float right = voice.envelope_large * (0.70f * large_sine + 0.50f * voice.noise_right) +
                            voice.envelope_small * (0.55f * small_sine + 0.45f * voice.noise_left);
        s16_out[2 * i] = ToS16(left);
        s16_out[2 * i + 1] = ToS16(right);
        if (f32_out) {
            f32_out[2 * i] = s16_out[2 * i] / 32768.0f;
            f32_out[2 * i + 1] = s16_out[2 * i + 1] / 32768.0f;
        }
    }
}

HapticStream::~HapticStream() {
    Stop();
}

HapticStream::Stats HapticStream::GetStats() const {
    std::scoped_lock lock(mutex_);
    Stats stats = stats_;
    stats.active = active_.load(std::memory_order_acquire);
    return stats;
}

int HapticStream::SelectCompatibleLocked() {
    if (compatible_selected_) return 0;
    const int result = scePadSetVibrationMode(pad_handle_, ps5::pad::kVibrationModeCompatible);
    compatible_selected_ = result == 0;
    stats_.vibration_mode = compatible_selected_ ? ps5::pad::kVibrationModeCompatible : -1;
    return result;
}

bool HapticStream::Ensure(int user_id, int pad_handle) {
    if (active_.load(std::memory_order_acquire)) return true;
    // Reap a worker that fell back by itself; no lock is held while joining it.
    if (worker_.joinable()) worker_.join();
    std::scoped_lock lock(mutex_);
    user_id_ = user_id;
    pad_handle_ = pad_handle;
    if (thread_start_failed_) {
        SelectCompatibleLocked();
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (attempted_ && now - last_attempt_ < kRetryInterval) {
        SelectCompatibleLocked();
        return false;
    }
    attempted_ = true;
    last_attempt_ = now;
    using ps5::native_audio::kAudioOutGrain, ps5::native_audio::kAudioOutRate,
        ps5::native_audio::kAudioOutStereoS16;
    if (!AudioOutReady()) {
        SelectCompatibleLocked();
        return false;
    }
    int port = sceAudioOutOpen(user_id, kHapticPortType, 0, kAudioOutGrain, kAudioOutRate,
                               kAudioOutStereoS16);
    bool float_format = false;
    if (port < 0) {
        port = sceAudioOutOpen(user_id, kHapticPortType, 0, kAudioOutGrain, kAudioOutRate,
                               kHapticFormatF32Stereo);
        float_format = port >= 0;
    }
    if (port < 0) {
        SelectCompatibleLocked();
        return false;
    }
    if (scePadSetVibrationMode(pad_handle, ps5::pad::kVibrationModeAdvanced) != 0) {
        sceAudioOutClose(port);
        SelectCompatibleLocked();
        return false;
    }
    port_.store(port, std::memory_order_release);
    float_format_.store(float_format, std::memory_order_release);
    compatible_selected_ = false;
    stats_.port = port;
    stats_.float_format = float_format;
    stats_.vibration_mode = ps5::pad::kVibrationModeAdvanced;
    // Publish the routing state under the lock; Submit and the worker read it there.
    active_.store(true, std::memory_order_release);
    try {
        worker_ = std::jthread([this](std::stop_token stop) { Worker(stop); });
    } catch (...) {
        active_.store(false, std::memory_order_release);
        port_.store(-1, std::memory_order_release);
        sceAudioOutClose(port);
        stats_.port = -1;
        thread_start_failed_ = true;
        ++stats_.fallbacks;
        SelectCompatibleLocked();
        return false;
    }
    return true;
}

void HapticStream::RetryTick() {
    if (active_.load(std::memory_order_acquire)) return;
    int user_id, pad_handle;
    {
        std::scoped_lock lock(mutex_);
        user_id = user_id_;
        pad_handle = pad_handle_;
    }
    if (pad_handle < 0) return;
    (void)Ensure(user_id, pad_handle);
}

int HapticStream::Submit(const HapticLevels& levels) {
    motor_large_.store(levels.large, std::memory_order_relaxed);
    motor_small_.store(levels.small, std::memory_order_relaxed);
    large_amplitude_.store(SanitizeAmplitude(levels.large_amplitude), std::memory_order_relaxed);
    small_amplitude_.store(SanitizeAmplitude(levels.small_amplitude), std::memory_order_relaxed);
    large_hz_.store(levels.large_hz, std::memory_order_relaxed);
    small_hz_.store(levels.small_hz, std::memory_order_relaxed);
    std::scoped_lock lock(mutex_);
    if (active_.load(std::memory_order_acquire)) {
        if (!stats_.saw_nonzero && (levels.large != 0 || levels.small != 0))
            stats_.saw_nonzero = true;
        return 0;
    }
    if (pad_handle_ < 0) return 0; // stopped: nowhere to route to
    const int mode = SelectCompatibleLocked();
    if (mode != 0) return mode;
    const ps5::pad::Vibration vibration{.large_motor = levels.large, .small_motor = levels.small};
    return scePadSetVibration(pad_handle_, &vibration);
}

void HapticStream::Stop() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    std::scoped_lock lock(mutex_);
    active_.store(false, std::memory_order_release);
    const int port = port_.exchange(-1, std::memory_order_acq_rel);
    if (port >= 0) {
        sceAudioOutOutput(port, nullptr);
        sceAudioOutClose(port);
        stats_.port = -1;
    }
    if (pad_handle_ >= 0) {
        (void)scePadSetVibrationMode(pad_handle_, ps5::pad::kVibrationModeCompatible);
        const ps5::pad::Vibration stop{};
        (void)scePadSetVibration(pad_handle_, &stop);
        stats_.vibration_mode = ps5::pad::kVibrationModeCompatible;
    }
    user_id_ = -1;
    pad_handle_ = -1;
    compatible_selected_ = false;
    large_amplitude_.store(0.0f, std::memory_order_relaxed);
    small_amplitude_.store(0.0f, std::memory_order_relaxed);
    motor_large_.store(0, std::memory_order_relaxed);
    motor_small_.store(0, std::memory_order_relaxed);
}

void HapticStream::Worker(std::stop_token stop) {
    using ps5::native_audio::kAudioOutGrain;
    alignas(16) std::int16_t pcm_s16[kAudioOutGrain * 2];
    alignas(16) float pcm_f32[kAudioOutGrain * 2];
    HapticVoice voice;
    int failures = 0;
    const int port = port_.load(std::memory_order_acquire);
    const bool float_format = float_format_.load(std::memory_order_acquire);
    while (!stop.stop_requested()) {
        const float large_amplitude = large_amplitude_.load(std::memory_order_relaxed);
        const float large_hz = large_hz_.load(std::memory_order_relaxed);
        const float small_amplitude = small_amplitude_.load(std::memory_order_relaxed);
        const float small_hz = small_hz_.load(std::memory_order_relaxed);
        RenderHapticBlock(voice, large_amplitude, large_hz, small_amplitude, small_hz,
                           kAudioOutGrain, pcm_s16, float_format ? pcm_f32 : nullptr);
        const void* samples = float_format ? static_cast<const void*>(pcm_f32)
                                           : static_cast<const void*>(pcm_s16);
        if (sceAudioOutOutput(port, samples) < 0) {
            std::scoped_lock lock(mutex_);
            ++stats_.output_errors;
            if (++failures >= kOutputFailuresToFallback) {
                FallbackFromWorker();
                return;
            }
        } else {
            failures = 0;
            std::scoped_lock lock(mutex_);
            ++stats_.outputs;
        }
        if (failures > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void HapticStream::FallbackFromWorker() {
    // Called with mutex_ held, from the worker only.
    const int failed_port = port_.exchange(-1, std::memory_order_acq_rel);
    int mode_result = -1;
    if (pad_handle_ >= 0) {
        mode_result = scePadSetVibrationMode(pad_handle_, ps5::pad::kVibrationModeCompatible);
        compatible_selected_ = mode_result == 0;
        if (compatible_selected_) {
            const ps5::pad::Vibration levels{.large_motor = MotorLarge(),
                                             .small_motor = MotorSmall()};
            (void)scePadSetVibration(pad_handle_, &levels);
        }
    }
    stats_.vibration_mode =
        compatible_selected_ ? ps5::pad::kVibrationModeCompatible : -1;
    stats_.port = -1;
    ++stats_.fallbacks;
    active_.store(false, std::memory_order_release);
    if (failed_port >= 0) sceAudioOutClose(failed_port);
}
} // namespace Eden
