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
/// All public methods are called from the Android main/worker threads and are not real-time.
class Engine {
public:
    Engine();
    ~Engine();

    /// Loads an audio file from an open fd (duplicated internally). Returns false with error.
    bool load(int fd, std::string* error);
    bool play();
    bool pause();
    bool seek(int64_t ms);
    void setSurface(ANativeWindow* window);
    /// -1 = automatic scene selection, otherwise a scene index.
    void setScene(int index) { manualScene_.store(index); if (worker_) worker_->setManualScene(index); }
    int sceneIndex() const { return manualScene_.load(); }
    void setSettings(const RenderSettings& s) {
        { std::lock_guard<std::mutex> lk(settingsMu_); settings_ = s; }
        if (worker_) worker_->setSettings(s);
    }

    /// Directory for cached analysis results (created by the caller). Empty disables caching.
    void setCacheDir(const std::string& dir) { cacheDir_ = dir; }
    /// Path of the Beat This! ONNX model (optional). Applies to the next file that is loaded.
    void setBeatModel(const std::string& path) { modelPath_ = path; }
    int analysisState() const { return static_cast<int>(analysis_.state()); }
    float analysisProgress() const { return analysis_.progress(); }
    bool analysisFromCache() const { return analysis_.fromCache(); }
    std::string analysisError() const { return analysis_.error(); }
    /// JSON summary (see timelineSummaryJson) or empty if the analysis is not finished.
    std::string summaryJson() const;

    bool loaded() const { return decoder_ != nullptr; }
    bool isPlaying() const { return player_ && player_->isPlaying(); }
    bool finished() const { return player_ && player_->finished(); }
    int64_t positionMs() const { return player_ ? player_->positionMs() : 0; }
    int64_t durationMs() const { return decoder_ ? decoder_->durationUs() / 1000 : 0; }
    float frameMs() const { return renderer_.lastFrameMs(); }
    void setTargetFps(float fps) { renderer_.setTargetFps(fps); }
    void setThermalLevel(int level) { renderer_.setThermalLevel(level); }
    void setAutoScale(bool on) { renderer_.setAutoScale(on); }
    void setPowerSaver(bool on) { renderer_.setPowerSaver(on); }
    void setOverlayImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) { renderer_.setOverlayImage(std::move(rgba), w, h); }
    void setBackgroundImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) { renderer_.setBackgroundImage(std::move(rgba), w, h); }
    void setBrandingOpacity(float overlay, float background) { renderer_.setBrandingOpacity(overlay, background); }
    VulkanRenderer::Perf perf() const { return renderer_.perf(); }

private:
    void unload();

    TripleBuffer<VisualState> state_;  // declared first: outlives worker and renderer
    AnalysisJob analysis_;             // outlives the worker that polls it
    std::string cacheDir_;
    std::string modelPath_;
    std::atomic<int> manualScene_{-1};
    std::mutex settingsMu_;
    RenderSettings settings_;
    SpscRing<float> tap_{1 << 15};
    VulkanRenderer renderer_{state_};
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<SpectrumWorker> worker_;
};

}  // namespace av
