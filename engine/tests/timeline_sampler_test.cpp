#include "av/sim/timeline_sampler.h"

#include <gtest/gtest.h>

TEST(TimelineSampler, BeatPulseDecaysAndDownbeatIsStronger) {
    auto t = std::make_shared<av::Timeline>();
    t->frameRate = 100;
    t->numFrames = 1000;
    for (auto& c : t->ch) c.assign(1000, 0.5f);
    t->bpm = 120;
    t->beats = {1.0f, 1.5f, 2.0f};
    t->beatBar = {0, 1, 2};
    t->sections = {{0.f, 10.f, av::SectionLabel::Verse, 0.7f, 0.f, 0}};
    av::TimelineSampler s(t);
    av::VisualState a, b, c, before;
    s.sample(1.0, &a);   // on the downbeat
    s.sample(1.25, &b);  // mid-beat
    s.sample(1.5, &c);   // on a normal beat
    s.sample(0.5, &before);
    EXPECT_NEAR(a.beatPulse, 1.0f, 1e-3f);
    EXPECT_NEAR(c.beatPulse, 0.6f, 1e-3f);
    EXPECT_LT(b.beatPulse, 0.2f);
    EXPECT_EQ(before.beatPulse, 0.f);
    EXPECT_EQ(a.barBeat, 0u);
    EXPECT_EQ(c.barBeat, 1u);
    EXPECT_NEAR(a.sectionEnergy, 0.7f, 1e-6f);
    EXPECT_TRUE(a.hasTimeline);
}

TEST(TimelineSampler, BandsAreNormalisedPerTrack) {
    auto t = std::make_shared<av::Timeline>();
    t->frameRate = 100;
    t->numFrames = 200;
    for (auto& c : t->ch) c.assign(200, 0.2f);
    auto& b3 = t->ch[static_cast<size_t>(av::Ch::Band3)];
    for (size_t i = 0; i < 200; ++i) b3[i] = 0.2f + 0.1f * static_cast<float>(i) / 199.f;
    av::TimelineSampler s(t);
    av::VisualState lo, hi;
    s.sample(0.0, &lo);
    s.sample(1.99, &hi);
    EXPECT_LT(lo.bands[3], 0.05f);
    EXPECT_GT(hi.bands[3], 0.95f);
}

#include "av/analysis/summary.h"

TEST(Summary, ProducesWellFormedJson) {
    av::Timeline t;
    t.durationSec = 12.5;
    t.bpm = 128.04f;
    t.key = 9;
    t.minor = true;
    t.beats = {0.5f, 1.0f};
    t.beatBar = {0, 1};
    t.sections = {{0.f, 12.5f, av::SectionLabel::Chorus, 0.9f, 0.f, 2}};
    t.energyCurve = {0.1f, 0.5f};
    const std::string j = av::timelineSummaryJson(t);
    EXPECT_NE(j.find("\"bpm\":128.0"), std::string::npos);
    EXPECT_NE(j.find("\"key\":\"A\""), std::string::npos);
    EXPECT_NE(j.find("\"minor\":true"), std::string::npos);
    EXPECT_NE(j.find("\"label\":\"chorus\""), std::string::npos);
    EXPECT_NE(j.find("\"downbeats\":[0.50]"), std::string::npos);
    int depth = 0;
    for (char c : j) depth += (c == '{' || c == '[') - (c == '}' || c == ']');
    EXPECT_EQ(depth, 0);
}
