#include "av/sim/front_end.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace av {

FrontEnd::FrontEnd(double sampleRate, size_t numBars)
    : sr_(sampleRate),
      numBars_(std::min<size_t>(numBars, VisualState::kMaxBars)),
      analyzer_(sampleRate, numBars_),
      history_(analyzer_.window(), 0.f),
      raw_(numBars_, 0.f),
      smooth_(numBars_, 0.f) {}

void FrontEnd::pushSamples(const float* mono, size_t n) {
    const size_t w = history_.size();
    if (n >= w) {
        std::memcpy(history_.data(), mono + (n - w), w * sizeof(float));
    } else {
        std::memmove(history_.data(), history_.data() + n, (w - n) * sizeof(float));
        std::memcpy(history_.data() + (w - n), mono, n * sizeof(float));
    }
    // Instantaneous level of what just arrived (RMS over the newest samples, at most 20 ms).
    const size_t m = std::min(n, static_cast<size_t>(sr_ * 0.02));
    double acc = 0;
    for (size_t i = w - m; i < w; ++i) acc += static_cast<double>(history_[i]) * history_[i];
    lastHopRms_ = m ? static_cast<float>(std::sqrt(acc / static_cast<double>(m))) : 0.f;
}

void FrontEnd::applyBars(const float* raw) {
    const float s = std::clamp(settings_.sensitivity, 0.1f, 8.f);
    for (size_t b = 0; b < numBars_; ++b) {
        const float pos = static_cast<float>(b) / static_cast<float>(numBars_);
        const float gainDb = pos < 0.33f ? settings_.gainBass : pos < 0.66f ? settings_.gainMid : settings_.gainTreble;
        // The raw bars map -80..0 dB onto 0..1, so a dB offset is a shift of gainDb/80.
        raw_[b] = std::clamp((raw[b] + gainDb / 80.f) * s, 0.f, 1.f);
    }
}

void FrontEnd::step(double dtSec) {
    std::vector<float> bars(numBars_);
    analyzer_.analyze(history_.data(), bars.data());
    applyBars(bars.data());
    // Time-based attack/release (the old per-hop constants correspond to ~9 ms / ~95 ms at 100 Hz).
    const float attackTau = 0.0095f;
    const float releaseTau = 0.03f + 0.22f * std::clamp(settings_.smoothing, 0.f, 1.f);
    const float ka = 1.f - std::exp(-static_cast<float>(dtSec) / attackTau);
    const float kr = std::exp(-static_cast<float>(dtSec) / releaseTau);
    for (size_t b = 0; b < numBars_; ++b) {
        if (raw_[b] > smooth_[b]) smooth_[b] += (raw_[b] - smooth_[b]) * ka;
        else smooth_[b] *= kr;
    }
    const float inst = lastHopRms_;
    rms_ += (inst - rms_) * (inst > rms_ ? ka : 1.f - kr);
}

void FrontEnd::decay(double dtSec) {
    const float kr = std::exp(-static_cast<float>(dtSec) / (0.03f + 0.22f * std::clamp(settings_.smoothing, 0.f, 1.f)));
    for (float& v : smooth_) v *= kr;
    rms_ *= kr;
    std::fill(history_.begin(), history_.end(), 0.f);
}

void FrontEnd::setTimeline(std::shared_ptr<const Timeline> t) {
    director_.setTimeline(t);
    sampler_ = t ? std::make_unique<TimelineSampler>(std::move(t)) : nullptr;
}

void FrontEnd::fill(double pos, double dt, bool playing, VisualState* out) {
    out->numBars = static_cast<uint32_t>(numBars_);
    std::memcpy(out->bars, smooth_.data(), numBars_ * sizeof(float));
    out->rms = rms_;

    // Waveform for oscilloscope scenes: peak-decimate the newest 2048 samples to 256.
    const size_t block = 2048 / VisualState::kWaveSamples;
    const float* w = history_.data() + history_.size() - 2048;
    for (size_t i = 0; i < VisualState::kWaveSamples; ++i) {
        float peak = 0.f;
        for (size_t k = 0; k < block; ++k)
            if (std::fabs(w[i * block + k]) > std::fabs(peak)) peak = w[i * block + k];
        out->wave[i] = peak;
    }

    if (sampler_) {
        sampler_->sample(pos, out);
        if (!playing) {
            out->beatPulse = 0.f;
            out->kick = 0.f;
        }
        out->beatPulse *= settings_.beatFlash;
        out->kick *= settings_.beatFlash;
    } else {
        out->hasTimeline = false;
        out->beatPulse = out->kick = out->beatPhase = out->sectionEnergy = out->bpm = 0.f;
        out->barBeat = 0;
        std::fill(std::begin(out->bands), std::end(out->bands), 0.f);
    }

    director_.update(pos, dt, out);
    if (settings_.palette >= 0) setLookPalette(&out->look, static_cast<uint32_t>(settings_.palette));
    out->look.bloom *= settings_.bloom;
}

}  // namespace av
