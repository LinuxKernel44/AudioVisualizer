#include "av/sim/front_end.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

std::vector<float> sine(double hz, double sr, size_t n) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i)
        s[i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / sr));
    return s;
}

// Runs `seconds` of a sine through a FrontEnd with the given push chunk size and step interval.
av::VisualState run(double hz, double sr, double seconds, size_t chunk,
                    const av::RenderSettings& rs = {}) {
    av::FrontEnd fe(sr);
    fe.setSettings(rs);
    auto x = sine(hz, sr, static_cast<size_t>(seconds * sr));
    av::VisualState vs;
    for (size_t i = 0; i + chunk <= x.size(); i += chunk) {
        fe.pushSamples(x.data() + i, chunk);
        fe.step(static_cast<double>(chunk) / sr);
    }
    fe.fill(seconds, 0.016, true, &vs);
    return vs;
}

size_t loudest(const av::VisualState& v) {
    return static_cast<size_t>(std::max_element(v.bars, v.bars + v.numBars) - v.bars);
}

}  // namespace

TEST(FrontEnd, LoudestBarFollowsFrequency) {
    const auto lo = run(80.0, 44100.0, 1.0, 441);
    const auto hi = run(3000.0, 44100.0, 1.0, 441);
    EXPECT_LT(loudest(lo), loudest(hi));
    EXPECT_GT(lo.bars[loudest(lo)], 0.5f);
    EXPECT_EQ(lo.numBars, 64u);
}

TEST(FrontEnd, ResultsDoNotDependOnPushChunking) {
    // The exporter feeds one video frame of audio at a time; as long as the same samples are
    // present at each step the output must match. Compare 441-sample steps against 441 in
    // two 220/221 halves.
    av::FrontEnd a(44100.0), b(44100.0);
    auto x = sine(440.0, 44100.0, 44100);
    av::VisualState va, vb;
    for (size_t i = 0; i + 441 <= x.size(); i += 441) {
        a.pushSamples(x.data() + i, 441);
        b.pushSamples(x.data() + i, 220);
        b.pushSamples(x.data() + i + 220, 221);
        a.step(0.01);
        b.step(0.01);
    }
    a.fill(1.0, 0.01, true, &va);
    b.fill(1.0, 0.01, true, &vb);
    for (size_t i = 0; i < va.numBars; ++i) EXPECT_FLOAT_EQ(va.bars[i], vb.bars[i]);
}

TEST(FrontEnd, SmoothingIsTimeBasedNotStepBased) {
    // Same audio analysed at 100 Hz and 50 Hz: the decayed level after silence must be close.
    auto decayed = [](size_t hop) {
        av::FrontEnd fe(44100.0);
        auto x = sine(1000.0, 44100.0, 22050);
        std::vector<float> zero(hop, 0.f);
        for (size_t i = 0; i + hop <= x.size(); i += hop) {
            fe.pushSamples(x.data() + i, hop);
            fe.step(static_cast<double>(hop) / 44100.0);
        }
        for (int i = 0; i < static_cast<int>(0.25 * 44100.0 / static_cast<double>(hop)); ++i) {
            fe.pushSamples(zero.data(), hop);
            fe.step(static_cast<double>(hop) / 44100.0);
        }
        av::VisualState v;
        fe.fill(0.0, 0.01, true, &v);
        return *std::max_element(v.bars, v.bars + v.numBars);
    };
    EXPECT_NEAR(decayed(441), decayed(882), 0.06f);
    EXPECT_LT(decayed(441), 0.9f);  // it did decay
}

TEST(FrontEnd, SettingsChangeTheOutput) {
    av::RenderSettings quiet, loud;
    quiet.sensitivity = 0.5f;
    loud.sensitivity = 2.0f;
    const auto q = run(1000.0, 44100.0, 1.0, 441, quiet);
    const auto l = run(1000.0, 44100.0, 1.0, 441, loud);
    EXPECT_LT(q.bars[loudest(q)], l.bars[loudest(l)]);

    av::RenderSettings bass;
    bass.gainBass = 20.f;
    const auto base = run(80.0, 44100.0, 1.0, 441);
    const auto boosted = run(80.0, 44100.0, 1.0, 441, bass);
    EXPECT_GT(boosted.bars[loudest(boosted)], base.bars[loudest(base)] - 1e-6f);

    av::RenderSettings pal;
    pal.palette = 3;
    av::FrontEnd fe(44100.0);
    fe.setSettings(pal);
    av::VisualState vs;
    fe.fill(0.0, 0.016, true, &vs);
    av::Look ref;
    av::setLookPalette(&ref, 3);
    EXPECT_FLOAT_EQ(vs.look.palette[0][0], ref.palette[0][0]);
}

TEST(FrontEnd, TimelineDrivesBeatFieldsAndFlashScales) {
    auto t = std::make_shared<av::Timeline>();
    t->frameRate = 100;
    t->numFrames = 1000;
    for (auto& c : t->ch) c.assign(1000, 0.4f);
    t->bpm = 120;
    t->beats = {1.0f, 1.5f};
    t->beatBar = {0, 1};
    t->sections = {{0.f, 10.f, av::SectionLabel::Verse, 0.5f, 0.f, 0}};
    av::FrontEnd fe(44100.0);
    fe.setTimeline(t);
    av::VisualState a, b;
    fe.fill(1.0, 0.016, true, &a);
    EXPECT_TRUE(a.hasTimeline);
    EXPECT_NEAR(a.beatPulse, 1.0f, 1e-3f);
    av::RenderSettings rs;
    rs.beatFlash = 0.f;
    fe.setSettings(rs);
    fe.fill(1.0, 0.016, true, &b);
    EXPECT_EQ(b.beatPulse, 0.f);
    fe.fill(1.0, 0.016, false, &b);  // paused
    EXPECT_EQ(b.beatPulse, 0.f);
}
