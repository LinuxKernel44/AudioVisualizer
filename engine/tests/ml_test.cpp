#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include "av/analysis/timeline.h"
#include "av/ml/beat_inference.h"
#include "av/ml/beat_timeline.h"
#include "av/ml/log_mel.h"
#include "av/ml/resampler.h"

namespace {
#include "data/ref_logmel.inc"
#include "data/ref_post.inc"
}  // namespace

// ---- LogMel against the official torchaudio implementation -----------------------------------

TEST(LogMel, MatchesTorchaudioReference) {
    const double sr = 22050.0;
    std::vector<float> x(static_cast<size_t>(sr * 2));
    for (size_t n = 0; n < x.size(); ++n) {
        const double t = static_cast<double>(n);
        x[n] = static_cast<float>(0.5 * std::sin(2 * M_PI * 440.0 * t / sr) +
                                  0.25 * std::sin(2 * M_PI * 3000.0 * t / sr) +
                                  (n > 11025 ? 0.2 * std::sin(2 * M_PI * 80.0 * t / sr) : 0.0));
    }
    av::LogMel lm;
    std::vector<float> frames;
    // Feed in awkward chunk sizes: the streaming result must not depend on them.
    for (size_t i = 0; i < x.size();) {
        const size_t n = std::min<size_t>(997 + (i % 13), x.size() - i);
        lm.push(x.data() + i, n, &frames);
        i += n;
    }
    lm.finish(&frames);
    ASSERT_EQ(frames.size(), kRefFrames * 128);
    for (size_t k = 0; k < sizeof(kRefRows) / sizeof(kRefRows[0]); ++k)
        for (size_t m = 0; m < 128; ++m)
            EXPECT_NEAR(frames[kRefRows[k] * 128 + m], kRefLogMel[k][m], 2e-3f)
                << "frame " << kRefRows[k] << " mel " << m;
    double sum = 0;
    for (float v : frames) sum += v;
    EXPECT_NEAR(sum / kRefSum, 1.0, 1e-4);
}

TEST(LogMel, FrameCountAndShortInput) {
    EXPECT_EQ(av::LogMel::frameCount(441 * 10), 11u);
    av::LogMel lm;
    std::vector<float> tiny(300, 0.1f), out;
    lm.push(tiny.data(), tiny.size(), &out);
    lm.finish(&out);
    EXPECT_EQ(out.size(), av::LogMel::frameCount(300) * 128);
    for (float v : out) EXPECT_TRUE(std::isfinite(v));
}

// ---- Resampler --------------------------------------------------------------------------------

