#pragma once

#include <array>
#include <memory>

#include "av/analysis/timeline.h"
#include "av/core/visual_state.h"

namespace av {

/// Turns a Timeline plus a playback position into the timeline-driven fields of VisualState.
/// Band channels are rescaled per track (5th..98th percentile) so quiet and loud material both
/// use the full 0..1 range.
class TimelineSampler {
   public:
    explicit TimelineSampler(std::shared_ptr<const Timeline> t);

    /// Fills the timeline fields of `out` for playback position `sec`.
    void sample(double sec, VisualState* out) const;

    const Timeline& timeline() const { return *t_; }

   private:
    std::shared_ptr<const Timeline> t_;
    std::array<float, kNumBands> lo_{}, hi_{};
};

}  // namespace av
