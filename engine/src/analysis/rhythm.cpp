#include "av/analysis/rhythm.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>

namespace av {

std::vector<float> conditionOnsetEnvelope(const std::vector<float>& raw, double frameRate) {
    const size_t n = raw.size();
    std::vector<float> out(n, 0.f);
    if (n == 0) return out;
    const long half = std::max<long>(1, static_cast<long>(frameRate * 0.5));
    std::vector<double> cum(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) cum[i + 1] = cum[i] + raw[i];
    double var = 0;
    for (size_t i = 0; i < n; ++i) {
        const long a = std::max<long>(0, static_cast<long>(i) - half);
        const long b = std::min<long>(static_cast<long>(n), static_cast<long>(i) + half + 1);
        const double mean = (cum[static_cast<size_t>(b)] - cum[static_cast<size_t>(a)]) / static_cast<double>(b - a);
        const float v = std::max(0.f, raw[i] - static_cast<float>(mean));
        out[i] = v;
        var += static_cast<double>(v) * v;
    }
    const float sd = static_cast<float>(std::sqrt(var / static_cast<double>(n)));
    if (sd > 1e-9f)
        for (float& v : out) v /= sd;
    return out;
}

namespace {

// Autocorrelation of env[a,b) for lags [minLag, maxLag], normalised by overlap length.
std::vector<float> acf(const std::vector<float>& env, size_t a, size_t b, size_t minLag, size_t maxLag) {
    std::vector<float> r(maxLag + 1, 0.f);
    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        if (b - a <= lag) break;
        double s = 0;
        for (size_t i = a + lag; i < b; ++i) s += static_cast<double>(env[i]) * env[i - lag];
        r[lag] = static_cast<float>(s / static_cast<double>(b - a - lag));
    }
    return r;
}

// Picks the tempo (in frames per beat, fractional) from an ACF using harmonic reinforcement and
// a log-normal prior around 120 BPM. Returns {lag, score, prominence}.
struct LagPick {
    double lag = 0;
    float score = 0;
    float prominence = 0;
};
LagPick pickLag(const std::vector<float>& r, size_t minLag, size_t maxLag, double frameRate) {
    std::vector<float> s(maxLag + 1, 0.f);
    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        float v = r[lag];
        if (lag * 2 <= maxLag * 2 && lag * 2 < r.size()) v += 0.5f * r[lag * 2];
        if (lag * 4 < r.size()) v += 0.25f * r[lag * 4];
        const double bpm = 60.0 * frameRate / static_cast<double>(lag);
        const double z = std::log2(bpm / 128.0) / 0.95;
        s[lag] = v * static_cast<float>(std::exp(-0.5 * z * z));
    }
    size_t best = minLag;
    for (size_t lag = minLag; lag <= maxLag; ++lag)
        if (s[lag] > s[best]) best = lag;
    LagPick p;
    p.score = s[best];
    p.lag = static_cast<double>(best);
    if (best > minLag && best < maxLag) {  // parabolic refinement
        const double y0 = s[best - 1], y1 = s[best], y2 = s[best + 1];
        const double den = y0 - 2 * y1 + y2;
        if (std::fabs(den) > 1e-12) p.lag += std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
    }
    double mean = 0;
    size_t cnt = 0;
    for (size_t lag = minLag; lag <= maxLag; ++lag, ++cnt) mean += s[lag];
    mean /= static_cast<double>(std::max<size_t>(cnt, 1));
    p.prominence = mean > 1e-9 ? static_cast<float>(std::min(1.0, (s[best] - mean) / (s[best] + mean + 1e-9))) : 0.f;
    return p;
}

}  // namespace

