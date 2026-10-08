#include "av/analysis/loudness.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
std::vector<float> stereoSine(double hz, double sr, double seconds, float amp) {
    const size_t n = static_cast<size_t>(sr * seconds);
    std::vector<float> s(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float v =
            amp * static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / sr));
        s[2 * i] = s[2 * i + 1] = v;
    }
    return s;
}
}  // namespace

// BS.1770: a 997 Hz sine at -23 dBFS in both channels measures -23 LUFS.
TEST(Loudness, ReferenceToneMatchesStandard) {
    for (double sr : {44100.0, 48000.0}) {
        const float amp = std::pow(10.f, -23.f / 20.f);
        auto x = stereoSine(997.0, sr, 10.0, amp);
        av::LoudnessMeter m(sr, static_cast<size_t>(sr / 100));
        m.process(x.data(), x.size() / 2);
        m.finish();
        EXPECT_NEAR(m.integrated(), -23.0f, 0.15f) << sr;
        EXPECT_NEAR(m.samplePeakDb(), -23.0f, 0.05f);
    }
}

TEST(Loudness, SilenceIsGatedOut) {
    std::vector<float> z(2 * 44100 * 3, 0.f);
    av::LoudnessMeter m(44100.0, 441);
    m.process(z.data(), z.size() / 2);
    m.finish();
    EXPECT_LE(m.integrated(), -69.f);
}

TEST(Loudness, TwiceTheAmplitudeIsSixDbLouder) {
    const double sr = 48000.0;
    auto a = stereoSine(1000.0, sr, 5.0, 0.1f);
    auto b = stereoSine(1000.0, sr, 5.0, 0.2f);
    av::LoudnessMeter ma(sr, 480), mb(sr, 480);
    ma.process(a.data(), a.size() / 2);
    ma.finish();
    mb.process(b.data(), b.size() / 2);
    mb.finish();
    EXPECT_NEAR(mb.integrated() - ma.integrated(), 6.02f, 0.05f);
}

TEST(Loudness, MomentaryTracksLevelChange) {
    const double sr = 44100.0;
    auto quiet = stereoSine(1000.0, sr, 3.0, 0.05f);
    auto loud = stereoSine(1000.0, sr, 3.0, 0.5f);
    quiet.insert(quiet.end(), loud.begin(), loud.end());
    av::LoudnessMeter m(sr, 441);
    m.process(quiet.data(), quiet.size() / 2);
    m.finish();
    const auto mo = m.momentary();
    EXPECT_NEAR(mo[200] - 0.0f, mo[200], 1e-3f);
    EXPECT_GT(mo[500] - mo[200], 19.0f);  // 10x amplitude = 20 dB
    EXPECT_GT(m.range(), 10.f);
}
