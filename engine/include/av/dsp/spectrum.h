#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace av {

/// Windowed real FFT (PFFFT) reduced to logarithmically spaced, dB-normalised bars.
class SpectrumAnalyzer {
   public:
    static constexpr size_t kMaxBars = 128;

    /// fftSize must be a power of two >= 64.
    SpectrumAnalyzer(size_t fftSize, double sampleRate, size_t numBars, double minHz = 30.0,
                     double maxHz = 16000.0);
    ~SpectrumAnalyzer();
    SpectrumAnalyzer(const SpectrumAnalyzer&) = delete;
    SpectrumAnalyzer& operator=(const SpectrumAnalyzer&) = delete;

    size_t fftSize() const { return fftSize_; }
    size_t numBars() const { return numBars_; }

    /// Analyzes fftSize mono samples. Writes numBars values in [0, 1] to out.
    void analyze(const float* mono, float* out);

    /// Raw linear magnitude spectrum (fftSize/2 bins) of the last analyze() call.
    const std::vector<float>& magnitudes() const { return mag_; }

    /// Index of the FFT bin for a frequency.
    size_t binForHz(double hz) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    size_t fftSize_;
    size_t numBars_;
    double sampleRate_;
    std::vector<float> window_;
    std::vector<float> mag_;
    std::vector<size_t> barLo_, barHi_;
    std::vector<float> barCenter_;
};

}  // namespace av
