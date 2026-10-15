// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// DualSense HD rumble through audio haptics: one HapticStream per pad.
//
// This module depends only on the C++ standard library and the native ABI contracts
// (third_party/); in particular it never logs, so it also compiles into a standalone
// host harness. The caller (headless/pad.cpp) reports route changes from Stats.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace Eden {
// Amplitudes arrive with the perceptual curve already applied (the caller shares it
// with the motor bytes); the synthesis itself stays linear.
struct HapticLevels {
    std::uint8_t large = 0;
    std::uint8_t small = 0;
    float large_amplitude = 0.0f;
    float small_amplitude = 0.0f;
    float large_hz = 55.0f;
    float small_hz = 190.0f;
};

// Threaded through every block; rendering stays continuous across block boundaries.
struct HapticVoice {
    double phase_large = 0.0;
    double phase_small = 0.0;
    float envelope_large = 0.0f;
    float envelope_small = 0.0f;
    float noise_left = 0.0f;
    float noise_right = 0.0f;
    std::uint32_t rng = 0x9e3779b9u;
};

// Renders `frames` stereo frames at 48 kHz into `s16_out`; `f32_out` (nullable) gets
// S16/32768. Amplitudes clamp to 0..1 (non-finite to 0) and frequencies to 20..1200 Hz
// (bad values to 55/190 Hz): a zero frequency would freeze an oscillator into a DC
// offset on a voice coil.
void RenderHapticBlock(HapticVoice& voice, float large_amplitude, float large_hz,
                       float small_amplitude, float small_hz, std::uint32_t frames,
                       std::int16_t* s16_out, float* f32_out);

class HapticStream final {
public:
    HapticStream() = default;
    ~HapticStream();
    HapticStream(const HapticStream&) = delete;
    HapticStream& operator=(const HapticStream&) = delete;

    struct Stats {
        bool active = false;
        int port = -1;            // open vibration audio port, -1 when none
        bool float_format = false;
        int vibration_mode = -1;  // 1 advanced, 2 compatible, -1 neither selected yet
        std::uint64_t outputs = 0;
        std::uint64_t output_errors = 0;
        std::uint64_t fallbacks = 0;  // worker/thread failures that fell back to motors
        bool saw_nonzero = false;     // a nonzero request reached the PCM route
    };
    Stats GetStats() const;

    // Brings up the PCM route, else the motor path. Returns true while PCM is active.
    // Only the pad poll thread may call Ensure/RetryTick/Submit/Stop; the worker only
    // signals through atomics and the routing mutex.
    bool Ensure(int user_id, int pad_handle);
    // Retries a failed bring-up after a successful pad poll, at most every 2 s.
    void RetryTick();
    // Routes levels to the PCM stream when it is active, else to the motors. Returns
    // the motor-path result when that path carries the levels, 0 otherwise.
    int Submit(const HapticLevels& levels);
    // Leaves the pad on the compatible path with still motors, as the launcher's menu
    // pulse expects. Idempotent.
    void Stop();
    bool Active() const { return active_.load(std::memory_order_acquire); }

private:
    void Worker(std::stop_token stop);
    void FallbackFromWorker();
    // Compatible mode, selected once until the next advanced bring-up. Call with mutex_
    // held.
    int SelectCompatibleLocked();
    std::uint8_t MotorLarge() const { return motor_large_.load(std::memory_order_relaxed); }
    std::uint8_t MotorSmall() const { return motor_small_.load(std::memory_order_relaxed); }

    mutable std::mutex mutex_;
    // The worker reads these once per block.
    std::atomic<float> large_amplitude_{0.0f};
    std::atomic<float> small_amplitude_{0.0f};
    std::atomic<float> large_hz_{55.0f};
    std::atomic<float> small_hz_{190.0f};
    std::atomic<std::uint8_t> motor_large_{0};
    std::atomic<std::uint8_t> motor_small_{0};
    std::atomic<bool> active_{false};
    std::atomic<int> port_{-1};
    std::atomic<bool> float_format_{false};
    int user_id_ = -1;  // guarded by mutex_
    int pad_handle_ = -1;
    bool compatible_selected_ = false;
    bool thread_start_failed_ = false;
    bool attempted_ = false;
    std::chrono::steady_clock::time_point last_attempt_{};
    std::jthread worker_;
    Stats stats_;
};
} // namespace Eden
