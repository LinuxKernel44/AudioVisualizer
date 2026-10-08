#pragma once

#include <atomic>
#include <thread>

#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"
#include "av/core/visual_state.h"

namespace av {

/// Pulls mono samples from the playback tap, runs the FFT on a sliding window and publishes
/// smoothed VisualState snapshots. When no audio arrives (paused) the bars decay to zero.
class SpectrumWorker {
public:
    SpectrumWorker(SpscRing<float>& tap, TripleBuffer<VisualState>& out, int sampleRate);
    ~SpectrumWorker();
    void start();
    void stop();

private:
    void run();

    SpscRing<float>& tap_;
    TripleBuffer<VisualState>& out_;
    int sampleRate_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace av
