#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "av/ml/beat_inference.h"

namespace av {

/// True if this build can run the beat model (ONNX Runtime linked in).
bool beatModelAvailable();

/// Decodes the audio behind `fd` again, runs the Beat This! model over it and returns beat and
/// downbeat times. Streams: memory use does not grow with the track length.
bool runBeatModelPass(int fd, const std::string& modelPath, const std::atomic<bool>* cancel,
                      const std::function<void(float)>& progress, BeatTimes* out,
                      std::string* error);

}  // namespace av
