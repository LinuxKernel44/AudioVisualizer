#include "av/ml/resampler.h"

#include <algorithm>
#include <cmath>

namespace av {

namespace {
double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
}  // namespace

Resampler::Resampler(double inRate, double outRate) : ratio_(inRate / outRate) {
    cutoff_ = std::min(1.0, outRate / inRate) * 0.96;  // stay below the output Nyquist
    halfWidth_ = static_cast<int>(std::ceil(20.0 / cutoff_));
    table_.resize(static_cast<size_t>(halfWidth_) * kTableRes + 2);
    constexpr double beta = 9.0;
    const double i0b = bessel0(beta);
    for (size_t i = 0; i < table_.size(); ++i) {
        const double t = static_cast<double>(i) / kTableRes;  // distance in input samples
        const double u = t / halfWidth_;
        const double win = u < 1.0 ? bessel0(beta * std::sqrt(1.0 - u * u)) / i0b : 0.0;
        const double x = M_PI * t * cutoff_;
        const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
        table_[i] = static_cast<float>(cutoff_ * sinc * win);
    }
}

void Resampler::drain(std::vector<float>* out, bool final) {
    for (;;) {
        const double pos =
            static_cast<double>(produced_) * ratio_;  // input time of the next output
        const long long need = static_cast<long long>(std::floor(pos)) + halfWidth_;
        if (!final && need >= total_) break;
        if (final && pos > static_cast<double>(total_ - 1)) break;
        const long long fl = static_cast<long long>(std::floor(pos));
        const long long first = std::max<long long>({fl - halfWidth_ + 1, base_, 0});
        double acc = 0.0;
        for (long long i = first; i <= fl + halfWidth_; ++i) {
            if (i >= total_) break;  // beyond the end: implicit silence
            const double d = std::fabs(pos - static_cast<double>(i));
            if (d >= halfWidth_) continue;
            const double ft = d * kTableRes;
            const size_t k = static_cast<size_t>(ft);
            const float w = table_[k] + (table_[k + 1] - table_[k]) *
                                            static_cast<float>(ft - static_cast<double>(k));
            acc += static_cast<double>(buf_[static_cast<size_t>(i - base_)]) * w;
        }
        out->push_back(static_cast<float>(acc));
        ++produced_;
    }
    // Discard input no later output can use.
    const long long keep =
        static_cast<long long>(std::floor(static_cast<double>(produced_) * ratio_)) - halfWidth_ -
        1;
    if (keep > base_ + 16384) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(keep - base_));
        base_ = keep;
    }
}

void Resampler::process(const float* in, size_t n, std::vector<float>* out) {
    buf_.insert(buf_.end(), in, in + n);
    total_ += static_cast<long long>(n);
    drain(out, false);
}

void Resampler::finish(std::vector<float>* out) { drain(out, true); }

}  // namespace av
