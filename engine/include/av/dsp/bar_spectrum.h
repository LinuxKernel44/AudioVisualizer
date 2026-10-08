#pragma once

#include <cstddef>
#include <vector>

#include "av/dsp/spectrum.h"

namespace av {

/// Multi-resolution log-spaced bar spectrum: a long FFT resolves the bass, a short one keeps
/// the mids and highs responsive. Input is the most recent `window()` mono samples (newest last).
class BarSpectrum {
   public:
    BarSpectrum(double sampleRate, size_t numBars, double minHz = 30.0, double maxHz = 16000.0,
                double crossoverHz = 300.0);

    size_t numBars() const { return numLow_ + numHigh_; }
    /// Number of most recent samples analyze() needs.
    size_t window() const { return lowSize_; }
    void analyze(const float* recent, float* out);

   private:
    static constexpr size_t kLowSize = 8192;
    static constexpr size_t kHighSize = 2048;
    size_t lowSize_ = kLowSize;
    size_t numLow_, numHigh_;
    SpectrumAnalyzer low_;
    SpectrumAnalyzer high_;
};

}  // namespace av
