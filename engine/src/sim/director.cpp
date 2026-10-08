#include "av/sim/director.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace av {

namespace {

constexpr float kPalettes[6][4][3] = {
    {{0.38f, 0.20f, 1.00f}, {0.00f, 0.72f, 0.95f}, {1.00f, 0.30f, 0.62f}, {0.98f, 0.82f, 0.25f}},  // neon
    {{1.00f, 0.35f, 0.10f}, {1.00f, 0.65f, 0.15f}, {0.85f, 0.15f, 0.35f}, {0.30f, 0.05f, 0.45f}},  // ember
    {{0.05f, 0.75f, 0.55f}, {0.10f, 0.45f, 0.95f}, {0.55f, 0.95f, 0.75f}, {0.85f, 0.95f, 0.30f}},  // aurora
    {{0.95f, 0.20f, 0.45f}, {0.55f, 0.15f, 0.95f}, {0.20f, 0.55f, 1.00f}, {1.00f, 0.80f, 0.90f}},  // synthwave
    {{0.90f, 0.95f, 1.00f}, {0.45f, 0.65f, 1.00f}, {0.15f, 0.30f, 0.85f}, {0.65f, 0.55f, 1.00f}},  // ice
    {{0.20f, 1.00f, 0.35f}, {0.85f, 1.00f, 0.15f}, {0.05f, 0.65f, 0.40f}, {1.00f, 0.45f, 0.10f}},  // acid
};

void setPalette(Look* l, uint32_t idx) {
    const auto& p = kPalettes[idx % 6];
    for (int c = 0; c < 4; ++c) {
        l->palette[c][0] = p[c][0];
        l->palette[c][1] = p[c][1];
        l->palette[c][2] = p[c][2];
        l->palette[c][3] = 1.f;
    }
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

Look lerpLook(const Look& a, const Look& b, float t) {
    Look o;
    o.bloom = lerp(a.bloom, b.bloom, t);
    o.chroma = lerp(a.chroma, b.chroma, t);
    o.grain = lerp(a.grain, b.grain, t);
    o.feedback = lerp(a.feedback, b.feedback, t);
    o.vignette = lerp(a.vignette, b.vignette, t);
    o.glitch = lerp(a.glitch, b.glitch, t);
    o.exposure = lerp(a.exposure, b.exposure, t);
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) o.palette[c][k] = lerp(a.palette[c][k], b.palette[c][k], t);
    return o;
}

float smooth(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

uint32_t sceneForLabel(SectionLabel label, uint32_t cluster) {
    switch (label) {
        case SectionLabel::Intro:
        case SectionLabel::Outro: return kSceneBars;
        case SectionLabel::Verse: return (cluster & 1) ? kSceneRing : kSceneBars;
        case SectionLabel::Build: return kSceneTunnel;
        case SectionLabel::Chorus: return (cluster & 1) ? kSceneFractal : kSceneTunnel;
        case SectionLabel::Drop: return kSceneFractal;
        case SectionLabel::Break: return kSceneRing;
        case SectionLabel::Bridge: return kSceneTunnel;
    }
    return kSceneBars;
}

Look lookForLabel(SectionLabel label, float energy, uint32_t paletteIdx) {
    Look l;
    setPalette(&l, paletteIdx);
    const float e = std::clamp(energy, 0.f, 1.f);
    switch (label) {
        case SectionLabel::Intro:
        case SectionLabel::Outro:
            l.bloom = 0.40f; l.vignette = 0.55f; l.grain = 0.035f; l.exposure = 0.85f; break;
        case SectionLabel::Verse:
            l.bloom = 0.45f + 0.2f * e; l.chroma = 0.0012f; break;
        case SectionLabel::Build:
            l.bloom = 0.55f + 0.3f * e; l.chroma = 0.0025f; l.feedback = 0.35f; l.glitch = 0.05f; break;
        case SectionLabel::Chorus:
            l.bloom = 0.65f + 0.3f * e; l.chroma = 0.0025f; l.exposure = 0.95f; break;
        case SectionLabel::Drop:
            l.bloom = 0.8f; l.chroma = 0.0035f; l.glitch = 0.18f; l.exposure = 1.0f; l.grain = 0.03f; break;
        case SectionLabel::Break:
            l.bloom = 0.55f; l.feedback = 0.62f; l.vignette = 0.6f; l.chroma = 0.0008f; l.exposure = 0.85f; break;
        case SectionLabel::Bridge:
            l.bloom = 0.6f; l.feedback = 0.3f; l.chroma = 0.0018f; break;
    }
    return l;
}

void Director::setTimeline(std::shared_ptr<const Timeline> t) {
    timeline_ = std::move(t);
    plan_.clear();
    if (!timeline_ || timeline_->sections.empty()) return;
    // Beat-aware transition length: two beats, clamped to a pleasant 0.6 .. 1.6 s.
    const float bpm = timeline_->bpm > 0 ? timeline_->bpm : 120.f;
    transitionSec_ = std::clamp(2.f * 60.f / bpm, 0.6f, 1.6f);

    uint32_t prevScene = ~0u;
    for (const Section& s : timeline_->sections) {
        Segment seg;
        seg.start = s.startSec;
        seg.end = s.endSec;
        seg.paletteIdx = s.cluster;
        uint32_t scene = sceneForLabel(s.label, s.cluster) % numScenes_;
        if (scene == prevScene && numScenes_ > 1) {  // avoid a no-op "transition" between different sections
            const uint32_t alt = (scene + 1 + (s.cluster % (numScenes_ - 1))) % numScenes_;
            scene = alt;
        }
        seg.scene = scene;
        seg.look = lookForLabel(s.label, s.energy, seg.paletteIdx);
        prevScene = scene;
        plan_.push_back(seg);
    }
}

int Director::segmentAt(double pos) const {
    if (plan_.empty()) return -1;
    for (size_t i = 0; i < plan_.size(); ++i)
        if (pos < plan_[i].end) return static_cast<int>(i);
    return static_cast<int>(plan_.size()) - 1;
}

void Director::autoState(double pos, VisualState* out) const {
    const int i = segmentAt(pos);
    if (i < 0) {
        out->sceneA = out->sceneB = kSceneBars % numScenes_;
        out->transition = 0.f;
        out->sceneTime = static_cast<float>(pos);
        out->look = Look{};
        return;
    }
    const Segment& cur = plan_[static_cast<size_t>(i)];
    const float since = static_cast<float>(pos) - cur.start;
    if (i > 0 && since < transitionSec_) {
        const Segment& prev = plan_[static_cast<size_t>(i) - 1];
        const float t = smooth(since / transitionSec_);
        out->sceneA = prev.scene;
        out->sceneB = cur.scene;
        out->transition = t;
        out->sceneTime = static_cast<float>(pos) - prev.start;
        out->look = lerpLook(prev.look, cur.look, t);
    } else {
        out->sceneA = out->sceneB = cur.scene;
        out->transition = 0.f;
        out->sceneTime = since;
        out->look = cur.look;
    }
}

void Director::setManualScene(int index) {
    const int next = index < 0 ? -1 : index % static_cast<int>(numScenes_);
    if (next == manual_) return;
    // Start a short blend from whatever is on screen to the new selection.
    shownA_ = shownB_;  // the incoming scene of the last blend (or the shown scene) becomes the base
    if (!initialised_) shownA_ = shownB_ = 0;
    manual_ = next;
    blending_ = true;
    blend_ = 0.f;
    manualClock_ = 0.0;
}

void Director::update(double pos, double dt, VisualState* out) {
    VisualState tmp;
    autoState(pos, &tmp);
    if (!initialised_) {
        shownA_ = shownB_ = tmp.sceneB;
        initialised_ = true;
    }
    if (manual_ < 0 && !blending_) {
        out->sceneA = tmp.sceneA;
        out->sceneB = tmp.sceneB;
        out->transition = tmp.transition;
        out->sceneTime = tmp.sceneTime;
        out->look = tmp.look;
        shownA_ = shownB_ = tmp.sceneB;
        return;
    }
    manualClock_ += dt;
    const uint32_t target = manual_ >= 0 ? static_cast<uint32_t>(manual_) : tmp.sceneB;
    if (blending_) {
        blend_ += static_cast<float>(dt) / 0.6f;
        if (blend_ >= 1.f) {
            blending_ = false;
            blend_ = 0.f;
            shownA_ = shownB_ = target;
        }
    }
    out->sceneA = blending_ ? shownA_ : target;
    out->sceneB = target;
    shownB_ = target;
    out->transition = blending_ ? smooth(blend_) : 0.f;
    out->sceneTime = static_cast<float>(manualClock_);
    out->look = tmp.look;  // the look still follows the musical structure
}

}  // namespace av
