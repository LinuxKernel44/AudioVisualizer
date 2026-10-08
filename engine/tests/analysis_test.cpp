#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "av/analysis/analyzer.h"
#include "av/analysis/rhythm.h"
#include "synth.h"

namespace {

av::Timeline analyze(const std::vector<float>& mono, double sr, size_t threads = 4) {
    av::VectorPcmSource src(synth::toStereo(mono), sr);
    av::AnalyzerOptions o;
    o.threads = threads;
    av::Timeline t;
    std::string err;
    EXPECT_TRUE(av::analyzeAudio(src, o, &t, &err)) << err;
    return t;
}

// Fraction of reference beats that have a detected beat within tolTime (phase-agnostic: we try
// every detected-beat offset by matching to the nearest reference).
double beatHitRate(const av::Timeline& t, const std::vector<double>& ref, double tol) {
    size_t hit = 0;
    for (double r : ref) {
        double best = 1e9;
        for (float b : t.beats) best = std::min(best, std::fabs(static_cast<double>(b) - r));
        if (best <= tol) ++hit;
    }
    return static_cast<double>(hit) / static_cast<double>(ref.size());
}

}  // namespace

class TempoTest : public ::testing::TestWithParam<double> {};

TEST_P(TempoTest, EstimatesBpmAndTracksBeats) {
    const double bpm = GetParam(), sr = 44100.0;
    std::vector<double> ref;
    auto x = synth::beatTrack(bpm, 40.0, sr, 4, 0.5, &ref);
    const av::Timeline t = analyze(x, sr);
    EXPECT_NEAR(t.bpm, bpm, bpm * 0.015) << "bpm";
    EXPECT_GT(t.bpmConfidence, 0.2f);
    // Ignore the first/last few beats (tracker warm-up).
    std::vector<double> mid(ref.begin() + 4, ref.end() - 4);
    EXPECT_GT(beatHitRate(t, mid, 0.045), 0.9) << "beat alignment at " << bpm << " BPM";
    ASSERT_GE(t.beats.size(), 8u);
    EXPECT_FALSE(t.onsets.empty());
}

INSTANTIATE_TEST_SUITE_P(Bpm, TempoTest, ::testing::Values(90.0, 120.0, 128.0, 140.0, 160.0));

TEST(Downbeats, FindsBarStartInFourFour) {
    const double sr = 44100.0, bpm = 120.0;
    std::vector<double> ref;
    auto x = synth::beatTrack(bpm, 48.0, sr, 4, 0.5, &ref);
    const av::Timeline t = analyze(x, sr);
    EXPECT_EQ(t.beatsPerBar, 4u);
    ASSERT_EQ(t.beats.size(), t.beatBar.size());
    // Reference downbeats are every 4th reference beat.
    size_t checked = 0, correct = 0;
    for (size_t i = 0; i < t.beats.size(); ++i) {
        const auto it = std::min_element(ref.begin(), ref.end(), [&](double a, double b) {
            return std::fabs(a - t.beats[i]) < std::fabs(b - t.beats[i]);
        });
        if (std::fabs(*it - t.beats[i]) > 0.05) continue;
        const size_t k = static_cast<size_t>(it - ref.begin());
        ++checked;
        if ((t.beatBar[i] == 0) == (k % 4 == 0)) ++correct;
    }
    ASSERT_GT(checked, 40u);
    EXPECT_GT(static_cast<double>(correct) / static_cast<double>(checked), 0.9);
}

TEST(Downbeats, DetectsWaltzTime) {
    const double sr = 44100.0;
    auto x = synth::beatTrack(120.0, 48.0, sr, 3, 0.5, nullptr);
    const av::Timeline t = analyze(x, sr);
    EXPECT_EQ(t.beatsPerBar, 3u);
}

