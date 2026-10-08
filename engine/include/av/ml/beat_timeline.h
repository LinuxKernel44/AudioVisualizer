#pragma once

#include <vector>

#include "av/analysis/timeline.h"
#include "av/ml/beat_inference.h"

namespace av {

/// How well a beat grid lines up with the audible pulse: the mean, over beats, of the conditioned
/// low/mid-weighted onset flux within +-30 ms. Higher is better; 0 if there is nothing to measure.
float beatAlignmentScore(const Timeline& t, const std::vector<float>& beatsSec);

/// Replaces the rhythm fields of `t` (bpm, tempo curve, beats, bar positions, meter, confidences)
/// with the model's beat/downbeat times. Returns false (leaving `t` untouched) when the model found
/// too few beats to be trusted.
bool applyMlBeats(const BeatTimes& bt, Timeline* t);

}  // namespace av
