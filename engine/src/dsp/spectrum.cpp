#include "av/dsp/spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "pffft.h"

namespace av {

struct SpectrumAnalyzer::Impl {
    PFFFT_Setup* setup = nullptr;
    float* in = nullptr;
    float* out = nullptr;
    float* work = nullptr;
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
        pffft_aligned_free(in);
        pffft_aligned_free(out);
        pffft_aligned_free(work);
    }
};

SpectrumAnalyzer::SpectrumAnalyzer(size_t fftSize, double sampleRate, size_t numBars, double minHz,
                                   double maxHz)
    : impl_(std::make_unique<Impl>()),
      fftSize_(fftSize),
      numBars_(std::min(numBars, kMaxBars)),
      sampleRate_(sampleRate) {
    impl_->setup = pffft_new_setup(static_cast<int>(fftSize), PFFFT_REAL);
    impl_->in = static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float)));
    impl_->out = static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float)));
    impl_->work = static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float)));

    window_.resize(fftSize);  // Hann
    for (size_t i = 0; i < fftSize; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                            static_cast<double>(fftSize));
    mag_.assign(fftSize / 2, 0.f);

    maxHz = std::min(maxHz, sampleRate * 0.5 * 0.98);
    barLo_.resize(numBars_);
    barHi_.resize(numBars_);
    barCenter_.resize(numBars_);
    const double ratio = std::pow(maxHz / minHz, 1.0 / static_cast<double>(numBars_));
    for (size_t b = 0; b < numBars_; ++b) {
        const double lo = minHz * std::pow(ratio, static_cast<double>(b));
        const double hi = lo * ratio;
        size_t l = binForHz(lo);
        size_t h = std::max(binForHz(hi), l + 1);  // at least one bin per bar
        barLo_[b] = l;
        barHi_[b] = std::min(h, fftSize / 2);
        barCenter_[b] = static_cast<float>(std::sqrt(lo * hi) * static_cast<double>(fftSize) /
                                           sampleRate);
    }
}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

size_t SpectrumAnalyzer::binForHz(double hz) const {
    const double bin = hz * static_cast<double>(fftSize_) / sampleRate_;
    return std::min(static_cast<size_t>(std::max(bin, 1.0)), fftSize_ / 2 - 1);
}

void SpectrumAnalyzer::analyze(const float* mono, float* out) {
    for (size_t i = 0; i < fftSize_; ++i) impl_->in[i] = mono[i] * window_[i];
    pffft_transform_ordered(impl_->setup, impl_->in, impl_->out, impl_->work, PFFFT_FORWARD);

    // PFFFT ordered real output: [re0, re(N/2), re1, im1, re2, im2, ...]
    const float norm = 4.0f / static_cast<float>(fftSize_);  // Hann coherent gain 0.5, x2 for 1-sided
    mag_[0] = std::fabs(impl_->out[0]) * norm * 0.5f;
    for (size_t k = 1; k < fftSize_ / 2; ++k) {
        const float re = impl_->out[2 * k];
        const float im = impl_->out[2 * k + 1];
        mag_[k] = std::sqrt(re * re + im * im) * norm;
    }

    constexpr float kFloorDb = -80.f;
    for (size_t b = 0; b < numBars_; ++b) {
        float peak = 0.f;
        if (barHi_[b] - barLo_[b] <= 1) {
            // Bar narrower than one FFT bin: interpolate between neighbours so adjacent bass
            // bars still differ instead of sharing one value.
            const float c = std::clamp(barCenter_[b], 1.f, static_cast<float>(fftSize_ / 2 - 2));
            const size_t k = static_cast<size_t>(c);
            const float frac = c - static_cast<float>(k);
            peak = mag_[k] * (1.f - frac) + mag_[k + 1] * frac;
        } else {
            for (size_t k = barLo_[b]; k < barHi_[b]; ++k) peak = std::max(peak, mag_[k]);
        }
        const float db = 20.f * std::log10(std::max(peak, 1e-6f));
        out[b] = std::clamp((db - kFloorDb) / -kFloorDb, 0.f, 1.f);
    }
}

}  // namespace av