TEST(Key, DetectsCMajorAndAMinor) {
    const double sr = 44100.0;
    auto chord = [&](std::vector<float>& x, size_t from, size_t to,
                     std::initializer_list<double> hz) {
        for (double f : hz) synth::addTone(x, sr, from, to, f, 0.08f);
    };
    const size_t seg = static_cast<size_t>(4.0 * sr);
    {  // C - F - G - C
        std::vector<float> x(seg * 6, 0.f);
        chord(x, 0, seg, {261.63, 329.63, 392.0});
        chord(x, seg, 2 * seg, {349.23, 440.0, 523.25});
        chord(x, 2 * seg, 3 * seg, {392.0, 493.88, 587.33});
        chord(x, 3 * seg, 4 * seg, {261.63, 329.63, 392.0});
        chord(x, 4 * seg, 5 * seg, {349.23, 440.0, 523.25});
        chord(x, 5 * seg, 6 * seg, {261.63, 329.63, 392.0});
        const av::Timeline t = analyze(x, sr);
        EXPECT_EQ(t.key, 0);
        EXPECT_FALSE(t.minor);
    }
    {  // Am - Dm - E - Am
        std::vector<float> x(seg * 6, 0.f);
        chord(x, 0, seg, {220.0, 261.63, 329.63});
        chord(x, seg, 2 * seg, {293.66, 349.23, 440.0});
        chord(x, 2 * seg, 3 * seg, {329.63, 415.30, 493.88});
        chord(x, 3 * seg, 4 * seg, {220.0, 261.63, 329.63});
        chord(x, 4 * seg, 5 * seg, {293.66, 349.23, 440.0});
        chord(x, 5 * seg, 6 * seg, {220.0, 261.63, 329.63});
        const av::Timeline t = analyze(x, sr);
        EXPECT_EQ(t.key, 9);
        EXPECT_TRUE(t.minor);
    }
}

TEST(Structure, FindsLoudMiddleSection) {
    const double sr = 44100.0;
    const double secs = 90.0;
    std::vector<float> x(static_cast<size_t>(secs * sr), 0.f);
    auto s = [&](double t) { return static_cast<size_t>(t * sr); };
    // A (0-30): quiet pad; B (30-60): beat + bass + brighter pad; C (60-90): quiet pad again.
    synth::addTone(x, sr, s(0), s(30), 261.63, 0.05f);
    synth::addTone(x, sr, s(0), s(30), 329.63, 0.05f);
    synth::addTone(x, sr, s(60), s(90), 261.63, 0.05f);
    synth::addTone(x, sr, s(60), s(90), 329.63, 0.05f);
    std::vector<double> ref;
    auto beat = synth::beatTrack(124.0, 30.0, sr, 4, 0.0, &ref, 1.0f);
    for (size_t i = 0; i < beat.size(); ++i) x[s(30) + i] += beat[i];
    synth::addTone(x, sr, s(30), s(60), 55.0, 0.25f);
    synth::addTone(x, sr, s(30), s(60), 392.0, 0.08f);
    synth::addTone(x, sr, s(30), s(60), 493.88, 0.08f);

    const av::Timeline t = analyze(x, sr);
    ASSERT_GE(t.sections.size(), 3u);
    auto nearBoundary = [&](double sec) {
        for (const auto& sec2 : t.sections)
            if (std::fabs(sec2.startSec - sec) < 3.0) return true;
        return false;
    };
    EXPECT_TRUE(nearBoundary(30.0));
    EXPECT_TRUE(nearBoundary(60.0));
    const av::Section* mid = t.sectionAt(45.0);
    ASSERT_NE(mid, nullptr);
    EXPECT_GT(mid->energy, t.sectionAt(10.0)->energy + 0.3f);
    EXPECT_TRUE(mid->label == av::SectionLabel::Chorus || mid->label == av::SectionLabel::Drop);
    EXPECT_EQ(t.sections.front().label, av::SectionLabel::Intro);
    // The two quiet sections are similar and should share a cluster.
    EXPECT_EQ(t.sectionAt(10.0)->cluster, t.sectionAt(75.0)->cluster);
}

TEST(Analyzer, ResultIsIndependentOfThreadCount) {
    const double sr = 44100.0;
    auto x = synth::beatTrack(128.0, 30.0, sr, 4, 0.2, nullptr);
    const av::Timeline a = analyze(x, sr, 1);
    const av::Timeline b = analyze(x, sr, 8);
    EXPECT_TRUE(a == b);
    EXPECT_EQ(a.beats, b.beats);
}

