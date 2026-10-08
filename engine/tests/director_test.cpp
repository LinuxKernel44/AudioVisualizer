#include "av/sim/director.h"

#include <gtest/gtest.h>

namespace {

std::shared_ptr<av::Timeline> makeTimeline() {
    auto t = std::make_shared<av::Timeline>();
    t->bpm = 120;  // transition = 2 beats = 1 s
    t->durationSec = 60;
    auto sec = [](float a, float b, av::SectionLabel l, float e, uint32_t c) {
        av::Section s;
        s.startSec = a;
        s.endSec = b;
        s.label = l;
        s.energy = e;
        s.cluster = c;
        return s;
    };
    t->sections = {sec(0, 10, av::SectionLabel::Intro, 0.2f, 0),
                   sec(10, 30, av::SectionLabel::Drop, 0.95f, 1),
                   sec(30, 40, av::SectionLabel::Break, 0.3f, 2),
                   sec(40, 60, av::SectionLabel::Drop, 0.95f, 1)};
    return t;
}

av::VisualState at(av::Director& d, double pos) {
    av::VisualState vs;
    d.update(pos, 1.0 / 60.0, &vs);
    return vs;
}

}  // namespace

TEST(Director, FollowsSectionsAndTransitionsOnBoundary) {
    av::Director d;
    d.setTimeline(makeTimeline());
    ASSERT_EQ(d.plan().size(), 4u);
    const auto intro = at(d, 5.0);
    EXPECT_EQ(intro.sceneA, av::kSceneBars);
    EXPECT_EQ(intro.transition, 0.f);

    EXPECT_EQ(at(d, 9.99).transition, 0.f);
    const auto mid = at(d, 10.5);  // half way through the 1 s transition
    EXPECT_EQ(mid.sceneA, av::kSceneBars);
    EXPECT_EQ(mid.sceneB, av::kSceneFractal);
    EXPECT_NEAR(mid.transition, 0.5f, 0.05f);

    const auto drop = at(d, 12.0);
    EXPECT_EQ(drop.sceneA, av::kSceneFractal);
    EXPECT_EQ(drop.sceneB, av::kSceneFractal);
    EXPECT_EQ(drop.transition, 0.f);
    EXPECT_GT(drop.look.glitch, 0.1f);  // drops get glitch + stronger bloom
    EXPECT_EQ(at(d, 35.0).sceneB, av::kSceneRing);
    EXPECT_GT(at(d, 35.0).look.feedback, 0.5f);  // breaks use trails
}

TEST(Director, TransitionProgressIsMonotonicThenSettles) {
    av::Director d;
    d.setTimeline(makeTimeline());
    float last = 0;
    bool sawMidTransition = false;
    for (double p = 10.0; p < 10.985; p += 0.01) {  // inside the 1 s window after the boundary
        const auto vs = at(d, p);
        EXPECT_GE(vs.transition, last - 1e-6f);
        last = vs.transition;
        sawMidTransition |= vs.transition > 0.3f && vs.transition < 0.7f;
    }
    EXPECT_TRUE(sawMidTransition);
    EXPECT_GT(last, 0.95f);
    const auto after = at(d, 11.05);  // settled: single scene again
    EXPECT_EQ(after.transition, 0.f);
    EXPECT_EQ(after.sceneA, after.sceneB);
    EXPECT_EQ(after.sceneA, av::kSceneFractal);
}

TEST(Director, IsAPureFunctionOfPositionSoSeekingWorks) {
    av::Director a, b;
    a.setTimeline(makeTimeline());
    b.setTimeline(makeTimeline());
    for (double p : {55.0, 3.0, 10.4, 41.0, 12.0}) at(a, p);  // jumpy history
    const auto x = at(a, 41.2);
    const auto y = at(b, 41.2);  // fresh director
    EXPECT_EQ(x.sceneA, y.sceneA);
    EXPECT_EQ(x.sceneB, y.sceneB);
    EXPECT_FLOAT_EQ(x.transition, y.transition);
    EXPECT_FLOAT_EQ(x.look.bloom, y.look.bloom);
}

TEST(Director, SameClusterGetsSamePaletteAndNoNoopTransitions) {
    av::Director d;
    d.setTimeline(makeTimeline());
    const auto& p = d.plan();
    EXPECT_EQ(p[1].paletteIdx, p[3].paletteIdx);
    for (size_t i = 1; i < p.size(); ++i) EXPECT_NE(p[i].scene, p[i - 1].scene);
}

TEST(Director, ManualSelectionBlendsAndCanHandBack) {
    av::Director d;
    d.setTimeline(makeTimeline());
    EXPECT_EQ(at(d, 5.0).sceneB, av::kSceneBars);
    d.setManualScene(av::kSceneTunnel);
    av::VisualState vs;
    d.update(5.0, 0.3, &vs);
    EXPECT_EQ(vs.sceneA, av::kSceneBars);
    EXPECT_EQ(vs.sceneB, av::kSceneTunnel);
    EXPECT_GT(vs.transition, 0.f);
    EXPECT_LT(vs.transition, 1.f);
    d.update(5.0, 0.5, &vs);  // blend finished
    EXPECT_EQ(vs.sceneA, av::kSceneTunnel);
    EXPECT_EQ(vs.transition, 0.f);
    d.update(25.0, 0.016, &vs);
    EXPECT_EQ(vs.sceneB, av::kSceneTunnel);  // manual wins over the plan
    d.setManualScene(-1);
    for (int i = 0; i < 60; ++i) d.update(25.0, 0.016, &vs);
    EXPECT_EQ(vs.sceneB, av::kSceneFractal);  // back to the plan (drop section)
    EXPECT_EQ(vs.transition, 0.f);
}

TEST(Director, WorksWithoutTimeline) {
    av::Director d;
    const auto vs = at(d, 3.0);
    EXPECT_EQ(vs.sceneA, av::kSceneBars);
    EXPECT_EQ(vs.transition, 0.f);
}
