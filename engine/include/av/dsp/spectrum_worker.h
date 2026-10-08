#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "av/analysis/analysis_job.h"
#include "av/core/render_settings.h"
#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"
#include "av/core/visual_state.h"

namespace av {

/// Real-time front end: pulls mono samples from the playback tap, drives a FrontEnd and publishes
/// VisualState snapshots (~100 Hz while audio flows, ~60 Hz while idle so transitions continue).
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
    void setSettings(const RenderSettings& s) {
        std::lock_guard<std::mutex> lk(settingsMu_);
        settings_ = s;
        settingsGen_.fetch_add(1);
    }

private:
    void run();

    SpscRing<float>& tap_;
    TripleBuffer<VisualState>& out_;
    int sampleRate_;
    Playback playback_;
    const AnalysisJob* job_;
    std::atomic<bool> stop_{false};
    std::atomic<int> manualScene_{-1};
    std::mutex settingsMu_;
    RenderSettings settings_;
    std::atomic<uint32_t> settingsGen_{0};
    std::thread thread_;
};

}  // namespace av
