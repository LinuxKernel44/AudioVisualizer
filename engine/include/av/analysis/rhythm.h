#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "av/analysis/timeline.h"

namespace av {

struct TempoResult {
    float bpm = 0.f;
    float confidence = 0.f;    // 0..1, peak prominence of the autocorrelation
    std::vector<float> curve;  // local BPM every `stepSec`
    float stepSec = 2.f;
};

/// Prepares an onset envelope for tempo/beat tracking: removes the local mean and scales by the
/// global standard deviation, half-wave rectified.
std::vector<float> conditionOnsetEnvelope(const std::vector<float>& raw, double frameRate);

/// Global tempo plus a local tempo curve from an (already conditioned) onset envelope.
TempoResult estimateTempo(const std::vector<float>& env, double frameRate, float minBpm = 60.f,
                          float maxBpm = 200.f);

/// Dynamic-programming beat tracker (Ellis 2007) following the local tempo curve.
/// Returns beat times in seconds.
std::vector<float> trackBeats(const std::vector<float>& env, double frameRate,
                              const TempoResult& tempo, float tightness = 100.f);

struct DownbeatResult {
    std::vector<uint8_t> barPos;  // per beat, 0 = downbeat
    uint32_t beatsPerBar = 4;
    float confidence = 0.f;
};

/// Chooses meter (3 or 4 beats) and bar phase from beat-synchronous accents. `bassEnergy` is the
/// per-frame *linear* energy below ~250 Hz (log-compressed features hide the 1.5-2x level
/// difference that marks a downbeat); the onset channels add a weaker timing cue.
DownbeatResult findDownbeats(const std::vector<float>& beats, double frameRate,
                             const std::vector<float>& bassEnergy,
                             const std::vector<float>& onsetLow, const std::vector<float>& onset);

/// Peak-picks onset times from per-band onset strengths.
std::vector<Onset> pickOnsets(const Timeline& t);

struct KeyResult {
    int key = -1;
    bool minor = false;
    float confidence = 0.f;
};
/// Krumhansl-Schmuckler key estimate from a mean chroma vector.
KeyResult estimateKey(const std::array<float, 12>& chroma);

}  // namespace av
