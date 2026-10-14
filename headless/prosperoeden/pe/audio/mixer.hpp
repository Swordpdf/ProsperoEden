// ProsperoEden - Real-time stereo mixer (48 kHz, S16 output).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/audio/stream_ring.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pe::audio
{

constexpr int kSampleRate = 48000;

enum class Bus : std::uint8_t
{
    ui,
    sfx,
    music,
    count,
};

// Immutable PCM owned by the caller; must outlive every voice playing it.
struct Clip
{
    const float *samples = nullptr; // interleaved stereo, -1..1
    std::size_t frames = 0;
};

enum class Wave : std::uint8_t
{
    sine,
    triangle,
    soft_square,
};

// Synthesized placeholder sound: a pitch sweep with an attack/release envelope.
struct Tone
{
    Wave wave = Wave::sine;
    float start_hz = 440.0f;
    float end_hz = 440.0f;
    float seconds = 0.08f;
    float attack = 0.004f;
    float release = 0.03f;
};

struct PlayParams
{
    float gain = 1.0f;
    float pitch = 1.0f; // playback-rate multiplier for clips, frequency multiplier for tones
    float pan = 0.0f;   // -1 left .. 1 right
    Bus bus = Bus::sfx;
};

// The game thread posts commands; the audio thread renders. Posting never
// blocks or allocates; if the queue is full the command is dropped.
class Mixer
{
  public:
    static constexpr std::size_t kVoices = 32;
    static constexpr std::size_t kQueue = 256;
    static constexpr std::size_t kStreams = 2;     // music decks, for crossfades
    static constexpr std::size_t kMaxGrain = 1024; // frames per render call

    Mixer();

    // ---- game thread ----
    bool play_clip(const Clip *clip, const PlayParams &params);
    bool play_tone(const Tone &tone, const PlayParams &params);
    bool set_bus_gain(Bus bus, float gain);
    bool set_master_gain(float gain);
    bool stop_all();
    // Acknowledged on the audio thread after queued voices have relinquished their clips.
    std::uint64_t stop_all_fenced();
    std::uint64_t completed_fence() const { return fence_done_.load(std::memory_order_acquire); }
    // Attach before the audio thread starts; streams play on the music bus.
    void attach_stream(std::size_t slot, StreamRing *ring)
    {
        if (slot < kStreams)
            streams_[slot] = ring;
    }
    // Switches a slot to another ring on the audio thread; the caller keeps
    // the old ring alive for a few grains.
    bool swap_stream(std::size_t slot, StreamRing *ring);
    // Ramps a stream's gain to gain over seconds (0 = at once).
    bool set_stream_gain(std::size_t slot, float gain, float seconds);

    // ---- audio thread ----
    // Renders frames of interleaved stereo S16 at kSampleRate.
    void render(std::int16_t *out, int frames);
    int active_voices() const
    {
        return active_.load(std::memory_order_relaxed);
    }
    // Grains where an audible stream ran dry (the decoder fell behind).
    std::uint64_t stream_underruns() const
    {
        return underruns_.load(std::memory_order_relaxed);
    }
    std::uint64_t dropped_commands() const
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    // Soft limiter applied to the final mix: linear below the knee, never
    // louder than kCeiling (-1 dBFS).
    static constexpr float kKnee = 0.7f;
    static constexpr float kCeiling = 0.891f;
    static float limit(float sample);

  private:
    enum class CommandType : std::uint8_t
    {
        clip,
        tone,
        bus_gain,
        master_gain,
        stop_all,
        stream_gain,
        stream_swap,
    };
    struct Command
    {
        CommandType type = CommandType::stop_all;
        const Clip *clip = nullptr;
        Tone tone{};
        PlayParams params{};
        float value = 0.0f;
        float seconds = 0.0f;
        std::size_t slot = 0;
        StreamRing *ring = nullptr;
        std::uint64_t fence = 0;
    };
    struct Voice
    {
        bool active = false;
        bool is_tone = false;
        const Clip *clip = nullptr;
        double position = 0.0; // frames (clips) or seconds (tones)
        double phase = 0.0;
        Tone tone{};
        PlayParams params{};
        float left = 1.0f;
        float right = 1.0f;
        std::uint32_t age = 0;
    };

    bool post(const Command &command);
    void render_chunk(std::int16_t *out, int frames);
    void apply(const Command &command);
    Voice &allocate_voice();
    static float tone_sample(Voice &voice);

    std::array<Command, kQueue> queue_{};
    std::atomic<std::size_t> head_{0}; // written by the game thread
    std::atomic<std::size_t> tail_{0}; // written by the audio thread
    std::array<Voice, kVoices> voices_{};
    std::array<float, static_cast<std::size_t>(Bus::count)> bus_gain_{};
    std::array<float, static_cast<std::size_t>(Bus::count)> bus_target_{};
    float master_gain_ = 1.0f;
    float master_target_ = 1.0f;
    std::uint32_t voice_clock_ = 0;
    std::atomic<int> active_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::array<StreamRing *, kStreams> streams_{};
    std::array<float, kStreams> stream_gain_{};
    std::array<float, kStreams> stream_target_{};
    std::array<float, kStreams> stream_step_{};
    std::array<std::array<float, kMaxGrain * 2>, kStreams> stream_buffer_{};
    std::atomic<std::uint64_t> underruns_{0};
    std::uint64_t fence_next_ = 0;
    std::atomic<std::uint64_t> fence_done_{0};
};

} // namespace pe::audio
