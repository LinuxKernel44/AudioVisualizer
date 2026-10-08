#pragma once

#include <memory>
#include <vector>

#include "av/analysis/timeline.h"
#include "av/core/visual_state.h"

namespace av {

/// Built-in scene indices (order of createBuiltinScenes()).
enum SceneId : uint32_t { kSceneBars = 0, kSceneRing = 1, kSceneTunnel = 2, kSceneFractal = 3, kSceneParticles = 4, kSceneCount };

/// Chooses scene, transition and look from the musical structure. In automatic mode the output is
/// a pure function of the playback position (so it survives seeking and is reproducible for
/// offline export); manual selection is time-driven.
class Director {
public:
    explicit Director(uint32_t numScenes = kSceneCount) : numScenes_(numScenes ? numScenes : 1) {}

    void setTimeline(std::shared_ptr<const Timeline> t);
    /// -1 = automatic, otherwise the scene index to show.
    void setManualScene(int index);
    int manualScene() const { return manual_; }

    /// Writes sceneA/sceneB/transition/sceneTime/look into `out`.
    void update(double positionSec, double dtSec, VisualState* out);

    struct Segment {
        float start = 0, end = 0;
        uint32_t scene = 0;
        uint32_t paletteIdx = 0;
        Look look;
    };
    const std::vector<Segment>& plan() const { return plan_; }
    float transitionSeconds() const { return transitionSec_; }
    uint32_t numScenes() const { return numScenes_; }

private:
    void autoState(double pos, VisualState* out) const;
    int segmentAt(double pos) const;

    uint32_t numScenes_;
    std::shared_ptr<const Timeline> timeline_;
    std::vector<Segment> plan_;
    float transitionSec_ = 1.0f;

    int manual_ = -1;
    // Manual / hand-back blending (time driven).
    uint32_t shownA_ = 0;       // scene currently fully shown (or outgoing)
    uint32_t shownB_ = 0;       // incoming scene during a manual transition
    float blend_ = 0.f;
    bool blending_ = false;
    double manualClock_ = 0.0;  // seconds since the manual scene started
    bool initialised_ = false;
};

/// Overwrites the palette of `look` with built-in palette `idx` (wraps around).
void setLookPalette(Look* look, uint32_t idx);
uint32_t paletteCount();

/// Look presets used by the plan (exposed for tests and tooling).
Look lookForLabel(SectionLabel label, float energy, uint32_t paletteIdx);
uint32_t sceneForLabel(SectionLabel label, uint32_t cluster);

}  // namespace av
