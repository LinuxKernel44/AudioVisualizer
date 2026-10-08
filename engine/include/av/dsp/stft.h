#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace av {

/// Hann-windowed real FFT returning linear amplitude spectra (a unit sine peaks near 1.0).
/// Instances are not thread-safe; use one per thread.
class Stft {
   public:
    explicit Stft(size_t fftSize);
    ~Stft();
    Stft(const Stft&) = delete;
    Stft& operator=(const Stft&) = delete;

    size_t fftSize() const { return n_; }
    size_t numBins() const { return n_ / 2 + 1; }
    /// `in` holds fftSize samples; `mag` receives numBins amplitudes.
    void magnitude(const float* in, float* mag);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    size_t n_;
    std::vector<float> window_;
};

}  // namespace av
