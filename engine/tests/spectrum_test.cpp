#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "av/dsp/spectrum.h"

namespace {
std::vector<float> sine(double hz, double sr, size_t n, float amp = 1.f) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i)
        s[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / sr));
    return s;
}
}  // namespace

TEST(Spectrum, SinePeaksAtExpectedBin) {
    constexpr double sr = 48000.0;
    av::SpectrumAnalyzer an(2048, sr, 64);
    auto x = sine(1000.0, sr, 2048);
    std::vector<float> bars(64);
    an.analyze(x.data(), bars.data());
    const auto& mag = an.magnitudes();
    const size_t peak = std::max_element(mag.begin(), mag.end()) - mag.begin();
    EXPECT_NEAR(static_cast<double>(peak) * sr / 2048.0, 1000.0, sr / 2048.0);
    // Amplitude is ~1 for a unit sine (Hann-normalised), within scalloping loss.
    EXPECT_NEAR(mag[peak], 1.0f, 0.2f);
}

TEST(Spectrum, LoudestBarTracksFrequency) {
    constexpr double sr = 48000.0;
    av::SpectrumAnalyzer an(2048, sr, 64);
    std::vector<float> bars(64);
    size_t prev = 0;
    for (double hz : {100.0, 400.0, 1600.0, 6400.0}) {
        auto x = sine(hz, sr, 2048);
        an.analyze(x.data(), bars.data());
        const size_t loud = std::max_element(bars.begin(), bars.end()) - bars.begin();
        EXPECT_GT(loud, prev) << hz;
        prev = loud;
    }
}

TEST(Spectrum, SilenceIsZeroAndBarsBounded) {
    av::SpectrumAnalyzer an(1024, 44100.0, 32);
    std::vector<float> silence(1024, 0.f), bars(32, 1.f);
    an.analyze(silence.data(), bars.data());
    for (float b : bars) EXPECT_EQ(b, 0.f);
    auto loud = sine(500.0, 44100.0, 1024, 10.f);
    an.analyze(loud.data(), bars.data());
    for (float b : bars) {
        EXPECT_GE(b, 0.f);
        EXPECT_LE(b, 1.f);
    }
}
