#pragma once

#include <memory>
#include <vector>

#include "av/analysis/timeline.h"
#include "av/core/render_settings.h"
#include "av/core/visual_state.h"
#include "av/dsp/bar_spectrum.h"
#include "av/sim/director.h"
#include "av/sim/timeline_sampler.h"

namespace av {

/// Turns audio plus (optionally) the analysis Timeline into VisualState snapshots. Shared by the
/// real-time preview worker and the offline exporter: given the same samples, step times and
/// positions the output is identical (no clocks, no threads, no randomness).
class FrontEnd {
   public:
    explicit FrontEnd(double sampleRate, size_t numBars = 64);

    /// Appends mono samples to the analysis history (only the newest window() are kept).
    void pushSamples(const float* mono, size_t n);
    /// Analyses the newest window and updates the smoothed bars. `dtSec` is the time since the
    /// previous step (smoothing is time-based, so the step rate does not change the look).
    void step(double dtSec);
    /// No audio is arriving (paused): let everything fall to silence.
    void decay(double dtSec);
    /// Composes the full VisualState for playback position `positionSec`.
    void fill(double positionSec, double dtSec, bool playing, VisualState* out);

    void setSettings(const RenderSettings& s) { settings_ = s; }
    void setTimeline(std::shared_ptr<const Timeline> t);
    void setManualScene(int index) { director_.setManualScene(index); }
    bool hasTimeline() const { return sampler_ != nullptr; }
    const Director& director() const { return director_; }

   private:
    void applyBars(const float* raw);

    double sr_;
    size_t numBars_;
    BarSpectrum analyzer_;
    std::vector<float> history_;  // newest samples last, size = analyzer window
    std::vector<float> raw_, smooth_;
    float rms_ = 0.f;
    float lastHopRms_ = 0.f;
    RenderSettings settings_;
    std::unique_ptr<TimelineSampler> sampler_;
    Director director_;
};

}  // namespace av
