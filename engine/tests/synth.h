#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace synth {

struct Rng {
    uint32_t s = 12345;
    float next() {  // deterministic noise in [-1, 1]
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.f;
    }
};

inline void addKick(std::vector<float>& x, double sr, size_t at, float amp) {
    const size_t n = static_cast<size_t>(0.22 * sr);
    for (size_t i = 0; i < n && at + i < x.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        x[at + i] += amp * static_cast<float>(std::sin(2.0 * M_PI * (48.0 * t + 14.0 * (1.0 - std::exp(-t * 40.0)) / 40.0 * 8.0)) *
                                              std::exp(-t * 14.0));
    }
}
inline void addHat(std::vector<float>& x, double sr, size_t at, float amp, Rng& rng) {
    const size_t n = static_cast<size_t>(0.04 * sr);
    float prev = 0;
    for (size_t i = 0; i < n && at + i < x.size(); ++i) {
        const float w = rng.next();
        const float hp = w - prev;  // crude high-pass
        prev = w;
        x[at + i] += amp * hp * static_cast<float>(std::exp(-static_cast<double>(i) / sr * 110.0));
    }
}
inline void addTone(std::vector<float>& x, double sr, size_t from, size_t to, double hz, float amp) {
    for (size_t i = from; i < to && i < x.size(); ++i) {
        const double t = static_cast<double>(i) / sr;
        x[i] += amp * static_cast<float>(std::sin(2.0 * M_PI * hz * t) + 0.4 * std::sin(4.0 * M_PI * hz * t) +
                                         0.2 * std::sin(6.0 * M_PI * hz * t));
    }
}
inline std::vector<float> toStereo(const std::vector<float>& m) {
    std::vector<float> s(m.size() * 2);
    for (size_t i = 0; i < m.size(); ++i) s[2 * i] = s[2 * i + 1] = m[i];
    return s;
}

/// Four-on-the-floor pattern: kick on every beat (louder on beat 1 of each bar), hats on the
/// off-beats. `beatsPerBar` 4 or 3. Returns mono audio and fills `beatTimes`.
inline std::vector<float> beatTrack(double bpm, double seconds, double sr, int beatsPerBar,
                                    double startSec, std::vector<double>* beatTimes, float level = 1.f) {
    std::vector<float> x(static_cast<size_t>(seconds * sr), 0.f);
    Rng rng;
    const double beat = 60.0 / bpm;
    int i = 0;
    for (double t = startSec; t < seconds - 0.3; t += beat, ++i) {
        const bool down = i % beatsPerBar == 0;
        addKick(x, sr, static_cast<size_t>(t * sr), (down ? 0.9f : 0.5f) * level);
        addHat(x, sr, static_cast<size_t>((t + beat / 2) * sr), 0.25f * level, rng);
        if (beatTimes) beatTimes->push_back(t);
    }
    return x;
}

}  // namespace synth
