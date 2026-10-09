#pragma once

#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "av/analysis/analysis_job.h"
#include "av/audio/decoder.h"
#include "av/audio/player.h"
#include "av/core/render_settings.h"
#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"
#include "av/core/visual_state.h"
#include "av/dsp/spectrum_worker.h"
#include "av/render/vulkan_renderer.h"

namespace av {

/// Top-level native object: wires decoder -> player -> analysis worker -> renderer.
/// Public methods may be called from any thread. Transport calls (load, play, pause, seek) are
/// serialised by a mutex and may block (opening a file or seeking waits for the decoder); the
/// cheap getters never block: while a transport call is in progress they return the last values.
class Engine {
   public:
    Engine();
    ~Engine();

    /// Loads an audio file from an open fd (duplicated internally). Returns false with error.
    bool load(int fd, std::string* error);
    /// Starts playback; after the end of the track it starts again from the beginning.
    bool play();
    bool pause();
    bool seek(int64_t ms);
    void setSurface(ANativeWindow* window);
    /// -1 = automatic scene selection, otherwise a scene index.
    void setScene(int index) { controls_.manualScene.store(index); }
    int sceneIndex() const { return controls_.manualScene.load(); }
    void setSettings(const RenderSettings& s) { controls_.setSettings(s); }

    /// Directory for cached analysis results (created by the caller). Empty disables caching.
    void setCacheDir(const std::string& dir) {
        std::lock_guard<std::mutex> lk(mu_);
        cacheDir_ = dir;
    }
    /// Path of the Beat This! ONNX model (optional). Applies to the next file that is loaded.
    void setBeatModel(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        modelPath_ = path;
    }
    int analysisState() const { return static_cast<int>(analysis_.state()); }
    float analysisProgress() const { return analysis_.progress(); }
    bool analysisFromCache() const { return analysis_.fromCache(); }
    std::string analysisError() const { return analysis_.error(); }
    /// JSON summary (see timelineSummaryJson) or empty if the analysis is not finished.
    std::string summaryJson() const;

    bool loaded() const;
    bool isPlaying() const;
    bool finished() const;
    int64_t positionMs() const;
    int64_t durationMs() const;
    float frameMs() const { return renderer_.lastFrameMs(); }
    void setTargetFps(float fps) { renderer_.setTargetFps(fps); }
    void setThermalLevel(int level) { renderer_.setThermalLevel(level); }
    void setAutoScale(bool on) { renderer_.setAutoScale(on); }
    void setPowerSaver(bool on) { renderer_.setPowerSaver(on); }
    void setOverlayImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
        renderer_.setOverlayImage(std::move(rgba), w, h);
    }
    void setBackgroundImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
        renderer_.setBackgroundImage(std::move(rgba), w, h);
    }
    void setBrandingOpacity(float overlay, float background) {
        renderer_.setBrandingOpacity(overlay, background);
    }
    VulkanRenderer::Perf perf() const { return renderer_.perf(); }
    int audioLatencyMs() const;

   private:
    void unloadLocked();
    bool seekLocked(int64_t ms);
    /// Refreshes the cached getter values; requires mu_.
    void snapshotLocked() const;

    TripleBuffer<VisualState> state_;  // declared first: outlives worker and renderer
    AnalysisJob analysis_;             // outlives the worker that polls it
    WorkerControls controls_;          // outlives the worker that reads it
    mutable std::mutex mu_;            // guards everything below plus cacheDir_/modelPath_
    std::string cacheDir_;
    std::string modelPath_;
    SpscRing<float> tap_{1 << 15};
    VulkanRenderer renderer_{state_};
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<SpectrumWorker> worker_;

    // Last values seen by the getters, returned while a transport call holds mu_.
    mutable std::atomic<bool> cLoaded_{false}, cPlaying_{false}, cFinished_{false};
    mutable std::atomic<int64_t> cPositionMs_{0}, cDurationMs_{0};
    mutable std::atomic<int> cLatencyMs_{0};
};

}  // namespace av
