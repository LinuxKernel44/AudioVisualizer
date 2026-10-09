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

/// Controls shared between the app-facing Engine (any thread) and the worker. They outlive any
/// single worker, so a change made while a track is being (re)loaded is never lost.
struct WorkerControls {
    std::atomic<int> manualScene{-1};  // -1 = automatic (follow the musical structure)
    std::mutex settingsMu;
    RenderSettings settings;  // guarded by settingsMu
    std::atomic<uint32_t> settingsGen{0};
    /// Bumped to make the worker discard everything queued in the tap (after a seek). Only the
    /// worker reads the tap, so only the worker may drop samples from it.
    std::atomic<uint32_t> flushGen{0};

    void setSettings(const RenderSettings& s) {
        {
            std::lock_guard<std::mutex> lk(settingsMu);
            settings = s;
        }
        settingsGen.fetch_add(1);
    }
};

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
                   WorkerControls& controls, Playback playback = {},
                   const AnalysisJob* job = nullptr);
    ~SpectrumWorker();
    void start();
    void stop();

   private:
    void run();

    SpscRing<float>& tap_;
    TripleBuffer<VisualState>& out_;
    int sampleRate_;
    WorkerControls& controls_;
    Playback playback_;
    const AnalysisJob* job_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace av
