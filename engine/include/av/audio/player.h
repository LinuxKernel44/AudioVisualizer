#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>

#include "av/core/spsc_ring.h"

namespace av {

/// Low-latency AAudio output fed from a decoder ring. The data callback is lock-free and
/// allocation-free; it also taps a mono mix into a second ring for the analysis worker.
class Player {
   public:
    Player(SpscRing<float>& source, SpscRing<float>& tap, const std::atomic<bool>& sourceEof);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(int sampleRate);
    void close();
    bool play();
    bool pause();
    /// Pauses and blocks until the data callback can no longer run (needed before seeking).
    void pauseAndWait();
    /// Only valid while the stream is not running. Rebases the position and clears 'finished'.
    void setPositionMs(int64_t ms);

    bool isPlaying() const { return playing_.load(); }
    bool finished() const { return finished_.load(); }
    /// Position of the sound the listener is hearing right now: the frames handed to AAudio minus
    /// the output latency measured from the stream's hardware timestamps.
    int64_t positionMs() const;
    /// Measured output latency (frames still in flight between the app and the speaker).
    int latencyMs() const { return static_cast<int>(latencyFrames_.load() * 1000 / sampleRate_); }

   private:
    static aaudio_data_callback_result_t onData(AAudioStream*, void* user, void* audio,
                                                int32_t frames);
    static void onError(AAudioStream*, void* user, aaudio_result_t error);

    SpscRing<float>& source_;
    SpscRing<float>& tap_;
    const std::atomic<bool>& sourceEof_;
    AAudioStream* stream_ = nullptr;
    int sampleRate_ = 48000;
    std::atomic<bool> playing_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int64_t> framesPlayed_{0};
    mutable std::atomic<int64_t> latencyFrames_{0};
    mutable std::atomic<int64_t> lastLatencyNs_{0};
};

}  // namespace av
