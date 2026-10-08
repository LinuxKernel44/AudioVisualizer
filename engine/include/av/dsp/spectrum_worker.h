#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"
#include "av/analysis/analysis_job.h"
#include "av/core/visual_state.h"
#include "av/sim/director.h"
#include "av/sim/timeline_sampler.h"

namespace av {

/// Pulls mono samples from the playback tap, runs the FFT on a sliding window and publishes
/// smoothed VisualState snapshots. When no audio arrives (paused) the bars decay to zero.
class SpectrumWorker {
public:
    struct Playback {
        std::function<double()> positionSec;
        std::function<bool()> playing;
    };
    /// `job` (optional) is polled; once its Timeline is ready the beat/section fields are filled.
    SpectrumWorker(SpscRing<float>& tap, TripleBuffer<VisualState>& out, int sampleRate,
                   Playback playback = {}, const AnalysisJob* job = nullptr);
    ~SpectrumWorker();
    void start();
    void stop();
    /// -1 = automatic (follow the musical structure), otherwise a scene index.
    void setManualScene(int index) { manualScene_.store(index); }

private:
    void run();

    SpscRing<float>& tap_;
    TripleBuffer<VisualState>& out_;
    int sampleRate_;
    Playback playback_;
    const AnalysisJob* job_;
    std::atomic<bool> stop_{false};
    std::atomic<int> manualScene_{-1};
    std::thread thread_;
};

}  // namespace av
