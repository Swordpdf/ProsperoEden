// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"
#include "preferences.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>
#include "common/logging.h"
#include "native_audio.hpp"
#include "audio_out_init.h"

namespace Eden {
using namespace AudioCore::Sink;
using namespace ps5::native_audio;

namespace {
// Microphone input is not captured on the PS5. Upstream's NullSinkStreamImpl returns an empty
// vector from ReleaseBuffer, and DeviceSession::ReleaseBuffer then copies buffer.size bytes from
// its data(), a null pointer, into guest memory: any game that opens AudioIn (Portal's voice
// chat, for one) crashed on the AudioManager thread. Release silence of the requested length.
class SilentInputStream final : public SinkStream {
public:
    explicit SilentInputStream(Core::System& system) : SinkStream{system, StreamType::In} {}
    void AppendBuffer(SinkBuffer&, std::span<s16>) override {}
    std::vector<s16> ReleaseBuffer(u64 num_samples) override {
        return std::vector<s16>(num_samples, 0);
    }
};
} // namespace

AudioStream::AudioStream(Core::System& system, u32 channels, const std::string& name_, StreamType type)
    : SinkStream{system, type} {
    if (type == StreamType::In || (channels != 1 && channels != 2 && channels != 6))
        throw std::invalid_argument("Unsupported AudioOut stream format");
    name = name_;
    system_channels = channels;
    device_channels = 2;
    SetRingSize(4);
    if (!AudioOutReady()) throw std::runtime_error("sceAudioOutInit failed");
    handle = sceAudioOutOpen(0xff, 0, 0, kAudioOutGrain, kAudioOutRate, kAudioOutStereoS16);
    if (handle < 0) throw std::runtime_error("sceAudioOutOpen failed");
    std::array<int, 8> volumes;
    volumes.fill(AudioVolume(PreferencesFor(session_title.load())));
    if (sceAudioOutSetVolume(handle, 3, volumes.data()) < 0) {
        sceAudioOutClose(handle);
        handle = -1;
        throw std::runtime_error("sceAudioOutSetVolume failed");
    }
    try {
        worker = std::jthread([this](std::stop_token stop) {
            std::array<s16, kAudioOutGrain * 2> block{};
            std::unique_lock lock(mutex);
            while (!stop.stop_requested()) {
                if (!wake.wait(lock, stop, [this] { return !IsPaused(); })) break;
                ProcessAudioOutAndRender(block, kAudioOutGrain);
                // AudioOut paces complete 256-frame, 48 kHz stereo blocks.
                in_flight = true;
                lock.unlock();
                const int result = sceAudioOutOutput(handle, block.data());
                lock.lock();
                in_flight = false;
                wake.notify_all();
                if (result < 0) {
                    failed = true;
                    SignalPause();
                    LOG_ERROR(Audio_Sink, "PS5 AudioOut output failed");
                    break;
                }
                output_frames += kAudioOutGrain;
                for (std::size_t frame = 0; frame < kAudioOutGrain; ++frame)
                    nonzero_frames += block[frame * 2] != 0 || block[frame * 2 + 1] != 0;
            }
        });
    } catch (...) {
        sceAudioOutClose(handle);
        handle = -1;
        throw;
    }
}
AudioStream::~AudioStream() { Finalize(); }
void AudioStream::Start(bool) {
    std::scoped_lock lock(mutex);
    if (handle >= 0 && !failed) { paused = false; wake.notify_one(); }
}
void AudioStream::Stop() {
    SignalPause();
    std::unique_lock lock(mutex);
    wake.wait(lock, [this] { return !in_flight; });
}
void AudioStream::Finalize() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable()) worker.join();
    std::scoped_lock lock(mutex);
    SignalPause();
    if (handle >= 0) {
        const int drain = sceAudioOutOutput(handle, nullptr);
        const int close = sceAudioOutClose(handle);
        LOG_INFO(Audio_Sink, "EDEN_AUDIO_PORT_CLOSED frames={} nonzero={} failed={} drain={} close={}",
                 output_frames, nonzero_frames, failed.load(), drain, close);
        handle = -1;
    }
}
void AudioStream::AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) {
    std::scoped_lock lock(mutex); // Publish PCM and its descriptor before the consumer runs.
    if (buffer.frames == 0 || buffer.frames > 0x8000 || buffer.frames_played != 0 ||
        samples.size() != buffer.frames * system_channels)
        throw std::invalid_argument("Invalid PCM buffer dimensions");
    if (failed || handle < 0) throw std::runtime_error("AudioOut stream is unavailable");
    SinkStream::AppendBuffer(buffer, samples);
}
SinkStream* AudioSink::AcquireSinkStream(Core::System& system, u32 channels,
                                        const std::string& name, StreamType type) {
    system_channels = channels;
    // Microphone capture is outside this output adapter.
    SinkStreamPtr stream = type == StreamType::In ?
        SinkStreamPtr{std::make_unique<SilentInputStream>(system)} :
        SinkStreamPtr{std::make_unique<AudioStream>(system, channels, name, type)};
    stream->SetDeviceVolume(device_volume);
    stream->SetSystemVolume(system_volume);
    auto* result = stream.get();
    streams.push_back(std::move(stream));
    return result;
}
void AudioSink::CloseStream(SinkStream* stream) {
    std::erase_if(streams, [stream](const auto& item) { return item.get() == stream; });
}
void AudioSink::SetDeviceVolume(f32 value) {
    device_volume = std::clamp(value, 0.0f, 1.0f);
    for (auto& stream : streams) stream->SetDeviceVolume(device_volume);
}
void AudioSink::SetSystemVolume(f32 value) {
    system_volume = std::clamp(value, 0.0f, 1.0f);
    for (auto& stream : streams) stream->SetSystemVolume(system_volume);
}
}
