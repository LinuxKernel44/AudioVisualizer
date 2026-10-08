#include "av/analysis/sections.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace av {

namespace {

// z-scores each dimension, weights feature families, then L2-normalises each row so a dot
// product is a cosine similarity.
std::vector<float> normalise(const std::vector<float>& v, size_t dims, size_t groups) {
    std::vector<float> out(v.size());
    std::vector<double> mean(dims, 0.0), sd(dims, 0.0);
    for (size_t g = 0; g < groups; ++g)
        for (size_t d = 0; d < dims; ++d) mean[d] += v[g * dims + d];
    for (auto& m : mean) m /= static_cast<double>(groups);
    for (size_t g = 0; g < groups; ++g)
        for (size_t d = 0; d < dims; ++d) {
            const double x = v[g * dims + d] - mean[d];
            sd[d] += x * x;
        }
    for (auto& s : sd) s = std::sqrt(s / static_cast<double>(groups)) + 1e-6;
    // Family weights: chroma (harmony), cepstrum (timbre), loudness + bands (energy).
    for (size_t g = 0; g < groups; ++g) {
        double n2 = 0;
        for (size_t d = 0; d < dims; ++d) {
            double w = 1.0 / std::sqrt(12.0);
            if (d >= 12 && d < 25) w = 1.0 / std::sqrt(13.0);
            if (d >= 25) w = 1.6 / std::sqrt(static_cast<double>(dims - 25));
            const double x = (v[g * dims + d] - mean[d]) / sd[d] * w;
            out[g * dims + d] = static_cast<float>(x);
            n2 += x * x;
        }
        const float inv = n2 > 1e-12 ? static_cast<float>(1.0 / std::sqrt(n2)) : 0.f;
        for (size_t d = 0; d < dims; ++d) out[g * dims + d] *= inv;
    }
    return out;
}

float dot(const float* a, const float* b, size_t n) {
    float s = 0;
    for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

}  // namespace

std::vector<float> noveltyCurve(const std::vector<float>& x, size_t dims, size_t groups, size_t L) {
    std::vector<float> nov(groups, 0.f);
    if (groups < 2 * L + 1) return nov;
    // Gaussian-tapered checkerboard kernel over offsets -L..L-1 (excluding the diagonal bias).
    std::vector<float> kern(2 * L);
    for (size_t i = 0; i < 2 * L; ++i) {
        const double a =
            (static_cast<double>(i) - static_cast<double>(L) + 0.5) / static_cast<double>(L);
        kern[i] = static_cast<float>(std::exp(-4.0 * a * a));
    }
    for (size_t i = L; i + L <= groups; ++i) {
        double s = 0;
        for (size_t a = 0; a < 2 * L; ++a) {
            const float* va = &x[(i - L + a) * dims];
            for (size_t b = 0; b < 2 * L; ++b) {
                const bool sameSide = (a < L) == (b < L);
                const float sim = dot(va, &x[(i - L + b) * dims], dims);
                s += (sameSide ? 1.0 : -1.0) * kern[a] * kern[b] * sim;
            }
        }
        nov[i] = static_cast<float>(std::max(0.0, s));
    }
    return nov;
}

std::vector<Section> segmentStructure(const SectionInput& in) {
    std::vector<Section> out;
    const size_t G = in.groups, dims = in.dims;
    if (G == 0 || !in.vectors || in.durationSec <= 0) return out;
    auto whole = [&] {
        Section s;
        s.startSec = 0;
        s.endSec = static_cast<float>(in.durationSec);
        s.label = SectionLabel::Verse;
        s.energy = 0.5f;
        out.push_back(s);
        return out;
    };
    const size_t L = std::max<size_t>(
        4, static_cast<size_t>(std::lround(6.0 / in.groupSec)));  // 6 s half kernel
    if (G < 4 * L) return whole();

    const std::vector<float> x = normalise(*in.vectors, dims, G);
    std::vector<float> nov = noveltyCurve(x, dims, G, L);

    // Peak picking on the novelty curve.
    double novMean = 0, sd = 0;
    size_t cnt = 0;
    for (size_t i = L; i + L <= G; ++i, ++cnt) novMean += nov[i];
    novMean /= static_cast<double>(std::max<size_t>(cnt, 1));
    for (size_t i = L; i + L <= G; ++i) sd += (nov[i] - novMean) * (nov[i] - novMean);
    sd = std::sqrt(sd / static_cast<double>(std::max<size_t>(cnt, 1)));
    const float maxNov = *std::max_element(nov.begin(), nov.end());
    const float thresh = static_cast<float>(std::max(novMean + 1.0 * sd, 0.25 * maxNov));
    const size_t minGap = static_cast<size_t>(std::lround(8.0 / in.groupSec));

    struct Peak {
        size_t g;
        float v;
    };
    std::vector<Peak> peaks;
    for (size_t i = L; i + L <= G; ++i) {
        if (nov[i] < thresh) continue;
        bool isMax = true;
        const size_t a = i >= minGap / 2 ? i - minGap / 2 : 0, b = std::min(G, i + minGap / 2 + 1);
        for (size_t k = a; k < b; ++k)
            if (nov[k] > nov[i] || (nov[k] == nov[i] && k < i)) isMax = false;
        if (isMax) peaks.push_back({i, nov[i]});
    }

    // Boundaries (seconds); snap to the nearest beat within 1 s.
    std::vector<std::pair<float, float>> bounds;  // time, novelty
    bounds.push_back({0.f, 0.f});
    for (const auto& p : peaks) {
        float t = static_cast<float>(static_cast<double>(p.g) * in.groupSec);
        if (in.beats && !in.beats->empty()) {
            auto it = std::lower_bound(in.beats->begin(), in.beats->end(), t);
            float best = t, bd = 1.0f;
            for (auto c : {it, it == in.beats->begin() ? it : it - 1}) {
                if (c == in.beats->end()) continue;
                if (std::fabs(*c - t) < bd) {
                    bd = std::fabs(*c - t);
                    best = *c;
                }
            }
            t = best;
        }
        if (t - bounds.back().first >= 6.f && in.durationSec - t >= 6.0)
            bounds.push_back({t, p.v / std::max(maxNov, 1e-9f)});
    }
    bounds.push_back({static_cast<float>(in.durationSec), 0.f});
    if (bounds.size() < 3) return whole();

    // Per-section descriptors.
    const size_t S = bounds.size() - 1;
    std::vector<float> loud(G, 0.f);
    if (in.loudness && in.loudness->size() >= G)
        loud.assign(in.loudness->begin(), in.loudness->begin() + static_cast<long>(G));
    std::vector<float> sortedLoud(loud);
    std::sort(sortedLoud.begin(), sortedLoud.end());
    const float lo = sortedLoud[static_cast<size_t>(0.10 * static_cast<double>(G - 1))];
    const float hi = sortedLoud[static_cast<size_t>(0.95 * static_cast<double>(G - 1))];
    auto norm = [&](float l) {
        return hi > lo ? std::clamp((l - lo) / (hi - lo), 0.f, 1.f) : 0.5f;
    };

    std::vector<std::vector<float>> mean(S, std::vector<float>(dims, 0.f));
    std::vector<float> energy(S, 0.f), slope(S, 0.f);
    for (size_t s = 0; s < S; ++s) {
        const size_t g0 = std::min(
            G - 1, static_cast<size_t>(static_cast<double>(bounds[s].first) / in.groupSec));
        const size_t g1 = std::clamp(
            static_cast<size_t>(static_cast<double>(bounds[s + 1].first) / in.groupSec), g0 + 1, G);
        double e = 0;
        for (size_t g = g0; g < g1; ++g) {
            for (size_t d = 0; d < dims; ++d) mean[s][d] += x[g * dims + d];
            e += norm(loud[g]);
        }
        const float inv = 1.f / static_cast<float>(g1 - g0);
        for (auto& m : mean[s]) m *= inv;
        energy[s] = static_cast<float>(e) * inv;
        // loudness trend over the section (second half minus first half)
        const size_t mid = (g0 + g1) / 2;
        double a = 0, b = 0;
        for (size_t g = g0; g < mid; ++g) a += norm(loud[g]);
        for (size_t g = mid; g < g1; ++g) b += norm(loud[g]);
        if (mid > g0 && g1 > mid)
            slope[s] = static_cast<float>(b / static_cast<double>(g1 - mid) -
                                          a / static_cast<double>(mid - g0));
    }

    // Greedy clustering by cosine similarity of section means.
    std::vector<uint32_t> cluster(S, 0);
    std::vector<std::vector<float>> centroids;
    for (size_t s = 0; s < S; ++s) {
        double n2 = 0;
        for (float v : mean[s]) n2 += v * v;
        const float inv = n2 > 1e-12 ? static_cast<float>(1.0 / std::sqrt(n2)) : 0.f;
        std::vector<float> u(mean[s]);
        for (auto& v : u) v *= inv;
        int best = -1;
        float bs = 0.8f;
        for (size_t c = 0; c < centroids.size(); ++c) {
            const float sim = dot(u.data(), centroids[c].data(), dims);
            if (sim > bs) {
                bs = sim;
                best = static_cast<int>(c);
            }
        }
        if (best < 0) {
            centroids.push_back(u);
            best = static_cast<int>(centroids.size() - 1);
        }
        cluster[s] = static_cast<uint32_t>(best);
    }

    // Heuristic labels.
    std::vector<float> sortedE(energy);
    std::sort(sortedE.begin(), sortedE.end());
    const float medE = sortedE[sortedE.size() / 2];
    out.resize(S);
    for (size_t s = 0; s < S; ++s) {
        Section& sec = out[s];
        sec.startSec = bounds[s].first;
        sec.endSec = bounds[s + 1].first;
        sec.energy = energy[s];
        sec.novelty = bounds[s].second;
        sec.cluster = cluster[s];
        const float prevE = s > 0 ? energy[s - 1] : energy[s];
        const float nextE = s + 1 < S ? energy[s + 1] : energy[s];
        SectionLabel l = SectionLabel::Verse;
        if (s == 0 && energy[s] <= medE + 0.05f)
            l = SectionLabel::Intro;
        else if (s + 1 == S && energy[s] < prevE)
            l = SectionLabel::Outro;
        else if (energy[s] >= std::max(0.65f, medE + 0.1f)) {
            l = (energy[s] - prevE > 0.25f) ? SectionLabel::Drop : SectionLabel::Chorus;
        } else if (slope[s] > 0.2f && nextE > energy[s] + 0.1f)
            l = SectionLabel::Build;
        else if (energy[s] < 0.4f && prevE > energy[s] + 0.2f)
            l = SectionLabel::Break;
        sec.label = l;
    }
    // A mid-track section whose cluster appears only once, with medium energy, reads as a bridge.
    for (size_t s = 1; s + 1 < S; ++s) {
        if (out[s].label != SectionLabel::Verse) continue;
        const bool unique = std::count(cluster.begin(), cluster.end(), cluster[s]) == 1;
        if (unique && s > S / 2) out[s].label = SectionLabel::Bridge;
    }
    return out;
}

}  // namespace av
