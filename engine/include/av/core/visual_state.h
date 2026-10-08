#pragma once

#include <cstdint>

namespace av {

/// Post-processing / colour look. Plain data so it can travel inside VisualState.
struct Look {
    float bloom = 0.5f;
    float chroma = 0.0015f;  // chromatic aberration strength (fraction of screen)
    float grain = 0.025f;
    float feedback = 0.f;  // 0..0.95, amount of the previous frame blended back in
    float vignette = 0.45f;
    float glitch = 0.f;  // 0..1
    float exposure = 0.9f;
    float palette[4][4] = {{0.38f, 0.20f, 1.00f, 1.f},
                           {0.00f, 0.72f, 0.95f, 1.f},
                           {1.00f, 0.30f, 0.62f, 1.f},
                           {0.98f, 0.82f, 0.25f, 1.f}};
};

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
    float bands[8] = {};  // 7 normalised band energies (index 7 unused)

    static constexpr uint32_t kWaveSamples = 256;
    float wave[kWaveSamples] = {};  // recent mono waveform (peak-decimated), -1..1

    // Scene selection (written by the director; the renderer only reads it).
    uint32_t sceneA = 0;
    uint32_t sceneB = 0;
    float transition = 0.f;  // 0 = scene A only, 1 = scene B only
    float sceneTime = 0.f;   // seconds since scene A started
    Look look;
};

}  // namespace av
