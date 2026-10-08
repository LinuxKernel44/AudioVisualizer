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

    // Timeline-driven values (valid when hasTimeline); all roughly 0..1 unless noted.
    bool hasTimeline = false;
    float beatPulse = 0.f;      // 1 on a beat, decays to 0 (stronger on the downbeat)
    float beatPhase = 0.f;      // 0..1 inside the current beat
    uint32_t barBeat = 0;       // position in the bar, 0 = downbeat
    float kick = 0.f;           // low-band onset flash
    float sectionEnergy = 0.f;  // energy of the current section
    float bpm = 0.f;
    float bands[8] = {};        // 7 normalised band energies (index 7 unused)
};

}  // namespace av