TempoResult estimateTempo(const std::vector<float>& rawEnv, double fr, float minBpm, float maxBpm) {
    TempoResult res;
    // Widen the (often 1-2 frame) onset peaks so beat periods that are not a whole number of
    // frames still correlate with themselves; otherwise exact-integer half-tempo lags win.
    std::vector<float> env(rawEnv.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float>& src = pass == 0 ? rawEnv : env;
        std::vector<float> dst(rawEnv.size());
        for (size_t i = 0; i < src.size(); ++i) {
            const float a = i > 0 ? src[i - 1] : src[i];
            const float b = i + 1 < src.size() ? src[i + 1] : src[i];
            dst[i] = 0.25f * a + 0.5f * src[i] + 0.25f * b;
        }
        env = std::move(dst);
    }
    const size_t minLag = std::max<size_t>(2, static_cast<size_t>(std::floor(fr * 60.0 / maxBpm)));
    const size_t maxLag = static_cast<size_t>(std::ceil(fr * 60.0 / minBpm));
    if (env.size() < maxLag * 4) return res;

    const std::vector<float> r = acf(env, 0, env.size(), minLag, maxLag * 4 < env.size() ? maxLag * 4 : maxLag);
    const LagPick g = pickLag(r, minLag, maxLag, fr);
    res.bpm = static_cast<float>(60.0 * fr / g.lag);
    res.confidence = g.prominence;

    // Local tempo: 8 s windows every 2 s, biased toward the global tempo to avoid octave jumps.
    const size_t win = static_cast<size_t>(8.0 * fr);
    const size_t step = static_cast<size_t>(res.stepSec * fr);
    for (size_t c = 0;; c += step) {
        const long a = static_cast<long>(c) - static_cast<long>(win / 2);
        const size_t lo = static_cast<size_t>(std::max<long>(0, a));
        const size_t hi = std::min(env.size(), lo + win);
        if (c >= env.size() || hi <= lo + maxLag * 2) {
            if (c >= env.size()) break;
            res.curve.push_back(res.curve.empty() ? res.bpm : res.curve.back());
            continue;
        }
        const std::vector<float> rl = acf(env, lo, hi, minLag, std::min(maxLag * 2, hi - lo - 1));
        // Restrict the local search to +-12% of the global tempo (follows drifts, not octaves).
        const size_t lagLo = std::max(minLag, static_cast<size_t>(g.lag * 0.88));
        const size_t lagHi = std::min(maxLag, static_cast<size_t>(std::ceil(g.lag * 1.12)));
        size_t best = lagLo;
        for (size_t lag = lagLo; lag <= lagHi && lag < rl.size(); ++lag)
            if (rl[lag] > rl[best]) best = lag;
        double lag = static_cast<double>(best);
        if (best > lagLo && best < lagHi && best + 1 < rl.size()) {
            const double y0 = rl[best - 1], y1 = rl[best], y2 = rl[best + 1];
            const double den = y0 - 2 * y1 + y2;
            if (std::fabs(den) > 1e-12) lag += std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        res.curve.push_back(static_cast<float>(60.0 * fr / lag));
    }
    return res;
}

std::vector<float> trackBeats(const std::vector<float>& env, double fr, const TempoResult& tempo,
                              float tightness) {
    std::vector<float> beats;
    const size_t n = env.size();
    if (n < 4 || tempo.bpm <= 0) return beats;

    auto periodAt = [&](size_t frame) {
        double bpm = tempo.bpm;
        if (!tempo.curve.empty()) {
            const double pos = static_cast<double>(frame) / fr / tempo.stepSec;
            const size_t i = std::min(static_cast<size_t>(pos), tempo.curve.size() - 1);
            const size_t j = std::min(i + 1, tempo.curve.size() - 1);
            const double f = std::clamp(pos - static_cast<double>(i), 0.0, 1.0);
            bpm = tempo.curve[i] * (1 - f) + tempo.curve[j] * f;
        }
        return 60.0 * fr / bpm;
    };

    std::vector<float> score(n, 0.f);
    std::vector<int> prev(n, -1);
    for (size_t t = 0; t < n; ++t) {
        const double P = periodAt(t);
        const long lo = static_cast<long>(t) - static_cast<long>(std::lround(2.0 * P));
        const long hi = static_cast<long>(t) - static_cast<long>(std::lround(0.5 * P));
        float best = 0.f;  // starting a new chain here is always allowed
        int arg = -1;
        for (long tau = std::max<long>(0, lo); tau <= hi; ++tau) {
            if (tau < 0 || static_cast<size_t>(tau) >= t) continue;
            const double d = std::log(static_cast<double>(t - static_cast<size_t>(tau)) / P);
            const float cand = score[static_cast<size_t>(tau)] - tightness * static_cast<float>(d * d);
            if (cand > best) {
                best = cand;
                arg = static_cast<int>(tau);
            }
        }
        score[t] = env[t] + best;
        prev[t] = arg;
    }
    // Start the backtrace from the best score among the last beat period.
    const size_t tailStart = n - std::min(n, static_cast<size_t>(periodAt(n - 1)));
    size_t end = tailStart;
    for (size_t t = tailStart; t < n; ++t)
        if (score[t] > score[end]) end = t;
    std::vector<size_t> frames;
    for (long t = static_cast<long>(end); t >= 0; t = prev[static_cast<size_t>(t)]) {
        frames.push_back(static_cast<size_t>(t));
        if (prev[static_cast<size_t>(t)] < 0) break;
    }
    std::reverse(frames.begin(), frames.end());
    beats.reserve(frames.size());
    for (size_t f : frames) beats.push_back(static_cast<float>(static_cast<double>(f) / fr));
    return beats;
}

DownbeatResult findDownbeats(const std::vector<float>& beats, double fr,
                             const std::vector<float>& bassEnergy,
                             const std::vector<float>& onsetLow, const std::vector<float>& onset) {
    DownbeatResult res;
    const size_t nb = beats.size();
    res.barPos.assign(nb, 0);
    if (nb < 8) return res;

    auto at = [&](const std::vector<float>& v, double sec) {
        if (v.empty()) return 0.f;
        const long i = std::clamp<long>(std::lround(sec * fr), 0, static_cast<long>(v.size()) - 1);
        return v[static_cast<size_t>(i)];
    };
    // Divides each value by the mean of its neighbourhood to remove slow level drift.
    auto localNorm = [&](std::vector<float>& a) {
        std::vector<float> sm(nb);
        for (size_t i = 0; i < nb; ++i) {
            const size_t lo = i >= 8 ? i - 8 : 0, hi = std::min(nb, i + 9);
            double m = 0;
            for (size_t k = lo; k < hi; ++k) m += a[k];
            sm[i] = static_cast<float>(m / static_cast<double>(hi - lo)) + 1e-9f;
        }
        for (size_t i = 0; i < nb; ++i) a[i] /= sm[i];
    };
    std::vector<float> bass(nb, 0.f), timing(nb, 0.f);
    for (size_t i = 0; i < nb; ++i) {
        for (int d = -3; d <= 3; ++d) {  // tolerate +-30 ms alignment error
            const double t = beats[i] + d / fr;
            bass[i] = std::max(bass[i], at(bassEnergy, t));
            timing[i] = std::max(timing[i], at(onsetLow, t) + 0.5f * at(onset, t));
        }
    }
    localNorm(bass);
    localNorm(timing);
    std::vector<float> accent(nb);
    for (size_t i = 0; i < nb; ++i) accent[i] = bass[i] + 0.3f * timing[i];

    float bestScore = -1e9f;
    uint32_t bestM = 4, bestPhase = 0;
    std::array<float, 2> meterContrast{};
    for (uint32_t mi = 0; mi < 2; ++mi) {
        const uint32_t m = mi == 0 ? 4 : 3;
        std::vector<double> sum(m, 0.0);
        std::vector<size_t> cnt(m, 0);
        for (size_t i = 0; i < nb; ++i) {
            sum[i % m] += accent[i];
            ++cnt[i % m];
        }
        std::vector<double> mean(m);
        double all = 0;
        for (uint32_t p = 0; p < m; ++p) {
            mean[p] = sum[p] / static_cast<double>(std::max<size_t>(cnt[p], 1));
            all += mean[p];
        }
        all /= m;
        uint32_t arg = 0;
        for (uint32_t p = 1; p < m; ++p)
            if (mean[p] > mean[arg]) arg = p;
        // contrast: strongest phase relative to the average of the others
        double others = 0;
        for (uint32_t p = 0; p < m; ++p)
            if (p != arg) others += mean[p];
        others /= (m - 1);
        const float contrast = static_cast<float>((mean[arg] - others) / (all + 1e-9));
        meterContrast[mi] = contrast;
        // 4/4 is the prior: 3/4 must clearly win to be selected.
        const float score = contrast - (mi == 1 ? 0.15f : 0.f);
        if (score > bestScore) {
            bestScore = score;
            bestM = m;
            bestPhase = arg;
        }
    }
    res.beatsPerBar = bestM;
    res.confidence = std::clamp(bestScore * 2.f, 0.f, 1.f);
    for (size_t i = 0; i < nb; ++i) res.barPos[i] = static_cast<uint8_t>((i + bestM - bestPhase) % bestM);
    return res;
}

std::vector<Onset> pickOnsets(const Timeline& t) {
    std::vector<Onset> out;
    const auto& full = t.ch[static_cast<size_t>(Ch::Onset)];
    const size_t n = full.size();
    if (n < 5) return out;
    const double fr = t.frameRate;
    const long w = std::max<long>(2, static_cast<long>(fr * 0.5));
    std::vector<double> cum(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) cum[i + 1] = cum[i] + full[i];
    const long minGap = std::max<long>(1, static_cast<long>(fr * 0.04));
    long last = -minGap;
    const auto& lo = t.ch[static_cast<size_t>(Ch::OnsetLow)];
    const auto& mi = t.ch[static_cast<size_t>(Ch::OnsetMid)];
    const auto& hi = t.ch[static_cast<size_t>(Ch::OnsetHigh)];
    auto rel = [&](const std::vector<float>& v, size_t i) {
        // strength of this band relative to its own neighbourhood
        const size_t a = i >= 25 ? i - 25 : 0, b = std::min(n, i + 26);
        double m = 0;
        for (size_t k = a; k < b; ++k) m += v[k];
        return v[i] / static_cast<float>(m / static_cast<double>(b - a) + 1e-6);
    };
    for (size_t i = 1; i + 1 < n; ++i) {
        if (!(full[i] >= full[i - 1] && full[i] > full[i + 1])) continue;
        const long a = std::max<long>(0, static_cast<long>(i) - w);
        const long b = std::min<long>(static_cast<long>(n), static_cast<long>(i) + w + 1);
        const double mean = (cum[static_cast<size_t>(b)] - cum[static_cast<size_t>(a)]) / static_cast<double>(b - a);
        if (full[i] < mean * 1.6 + 0.05) continue;
        if (static_cast<long>(i) - last < minGap) continue;
        uint8_t bands = 0;
        if (rel(lo, i) > 1.8f) bands |= 1;
        if (rel(mi, i) > 1.8f) bands |= 2;
        if (rel(hi, i) > 1.8f) bands |= 4;
        out.push_back({static_cast<float>(static_cast<double>(i) / fr), full[i], bands});
        last = static_cast<long>(i);
    }
    return out;
}

KeyResult estimateKey(const std::array<float, 12>& chroma) {
    static const float major[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
    static const float minor[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};
    KeyResult r;
    double cm = 0;
    for (float v : chroma) cm += v;
    cm /= 12.0;
    double cs = 0;
    for (float v : chroma) cs += (v - cm) * (v - cm);
    if (cs < 1e-12) return r;
    double best = -2, second = -2;
    for (int mode = 0; mode < 2; ++mode) {
        const float* prof = mode == 0 ? major : minor;
        double pm = 0;
        for (int i = 0; i < 12; ++i) pm += prof[i];
        pm /= 12.0;
        double ps = 0;
        for (int i = 0; i < 12; ++i) ps += (prof[i] - pm) * (prof[i] - pm);
        for (int tonic = 0; tonic < 12; ++tonic) {
            double cov = 0;
            for (int i = 0; i < 12; ++i) cov += (chroma[static_cast<size_t>((i + tonic) % 12)] - cm) * (prof[i] - pm);
            const double corr = cov / std::sqrt(cs * ps);
            if (corr > best) {
                second = best;
                best = corr;
                r.key = tonic;
                r.minor = mode == 1;
            } else if (corr > second) {
                second = corr;
            }
        }
    }
    r.confidence = static_cast<float>(std::clamp(best - second, 0.0, 1.0) * 3.0);
    r.confidence = std::min(r.confidence, 1.f);
    return r;
}

}  // namespace av
