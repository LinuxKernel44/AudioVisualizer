#pragma once

#include <cstddef>
#include <vector>

#include "av/analysis/timeline.h"

namespace av {

struct SectionInput {
    const std::vector<float>* vectors = nullptr;  // groups * dims, row-major
    size_t dims = 0;
    size_t groups = 0;
    double groupSec = 0.25;
    const std::vector<float>* loudness = nullptr;  // per group, LUFS
    const std::vector<float>* beats = nullptr;     // optional, seconds: boundaries snap to them
    const std::vector<float>* beatBar = nullptr;   // unused placeholder for future meter-aware snapping
    double durationSec = 0;
};

/// Foote-style structural segmentation: banded self-similarity -> checkerboard novelty ->
/// peak picking -> merging of short sections -> clustering and heuristic labelling.
std::vector<Section> segmentStructure(const SectionInput& in);

/// Novelty curve only (exposed for tests).
std::vector<float> noveltyCurve(const std::vector<float>& normalizedVectors, size_t dims,
                                size_t groups, size_t halfWidth);

}  // namespace av
