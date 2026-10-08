#include "av/dsp/bar_spectrum.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
std::vector<float> sine(double hz, double sr, size_t n) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / sr));
    return s;
}
size_t loudest(av::BarSpectrum& bs, double hz, double sr) {
    auto x = sine(hz, sr, bs.window());
    std::vector<float> bars(bs.numBars());
    bs.analyze(x.data(), bars.data());
    return static_cast<size_t>(std::max_element(bars.begin(), bars.end()) - bars.begin());
}
}  // namespace

TEST(BarSpectrum, BassFrequenciesMapToDistinctBars) {
    constexpr double sr = 44100.0;
    av::BarSpectrum bs(sr, 64);
    size_t prev = 0;
    for (double hz : {40.0, 55.0, 75.0, 110.0, 160.0, 240.0}) {
        const size_t bar = loudest(bs, hz, sr);
        EXPECT_GT(bar, prev) << hz << " Hz";  // strictly increasing across the bass range
        prev = bar;
    }
}

TEST(BarSpectrum, MonotonicAcrossCrossover) {
    constexpr double sr = 48000.0;
    av::BarSpectrum bs(sr, 64);
    size_t prev = 0;
    for (double hz : {100.0, 250.0, 500.0, 1000.0, 4000.0, 12000.0}) {
        const size_t bar = loudest(bs, hz, sr);
        EXPECT_GT(bar, prev) << hz << " Hz";
        prev = bar;
    }
    EXPECT_EQ(bs.numBars(), 64u);
}
