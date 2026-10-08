#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "av/analysis/pcm_source.h"
#include "av/analysis/timeline.h"

namespace av {

struct AnalyzerOptions {
    size_t threads = 0;                   // 0 = hardware concurrency
    double expectedDurationSec = 0;       // only used for progress reporting
    std::function<void(float)> progress;  // 0..1, called from the producer thread
    const std::atomic<bool>* cancel = nullptr;
    /// Optional: called after the classical rhythm stage and before onsets/structure, with the
    /// partially filled Timeline. Lets the ML beat model replace beats, tempo and meter.
    std::function<void(Timeline&)> refineRhythm;
    /// Optional: receives a one-line breakdown of where the wall time went.
    std::function<void(const std::string&)> timingReport;
};

/// Offline analysis pipeline. A producer thread reads PCM, measures loudness sequentially and
/// slices the stream into self-contained chunks that a thread pool turns into features; the
/// global stages (tempo, beats, key, structure) then run on the merged result. The result does
/// not depend on the number of threads.
bool analyzeAudio(PcmSource& source, const AnalyzerOptions& opts, Timeline* out,
                  std::string* error);

}  // namespace av
