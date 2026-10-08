#pragma once

#include <cstdint>

namespace av {

/// Snapshot of everything the renderer needs for one frame. Plain data, published through a
/// TripleBuffer from the analysis worker to the render thread.
struct VisualState {
    static constexpr uint32_t kMaxBars = 128;
    float bars[kMaxBars] = {};
    uint32_t numBars = 0;
    float rms = 0.f;
};

}  // namespace av
