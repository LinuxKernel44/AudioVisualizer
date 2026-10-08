#pragma once

#include <cstdint>

#include "av/core/visual_state.h"

namespace av {

/// Per-frame uniform block shared by every pass. Mirrors `shaders/common.glsl` (std140).
struct alignas(16) FrameData {
    float res[4];     // width, height, aspect, time
    float audio[4];   // rms, kick, beat pulse, beat phase
    float audio2[4];  // bar beat, section energy, bpm, hasTimeline
    float fx[4];      // bloom, chroma, grain, feedback
    float fx2[4];     // vignette, glitch, exposure, dt
    float pal[4][4];  // palette colours
    float bands[8];   // 7 band energies
    float bars[VisualState::kMaxBars];
    float wave[VisualState::kWaveSamples];
    float ctl[4];     // transition, sceneTime, bar count, unused
    float brand[4];   // overlay opacity, background opacity, unused, unused
};
static_assert(sizeof(FrameData) % 16 == 0, "std140 block must be a multiple of 16 bytes");
static_assert(sizeof(FrameData) == 16 * (5 + 4 + 2 + 32 + 64 + 1 + 1), "layout drift vs common.glsl");

}  // namespace av
