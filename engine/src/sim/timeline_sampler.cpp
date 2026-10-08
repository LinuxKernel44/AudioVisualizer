#include "av/sim/timeline_sampler.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace av {

TimelineSampler::TimelineSampler(std::shared_ptr<const Timeline> t) : t_(std::move(t)) {
    for (size_t b = 0; b < kNumBands; ++b) {
        const auto& c = t_->ch[static_cast<size_t>(Ch::Band0) + b];
        if (c.empty()) {
            lo_[b] = 0.f;
            hi_[b] = 1.f;
            continue;
        }
        std::vector<float> s(c);
        std::sort(s.begin(), s.end());
        lo_[b] = s[static_cast<size_t>(0.05 * static_cast<double>(s.size() - 1))];
        hi_[b] = std::max(s[static_cast<size_t>(0.98 * static_cast<double>(s.size() - 1))],
                          lo_[b] + 1e-3f);
    }
}

void TimelineSampler::sample(double sec, VisualState* out) const {
    const Timeline& t = *t_;
    out->hasTimeline = true;
    out->bpm = t.bpm;
    const Timeline::BeatPos p = t.beatPosAt(sec);
    out->beatPhase = p.phase;
    out->barBeat = static_cast<uint32_t>(p.barBeat);
    if (p.beatIndex >= 0) {
        const float accent = p.barBeat == 0 ? 1.0f : 0.6f;
        const float decay = std::pow(1.f - p.phase, 3.f);
        out->beatPulse = accent * decay;
    } else {
        out->beatPulse = 0.f;
    }
    out->kick = std::clamp(t.valueAt(Ch::OnsetLow, sec) * 0.5f, 0.f, 1.f);
    const Section* s = t.sectionAt(sec);
    out->sectionEnergy = s ? s->energy : 0.5f;
    for (size_t b = 0; b < kNumBands; ++b) {
        const float v = t.valueAt(static_cast<Ch>(static_cast<size_t>(Ch::Band0) + b), sec);
        out->bands[b] = std::clamp((v - lo_[b]) / (hi_[b] - lo_[b]), 0.f, 1.f);
    }
}

}  // namespace av