namespace {
double toneAmplitude(const std::vector<float>& y, double sr, double hz) {
    // Goertzel-style correlation over the middle half of the signal.
    const size_t a = y.size() / 4, b = y.size() * 3 / 4;
    double re = 0, im = 0;
    for (size_t i = a; i < b; ++i) {
        const double ph = 2 * M_PI * hz * static_cast<double>(i) / sr;
        re += y[i] * std::cos(ph);
        im += y[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(b - a);
}
}  // namespace

TEST(Resampler, PreservesInBandTonesAndRejectsAliases) {
    for (double inRate : {44100.0, 48000.0, 32000.0, 22050.0}) {
        const size_t n = static_cast<size_t>(inRate * 2);
        std::vector<float> x(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / inRate;
            x[i] =
                static_cast<float>(0.5 * std::sin(2 * M_PI * 1000.0 * t) +
                                   (inRate > 30000 ? 0.4 * std::sin(2 * M_PI * 15000.0 * t) : 0.0));
        }
        av::Resampler rs(inRate, 22050.0);
        std::vector<float> y;
        for (size_t i = 0; i < n; i += 3001)
            rs.process(x.data() + i, std::min<size_t>(3001, n - i), &y);
        rs.finish(&y);
        EXPECT_NEAR(static_cast<double>(y.size()), static_cast<double>(n) * 22050.0 / inRate, 4.0)
            << inRate;
        EXPECT_NEAR(toneAmplitude(y, 22050.0, 1000.0), 0.5, 0.01) << inRate;
        if (inRate > 30000)
            EXPECT_LT(toneAmplitude(y, 22050.0, 22050.0 - 15000.0), 0.01)
                << "alias at " << inRate;  // 15 kHz folds to 7.05 kHz
    }
}

TEST(Resampler, OutputTimingIsAligned) {
    // An impulse at input time t must show up at output sample t * out/in.
    const double inRate = 48000.0;
    std::vector<float> x(48000, 0.f);
    x[24000] = 1.f;
    av::Resampler rs(inRate, 22050.0);
    std::vector<float> y;
    rs.process(x.data(), x.size(), &y);
    rs.finish(&y);
    size_t peak = 0;
    for (size_t i = 0; i < y.size(); ++i)
        if (std::fabs(y[i]) > std::fabs(y[peak])) peak = i;
    EXPECT_NEAR(static_cast<double>(peak), 24000.0 * 22050.0 / 48000.0, 1.0);
}

// ---- Chunked inference -------------------------------------------------------------------------

namespace {
// A "model" that echoes the first mel value of each frame (= the frame's absolute index in our test
// input) so any misplacement or gap in the aggregation shows up as a wrong value.
av::BeatPredictor echoPredictor(size_t* maxLen, size_t* calls) {
    return [=](const float* s, size_t T, float* beat, float* down) {
        if (T > *maxLen) *maxLen = T;
        ++*calls;
        for (size_t t = 0; t < T; ++t) {
            beat[t] = s[t * 128];
            down[t] = -s[t * 128];
        }
        return true;
    };
}
}  // namespace

class ChunkTest : public ::testing::TestWithParam<size_t> {};

TEST_P(ChunkTest, AggregatesEveryFrameInPlaceWithBoundedChunks) {
    const size_t L = GetParam();
    size_t maxLen = 0, calls = 0;
    av::ChunkedBeatInference inf(echoPredictor(&maxLen, &calls));
    // Frame f carries value f+1 (so the zero padding is distinguishable) pushed in uneven pieces.
    std::vector<float> frames(L * 128, 0.f);
    for (size_t f = 0; f < L; ++f) frames[f * 128] = static_cast<float>(f + 1);
    for (size_t i = 0; i < L;) {
        const size_t n = std::min<size_t>(1 + (i * 7919) % 977, L - i);
        ASSERT_TRUE(inf.push(frames.data() + i * 128, n));
        i += n;
    }
    av::BeatActivations act;
    ASSERT_TRUE(inf.finish(&act));
    ASSERT_EQ(act.beat.size(), L);
    for (size_t f = 0; f < L; ++f) {
        ASSERT_EQ(act.beat[f], static_cast<float>(f + 1)) << "frame " << f;
        ASSERT_EQ(act.downbeat[f], -static_cast<float>(f + 1));
    }
    EXPECT_LE(maxLen, av::ChunkedBeatInference::kChunk);
    EXPECT_GE(calls, 1u);
}

INSTANTIATE_TEST_SUITE_P(Lengths, ChunkTest,
                         ::testing::Values<size_t>(1, 20, 300, 1000, 1488, 1489, 1494, 1495, 1500,
                                                   1501, 2980, 2982, 2990, 4500, 7777, 15000));

TEST(ChunkedInference, PropagatesPredictorFailure) {
    av::ChunkedBeatInference inf([](const float*, size_t, float*, float*) { return false; });
    std::vector<float> frames(3000 * 128, 0.f);
    EXPECT_FALSE(inf.push(frames.data(), 3000) && [&] {
        av::BeatActivations a;
        return inf.finish(&a);
    }());
}

// ---- Post-processing against the official minimal postprocessor --------------------------------

TEST(Postprocess, MatchesOfficialMinimalPostprocessor) {
    std::vector<float> beat(kPostBeatLogits, kPostBeatLogits + kPostT),
        down(kPostDownLogits, kPostDownLogits + kPostT);
    const auto bt = av::postprocessMinimal(beat, down);
    const size_t nb = sizeof(kPostBeats) / sizeof(kPostBeats[0]),
                 nd = sizeof(kPostDownbeats) / sizeof(kPostDownbeats[0]);
    ASSERT_EQ(bt.beats.size(), nb);
    ASSERT_EQ(bt.downbeats.size(), nd);
    for (size_t i = 0; i < nb; ++i) EXPECT_NEAR(bt.beats[i], kPostBeats[i], 1e-5);
    for (size_t i = 0; i < nd; ++i) EXPECT_NEAR(bt.downbeats[i], kPostDownbeats[i], 1e-5);
}

TEST(Postprocess, EmptyAndSilentInput) {
    const auto a = av::postprocessMinimal({}, {});
    EXPECT_TRUE(a.beats.empty());
    std::vector<float> quiet(500, -5.f);
    const auto b = av::postprocessMinimal(quiet, quiet);
    EXPECT_TRUE(b.beats.empty());
    EXPECT_TRUE(b.downbeats.empty());
}

// ---- Timeline mapping
// ----------------------------------------------------------------------------

TEST(BeatTimeline, DerivesTempoMeterAndBarPositions) {
    av::BeatTimes bt;
    for (int i = 0; i < 64; ++i) bt.beats.push_back(1.0 + 0.5 * i);  // 120 BPM
    for (int i = 2; i < 64; i += 4)
        bt.downbeats.push_back(bt.beats[static_cast<size_t>(i)]);  // downbeat at beat 2, 6, ...
    av::Timeline t;
    t.durationSec = 40;
    ASSERT_TRUE(av::applyMlBeats(bt, &t));
    EXPECT_NEAR(t.bpm, 120.f, 0.01f);
    EXPECT_EQ(t.beatsPerBar, 4u);
    EXPECT_EQ(t.beatSource, 1u);
    EXPECT_GT(t.downbeatConfidence, 0.95f);
    EXPECT_EQ(t.beatBar[2], 0);
    EXPECT_EQ(t.beatBar[3], 1);
    EXPECT_EQ(t.beatBar[6], 0);
    EXPECT_EQ(t.beatBar[1], 3);  // extrapolated backwards before the first downbeat
    EXPECT_EQ(t.beatBar[0], 2);
    EXPECT_FALSE(t.tempoCurve.empty());
    EXPECT_NEAR(t.tempoCurve[3], 120.f, 0.5f);

    // Too few beats, or an implausible tempo: leave the DSP result alone.
    av::BeatTimes few;
    few.beats = {1.0, 1.5, 2.0};
    av::Timeline untouched;
    untouched.bpm = 99.f;
    EXPECT_FALSE(av::applyMlBeats(few, &untouched));
    EXPECT_EQ(untouched.bpm, 99.f);
}

TEST(BeatTimeline, WaltzMeter) {
    av::BeatTimes bt;
    for (int i = 0; i < 60; ++i) bt.beats.push_back(0.4 * i);
    for (int i = 0; i < 60; i += 3) bt.downbeats.push_back(bt.beats[static_cast<size_t>(i)]);
    av::Timeline t;
    t.durationSec = 30;
    ASSERT_TRUE(av::applyMlBeats(bt, &t));
    EXPECT_EQ(t.beatsPerBar, 3u);
    EXPECT_NEAR(t.bpm, 150.f, 0.1f);
}

TEST(BeatTimeline, RejectsIrregularBeatGrids) {
    // The pattern the model produced on a synthetic click track: alternating short/long gaps.
    av::BeatTimes bt;
    double t0 = 0.0;
    for (int i = 0; i < 40; ++i) {
        bt.beats.push_back(t0);
        t0 += (i % 2 == 0) ? 0.24 : 0.70;
    }
    av::Timeline t;
    t.durationSec = 30;
    t.bpm = 128.f;
    EXPECT_FALSE(av::applyMlBeats(bt, &t));
    EXPECT_EQ(t.bpm, 128.f);
    EXPECT_EQ(t.beatSource, 0u);

    // A grid with a few outliers (a fill, a dropped beat) is still accepted.
    av::BeatTimes ok;
    double u = 0.0;
    for (int i = 0; i < 60; ++i) {
        ok.beats.push_back(u);
        u += (i == 17 || i == 41) ? 0.9 : 0.5;
    }
    ok.downbeats = {ok.beats[0], ok.beats[4], ok.beats[8]};
    av::Timeline t2;
    t2.durationSec = 40;
    EXPECT_TRUE(av::applyMlBeats(ok, &t2));
    EXPECT_EQ(t2.beatSource, 1u);
}

namespace {
// A timeline whose low-band onset flux pulses every 0.5 s, with a DSP grid sitting on the pulses.
av::Timeline pulseTimeline() {
    av::Timeline t;
    t.frameRate = 100;
    t.numFrames = 4000;
    t.durationSec = 40;
    for (auto& c : t.ch) c.assign(4000, 0.f);
    auto& lo = t.ch[static_cast<size_t>(av::Ch::OnsetLow)];
    for (size_t i = 50; i < 4000; i += 50) {
        lo[i] = 3.f;
        lo[i - 1] = 1.f;
    }
    for (int k = 0; k < 78; ++k) t.beats.push_back(0.5f + 0.5f * static_cast<float>(k));
    t.beatBar.assign(t.beats.size(), 0);
    t.bpm = 120.f;
    return t;
}
}  // namespace

TEST(BeatTimeline, AlignmentScorePrefersBeatsOnThePulse) {
    const av::Timeline t = pulseTimeline();
    std::vector<float> off;
    for (float b : t.beats) off.push_back(b + 0.25f);  // half a beat late
    EXPECT_GT(av::beatAlignmentScore(t, t.beats), 3.f * av::beatAlignmentScore(t, off));
}

TEST(BeatTimeline, RejectsRegularButMisplacedModelGrid) {
    av::Timeline t = pulseTimeline();
    av::BeatTimes shifted;
    for (int i = 0; i < 70; ++i)
        shifted.beats.push_back(0.75 + 0.5 * i);  // regular, but between the pulses
    EXPECT_FALSE(av::applyMlBeats(shifted, &t));
    EXPECT_EQ(t.beatSource, 0u);
    av::BeatTimes aligned;
    for (int i = 0; i < 70; ++i) aligned.beats.push_back(0.5 + 0.5 * i);
    EXPECT_TRUE(av::applyMlBeats(aligned, &t));
    EXPECT_EQ(t.beatSource, 1u);
}
