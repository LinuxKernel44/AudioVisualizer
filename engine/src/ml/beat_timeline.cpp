#include "av/ml/beat_timeline.h"

#include <algorithm>
#include <cmath>

#include "av/analysis/rhythm.h"

namespace av {

namespace {
double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
}
}  // namespace

float beatAlignmentScore(const Timeline& t, const std::vector<float>& beats) {
    const auto& lo = t.ch[static_cast<size_t>(Ch::OnsetLow)];
    const auto& mi = t.ch[static_cast<size_t>(Ch::OnsetMid)];
    const auto& hi = t.ch[static_cast<size_t>(Ch::OnsetHigh)];
    const size_t n = lo.size();
    if (beats.empty() || n == 0 || mi.size() != n || hi.size() != n) return 0.f;
    std::vector<float> raw(n);
    for (size_t i = 0; i < n; ++i) raw[i] = 1.0f * lo[i] + 0.6f * mi[i] + 0.15f * hi[i];
    const std::vector<float> env = conditionOnsetEnvelope(raw, t.frameRate);
    const long tol = std::max<long>(1, std::lround(0.03 * t.frameRate));
    double sum = 0;
    size_t cnt = 0;
    for (float b : beats) {
        const long c = std::lround(static_cast<double>(b) * t.frameRate);
        if (c < 0 || c >= static_cast<long>(n)) continue;
        float best = 0.f;
        for (long k = std::max<long>(0, c - tol);
             k <= std::min<long>(static_cast<long>(n) - 1, c + tol); ++k)
            best = std::max(best, env[static_cast<size_t>(k)]);
        sum += best;
        ++cnt;
    }
    return cnt ? static_cast<float>(sum / static_cast<double>(cnt)) : 0.f;
}

bool applyMlBeats(const BeatTimes& bt, Timeline* t) {
    const size_t nb = bt.beats.size();
    if (nb < 8) return false;

    std::vector<double> ibi;
    for (size_t i = 1; i < nb; ++i) ibi.push_back(bt.beats[i] - bt.beats[i - 1]);
    const double med = medianOf(ibi);
    if (med <= 0.2 || med > 2.0) return false;  // outside 30..300 BPM: not a plausible tempo

    // Quality gate: a usable beat grid has a steady pulse. If too many inter-beat intervals stray
    // from the median the model has locked onto something else (e.g. hats or an off-beat), and the
    // classical DSP grid is the better choice for driving visuals.
    size_t steady = 0;
    for (double d : ibi)
        if (std::fabs(d - med) <= 0.15 * med) ++steady;
    if (static_cast<double>(steady) / static_cast<double>(ibi.size()) < 0.7) return false;

    // Second gate: the grid must sit on the audible pulse about as well as the DSP grid does. A
    // regular but misplaced grid (wrong phase, slightly wrong tempo) fails here.
    if (!t->beats.empty()) {
        std::vector<float> mlBeats(bt.beats.begin(), bt.beats.end());
        const float ml = beatAlignmentScore(*t, mlBeats);
        const float dsp = beatAlignmentScore(*t, t->beats);
        if (dsp > 0.f && ml < 0.6f * dsp) return false;
    }

    t->beats.assign(bt.beats.begin(), bt.beats.end());
    t->bpm = static_cast<float>(60.0 / med);
    double dev = 0;
    for (double d : ibi) dev += std::fabs(d - med);
    t->bpmConfidence = static_cast<float>(
        std::clamp(1.0 - (dev / static_cast<double>(ibi.size())) / med * 4.0, 0.0, 1.0));

    // Tempo curve: median inter-beat interval within +-4 s of each 2 s step.
    t->tempoCurveStepSec = 2.f;
    t->tempoCurve.clear();
    for (double c = 0.0; c < t->durationSec; c += t->tempoCurveStepSec) {
        std::vector<double> local;
        for (size_t i = 1; i < nb; ++i)
            if (std::fabs(bt.beats[i] - c) <= 4.0) local.push_back(bt.beats[i] - bt.beats[i - 1]);
        const double m = medianOf(local);
        t->tempoCurve.push_back(static_cast<float>(m > 0.2 ? 60.0 / m : t->bpm));
    }

    // Bar positions from the downbeats: beats-per-bar is the typical number of beats between them.
    std::vector<size_t> downIdx;
    for (double d : bt.downbeats) {
        auto it = std::lower_bound(bt.beats.begin(), bt.beats.end(), d - 1e-6);
        if (it != bt.beats.end() && std::fabs(*it - d) < 1e-3)
            downIdx.push_back(static_cast<size_t>(it - bt.beats.begin()));
    }
    t->beatBar.assign(nb, 0);
    t->beatsPerBar = 4;
    t->downbeatConfidence = 0.f;
    if (downIdx.size() >= 2) {
        std::vector<double> gaps;
        for (size_t i = 1; i < downIdx.size(); ++i)
            gaps.push_back(static_cast<double>(downIdx[i] - downIdx[i - 1]));
        const int bpb = std::clamp(static_cast<int>(std::lround(medianOf(gaps))), 2, 12);
        t->beatsPerBar = static_cast<uint32_t>(bpb);
        size_t consistent = 0;
        for (double g : gaps)
            if (static_cast<int>(g) == bpb) ++consistent;
        t->downbeatConfidence = static_cast<float>(consistent) / static_cast<float>(gaps.size());
        // Count beats after each downbeat; before the first one extrapolate backwards.
        size_t d = 0;
        for (size_t i = 0; i < nb; ++i) {
            if (d < downIdx.size() && i == downIdx[d]) {
                t->beatBar[i] = 0;
                ++d;
            } else if (d == 0) {
                const size_t back = downIdx[0] - i;
                t->beatBar[i] = static_cast<uint8_t>(
                    (static_cast<size_t>(bpb) - back % static_cast<size_t>(bpb)) %
                    static_cast<size_t>(bpb));
            } else {
                t->beatBar[i] = static_cast<uint8_t>((t->beatBar[i - 1] + 1) % bpb);
            }
        }
    } else {
        for (size_t i = 0; i < nb; ++i)
            t->beatBar[i] = static_cast<uint8_t>(i % 4);  // no downbeats: assume 4/4
    }
    t->beatSource = 1;
    return true;
}

}  // namespace av
