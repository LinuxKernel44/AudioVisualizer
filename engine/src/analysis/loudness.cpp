#include "av/analysis/loudness.h"

#include <algorithm>
#include <cmath>

namespace av {

LoudnessMeter::LoudnessMeter(double sampleRate, size_t hop)
    : sr_(sampleRate), hop_(hop), firstBlockLen_(hop - hop / 2) {
    // BS.1770 pre-filter (high shelf) and RLB high-pass, derived for any sample rate.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(M_PI * f0 / sr_);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& s : shelf_) {
            s.b0 = (Vh + Vb * K / Q + K * K) / a0;
            s.b1 = 2.0 * (K * K - Vh) / a0;
            s.b2 = (Vh - Vb * K / Q + K * K) / a0;
            s.a1 = 2.0 * (K * K - 1.0) / a0;
            s.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(M_PI * f0 / sr_);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& s : hp_) {
            s.b0 = 1.0;
            s.b1 = -2.0;
            s.b2 = 1.0;
            s.a1 = 2.0 * (K * K - 1.0) / a0;
            s.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
}

void LoudnessMeter::process(const float* x, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        double e = 0;
        for (int c = 0; c < 2; ++c) {
            const float v = x[2 * i + static_cast<size_t>(c)];
            peak_ = std::max(peak_, std::fabs(v));
            const double y = hp_[c].step(shelf_[c].step(v));
            e += y * y;
        }
        acc_ += e;
        ++inBlock_;
        const size_t len = first_ ? firstBlockLen_ : hop_;
        if (inBlock_ == len) {
            blocks_.push_back(acc_ / static_cast<double>(hop_));
            acc_ = 0;
            inBlock_ = 0;
            first_ = false;
        }
    }
}

void LoudnessMeter::finish() {
    if (inBlock_ > 0) {
        blocks_.push_back(acc_ / static_cast<double>(hop_));
        acc_ = 0;
        inBlock_ = 0;
        first_ = false;
    }
}

namespace {
double toLufs(double meanSquare) { return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1e-12)); }
}  // namespace

std::vector<float> LoudnessMeter::momentary() const {
    const size_t win = std::max<size_t>(
        1, static_cast<size_t>(std::lround(0.4 * sr_ / static_cast<double>(hop_))));
    std::vector<float> out(blocks_.size());
    double sum = 0;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        sum += blocks_[i];
        if (i >= win) sum -= blocks_[i - win];
        out[i] = static_cast<float>(toLufs(sum / static_cast<double>(std::min(i + 1, win))));
    }
    return out;
}

float LoudnessMeter::integrated() const {
    const size_t win = std::max<size_t>(
        1, static_cast<size_t>(std::lround(0.4 * sr_ / static_cast<double>(hop_))));
    const size_t stride = std::max<size_t>(1, win / 4);  // 75% overlap
    std::vector<double> gating;
    if (blocks_.size() < win) return -70.f;
    double sum = 0;
    for (size_t i = 0; i < win; ++i) sum += blocks_[i];
    for (size_t s = 0; s + win <= blocks_.size(); s += stride) {
        if (s > 0) {
            sum = 0;
            for (size_t i = s; i < s + win; ++i) sum += blocks_[i];
        }
        gating.push_back(sum / static_cast<double>(win));
    }
    double absSum = 0;
    size_t absN = 0;
    for (double g : gating)
        if (toLufs(g) > -70.0) {
            absSum += g;
            ++absN;
        }
    if (absN == 0) return -70.f;
    const double relGate = toLufs(absSum / static_cast<double>(absN)) - 10.0;
    double s2 = 0;
    size_t n2 = 0;
    for (double g : gating)
        if (toLufs(g) > -70.0 && toLufs(g) > relGate) {
            s2 += g;
            ++n2;
        }
    return n2 ? static_cast<float>(toLufs(s2 / static_cast<double>(n2))) : -70.f;
}

float LoudnessMeter::range() const {
    const size_t win = std::max<size_t>(
        1, static_cast<size_t>(std::lround(3.0 * sr_ / static_cast<double>(hop_))));
    const size_t stride =
        std::max<size_t>(1, static_cast<size_t>(std::lround(sr_ / static_cast<double>(hop_))));
    if (blocks_.size() < win) return 0.f;
    std::vector<double> st;
    for (size_t s = 0; s + win <= blocks_.size(); s += stride) {
        double sum = 0;
        for (size_t i = s; i < s + win; ++i) sum += blocks_[i];
        st.push_back(sum / static_cast<double>(win));
    }
    double absSum = 0;
    size_t absN = 0;
    for (double g : st)
        if (toLufs(g) > -70.0) {
            absSum += g;
            ++absN;
        }
    if (absN == 0) return 0.f;
    const double relGate = toLufs(absSum / static_cast<double>(absN)) - 20.0;
    std::vector<double> l;
    for (double g : st)
        if (toLufs(g) > -70.0 && toLufs(g) > relGate) l.push_back(toLufs(g));
    if (l.size() < 2) return 0.f;
    std::sort(l.begin(), l.end());
    const double lo = l[static_cast<size_t>(0.10 * static_cast<double>(l.size() - 1))];
    const double hi = l[static_cast<size_t>(0.95 * static_cast<double>(l.size() - 1))];
    return static_cast<float>(hi - lo);
}

float LoudnessMeter::samplePeakDb() const { return 20.f * std::log10(std::max(peak_, 1e-6f)); }

}  // namespace av
