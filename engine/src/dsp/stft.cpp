#include "av/dsp/stft.h"

#include <cmath>

#include "pffft.h"

namespace av {

struct Stft::Impl {
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

Stft::Stft(size_t fftSize) : impl_(std::make_unique<Impl>()), n_(fftSize) {
    impl_->setup = pffft_new_setup(static_cast<int>(n_), PFFFT_REAL);
    impl_->in = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    impl_->out = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    impl_->work = static_cast<float*>(pffft_aligned_malloc(n_ * sizeof(float)));
    window_.resize(n_);
    for (size_t i = 0; i < n_; ++i)
        window_[i] = 0.5f - 0.5f * static_cast<float>(
                                       std::cos(2.0 * M_PI * static_cast<double>(i) /
                                                static_cast<double>(n_)));
}

Stft::~Stft() = default;

void Stft::magnitude(const float* in, float* mag) {
    for (size_t i = 0; i < n_; ++i) impl_->in[i] = in[i] * window_[i];
    pffft_transform_ordered(impl_->setup, impl_->in, impl_->out, impl_->work, PFFFT_FORWARD);
    const float norm = 4.0f / static_cast<float>(n_);
    mag[0] = std::fabs(impl_->out[0]) * norm * 0.5f;
    mag[n_ / 2] = std::fabs(impl_->out[1]) * norm * 0.5f;  // Nyquist packed at index 1
    for (size_t k = 1; k < n_ / 2; ++k) {
        const float re = impl_->out[2 * k];
        const float im = impl_->out[2 * k + 1];
        mag[k] = std::sqrt(re * re + im * im) * norm;
    }
}

}  // namespace av
