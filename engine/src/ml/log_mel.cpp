#include "av/ml/log_mel.h"

#include <algorithm>
#include <cmath>

#include "pffft.h"

namespace av {

namespace {

// torchaudio's slaney mel scale.
double hzToMel(double f) {
    constexpr double fSp = 200.0 / 3.0, minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp, logstep = std::log(6.4) / 27.0;
    return f >= minLogHz ? minLogMel + std::log(f / minLogHz) / logstep : f / fSp;
}
double melToHz(double m) {
    constexpr double fSp = 200.0 / 3.0, minLogHz = 1000.0;
    const double minLogMel = minLogHz / fSp, logstep = std::log(6.4) / 27.0;
    return m >= minLogMel ? minLogHz * std::exp(logstep * (m - minLogMel)) : fSp * m;
}

}  // namespace

struct LogMel::Impl {
    PFFFT_Setup* setup = nullptr;
    float* in = nullptr;
    float* out = nullptr;
    float* work = nullptr;
    std::vector<float> window;  // periodic Hann, as torch.hann_window
    struct Filter {
        size_t first = 0;
        std::vector<float> w;
    };
    std::vector<Filter> fb;

    Impl() {
        setup = pffft_new_setup(static_cast<int>(kFft), PFFFT_REAL);
        in = static_cast<float*>(pffft_aligned_malloc(kFft * sizeof(float)));
        out = static_cast<float*>(pffft_aligned_malloc(kFft * sizeof(float)));
        work = static_cast<float*>(pffft_aligned_malloc(kFft * sizeof(float)));
        window.resize(kFft);
        for (size_t i = 0; i < kFft; ++i)
            window[i] =
                static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                                        static_cast<double>(kFft)));

        // melscale_fbanks(n_freqs=513, f_min=30, f_max=11000, n_mels=128, sr=22050, norm=None,
        // "slaney")
        const size_t nFreqs = kFft / 2 + 1;
        const double mMin = hzToMel(30.0), mMax = hzToMel(11000.0);
        std::vector<double> fPts(kMels + 2);
        for (size_t i = 0; i < fPts.size(); ++i)
            fPts[i] = melToHz(mMin + (mMax - mMin) * static_cast<double>(i) /
                                         static_cast<double>(kMels + 1));
        fb.resize(kMels);
        for (size_t m = 0; m < kMels; ++m) {
            std::vector<float> w;
            size_t first = nFreqs, last = 0;
            std::vector<float> full(nFreqs, 0.f);
            for (size_t k = 0; k < nFreqs; ++k) {
                const double f = static_cast<double>(k) * (static_cast<double>(kSampleRate / 2)) /
                                 static_cast<double>(nFreqs - 1);
                const double down = (f - fPts[m]) / (fPts[m + 1] - fPts[m]);
                const double up = (fPts[m + 2] - f) / (fPts[m + 2] - fPts[m + 1]);
                const double v = std::max(0.0, std::min(down, up));
                full[k] = static_cast<float>(v);
                if (v > 0) {
                    first = std::min(first, k);
                    last = std::max(last, k);
                }
            }
            if (first <= last) {
                fb[m].first = first;
                fb[m].w.assign(full.begin() + static_cast<long>(first),
                               full.begin() + static_cast<long>(last) + 1);
            }
        }
    }
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
        pffft_aligned_free(in);
        pffft_aligned_free(out);
        pffft_aligned_free(work);
    }
};

LogMel::LogMel() : impl_(std::make_unique<Impl>()) {}
LogMel::~LogMel() = default;

float LogMel::sampleAt(long long idx, long long total) const {
    if (idx < 0) idx = -idx;                         // reflect at the start (x[-k] = x[k])
    if (idx >= total) idx = 2 * (total - 1) - idx;   // reflect at the end
    idx = std::clamp<long long>(idx, 0, total - 1);  // very short inputs: clamp to the edge
    return buf_[static_cast<size_t>(idx - base_)];
}

void LogMel::emitFrame(size_t f, const float* window, std::vector<float>* out) {
    Impl& I = *impl_;
    for (size_t i = 0; i < kFft; ++i) I.in[i] = window[i] * I.window[i];
    pffft_transform_ordered(I.setup, I.in, I.out, I.work, PFFFT_FORWARD);
    // Magnitudes of bins 0..512 (PFFFT ordered real layout: [re0, reN/2, re1, im1, ...]).
    float mag[kFft / 2 + 1];
    const float norm = 1.0f / std::sqrt(static_cast<float>(kFft));  // normalized="frame_length"
    mag[0] = std::fabs(I.out[0]) * norm;
    mag[kFft / 2] = std::fabs(I.out[1]) * norm;
    for (size_t k = 1; k < kFft / 2; ++k)
        mag[k] =
            std::sqrt(I.out[2 * k] * I.out[2 * k] + I.out[2 * k + 1] * I.out[2 * k + 1]) * norm;
    const size_t base = out->size();
    out->resize(base + kMels);
    for (size_t m = 0; m < kMels; ++m) {
        float e = 0.f;
        const auto& flt = I.fb[m];
        for (size_t i = 0; i < flt.w.size(); ++i) e += flt.w[i] * mag[flt.first + i];
        (*out)[base + m] = std::log1p(1000.f * e);
    }
    (void)f;
}

void LogMel::push(const float* x, size_t n, std::vector<float>* out) {
    buf_.insert(buf_.end(), x, x + n);
    total_ += static_cast<long long>(n);
    float window[kFft];
    // Frame f needs samples up to f*hop + 511 (its right half) to be present.
    while (static_cast<long long>(next_ * kHop) + static_cast<long long>(kFft / 2) <= total_) {
        const long long start =
            static_cast<long long>(next_ * kHop) - static_cast<long long>(kFft / 2);
        for (size_t i = 0; i < kFft; ++i)
            window[i] = sampleAt(start + static_cast<long long>(i), total_ + 1'000'000'000LL);
        emitFrame(next_, window, out);
        ++next_;
    }
    // Drop samples that no later frame can need (keep the left reflection source too: index < 512).
    const long long keepFrom =
        std::max<long long>(static_cast<long long>(next_ * kHop) - static_cast<long long>(kFft / 2),
                            static_cast<long long>(kFft));
    if (keepFrom > base_ + 65536) {
        const size_t drop = static_cast<size_t>(keepFrom - base_);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(std::min(drop, buf_.size())));
        base_ += static_cast<long long>(drop);
    }
}

void LogMel::finish(std::vector<float>* out) {
    if (finished_) return;
    finished_ = true;
    const size_t frames = frameCount(static_cast<size_t>(total_));
    float window[kFft];
    while (next_ < frames) {
        const long long start =
            static_cast<long long>(next_ * kHop) - static_cast<long long>(kFft / 2);
        for (size_t i = 0; i < kFft; ++i)
            window[i] = sampleAt(start + static_cast<long long>(i), total_);
        emitFrame(next_, window, out);
        ++next_;
    }
}

}  // namespace av