TEST(Analyzer, HandlesDegenerateInput) {
    const double sr = 48000.0;
    std::vector<float> silence(static_cast<size_t>(5 * sr), 0.f);
    const av::Timeline s = analyze(silence, sr);
    EXPECT_EQ(s.numFrames, static_cast<size_t>(5 * sr) / s.hop + 1);
    EXPECT_LE(s.integratedLufs, -69.f);
    std::vector<float> tiny(static_cast<size_t>(0.3 * sr), 0.1f);
    const av::Timeline t = analyze(tiny, sr);
    EXPECT_GT(t.numFrames, 0u);
    EXPECT_TRUE(t.beats.empty());
    av::VectorPcmSource empty({}, sr);
    av::Timeline e;
    std::string err;
    EXPECT_FALSE(av::analyzeAudio(empty, {}, &e, &err));
}

TEST(Analyzer, ReportsProgressAndSupportsCancel) {
    const double sr = 44100.0;
    auto x = synth::beatTrack(120.0, 20.0, sr, 4, 0.0, nullptr);
    {
        av::VectorPcmSource src(synth::toStereo(x), sr);
        av::AnalyzerOptions o;
        o.expectedDurationSec = 20.0;
        float last = -1;
        o.progress = [&](float p) {
            EXPECT_GE(p, last);
            last = p;
        };
        av::Timeline t;
        ASSERT_TRUE(av::analyzeAudio(src, o, &t, nullptr));
        EXPECT_FLOAT_EQ(last, 1.f);
    }
    {
        av::VectorPcmSource src(synth::toStereo(x), sr);
        std::atomic<bool> cancel{true};
        av::AnalyzerOptions o;
        o.cancel = &cancel;
        av::Timeline t;
        std::string err;
        EXPECT_FALSE(av::analyzeAudio(src, o, &t, &err));
        EXPECT_EQ(err, "cancelled");
    }
}

TEST(Timeline, SerializationRoundTrip) {
    const double sr = 44100.0;
    auto x = synth::beatTrack(120.0, 25.0, sr, 4, 0.3, nullptr);
    const av::Timeline t = analyze(x, sr);
    const auto bytes = av::serialize(t);
    av::Timeline r;
    std::string err;
    ASSERT_TRUE(av::deserialize(bytes.data(), bytes.size(), &r, &err)) << err;
    EXPECT_EQ(r.beats, t.beats);
    EXPECT_EQ(r.beatBar, t.beatBar);
    EXPECT_EQ(r.sections.size(), t.sections.size());
    EXPECT_FLOAT_EQ(r.bpm, t.bpm);
    for (size_t c = 0; c < av::kNumChannels; ++c) {
        ASSERT_EQ(r.ch[c].size(), t.ch[c].size());
        for (size_t i = 0; i < t.ch[c].size(); i += 7)
            EXPECT_NEAR(r.ch[c][i], t.ch[c][i], 2e-3f * (1.f + std::fabs(t.ch[c][i])));
    }
    // Corrupt/truncated input must be rejected, not crash.
    EXPECT_FALSE(av::deserialize(bytes.data(), bytes.size() / 2, &r, &err));
    EXPECT_FALSE(av::deserialize(bytes.data(), 3, &r, &err));
    std::vector<uint8_t> bad(bytes);
    bad[0] = 'X';
    EXPECT_FALSE(av::deserialize(bad.data(), bad.size(), &r, &err));
}

TEST(Timeline, BeatPositionLookup) {
    av::Timeline t;
    t.bpm = 120;
    t.beats = {1.0f, 1.5f, 2.0f, 2.5f};
    t.beatBar = {0, 1, 2, 3};
    const auto p = t.beatPosAt(1.75);
    EXPECT_EQ(p.beatIndex, 1);
    EXPECT_NEAR(p.phase, 0.5f, 1e-4f);
    EXPECT_EQ(p.barBeat, 1);
    EXPECT_EQ(t.beatPosAt(0.2).beatIndex, -1);
}
