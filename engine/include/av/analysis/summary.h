#pragma once

#include <string>

#include "av/analysis/timeline.h"

namespace av {

/// Human-facing summary of a Timeline as JSON: tempo, key, loudness, sections, beats and the
/// per-second energy curve. Used by the UI layer.
std::string timelineSummaryJson(const Timeline& t);

/// "C", "F#", ... or "?" for an unknown pitch class.
const char* pitchClassName(int pc);

}  // namespace av
