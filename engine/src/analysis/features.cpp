#include "av/analysis/features.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace av {

AnalysisConfig AnalysisConfig::forSampleRate(double sr) {
    AnalysisConfig c;
    c.sampleRate = sr;
    size_t n = 2048;
    while (static_cast<double>(n) < sr * 0.04) n <<= 1;  // ~40-90 ms window
    c.fftSize = n;
    c.hop = std::max<size_t>(1, static_cast<size_t>(std::lround(sr / 100.0)));
    return c;
}

SampleRange chunkSampleRange(const AnalysisConfig& cfg, size_t startFrame, size_t count) {
    const long long h = static_cast<long long>(cfg.hop);
    const long long ctx = static_cast<long long>(cfg.context);
    const long long half = static_cast<long long>(cfg.fftSize / 2);
    const long long first = static_cast<long long>(startFrame) - ctx;
    const long long last = static_cast<long long>(startFrame + count) + ctx;  // exclusive
    return {first * h - half, (last - 1) * h + half + 1};
}

namespace {

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

struct Filter {
    size_t firstBin = 0;
    std::vector<float> w;
};

// Type-II DCT basis for 13 cepstral coefficients.
std::vector<float> dctBasis(size_t coeffs, size_t bands) {
    std::vector<float> b(coeffs * bands);
    for (size_t k = 0; k < coeffs; ++k)
        for (size_t n = 0; n < bands; ++n)
            b[k * bands + n] = static_cast<float>(
                std::cos(M_PI * static_cast<double>(k) * (static_cast<double>(n) + 0.5) /
                         static_cast<double>(bands)));
    return b;
}

float median(std::vector<float>& v) {
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}  // namespace

struct FeatureExtractor::Impl {
    AnalysisConfig cfg;
    Stft stft;
    std::vector<Filter> mel;
    std::vector<float> melCenterHz;
    std::vector<float> dct;
    std::array<size_t, kNumBands + 1> bandBin{};
    std::vector<int> chromaOf;  // per FFT bin, -1 = unused
    std::vector<float> binLogFreq;
    size_t lowMelEnd = 0, midMelEnd = 0;  // mel band indices separating low / mid / high

    explicit Impl(const AnalysisConfig& c) : cfg(c), stft(c.fftSize) {
        const size_t bins = stft.numBins();
        const double nyq = cfg.sampleRate / 2.0;
        const double fmax = std::min(16000.0, nyq * 0.98);
        const double mLo = hzToMel(30.0), mHi = hzToMel(fmax);
        std::vector<double> edges(cfg.numMel + 2);
        for (size_t i = 0; i < edges.size(); ++i)
            edges[i] = melToHz(mLo + (mHi - mLo) * static_cast<double>(i) /
                                         static_cast<double>(cfg.numMel + 1));
        const double binHz = cfg.sampleRate / static_cast<double>(cfg.fftSize);
        mel.resize(cfg.numMel);
        melCenterHz.resize(cfg.numMel);
        for (size_t m = 0; m < cfg.numMel; ++m) {
            const double lo = edges[m], c0 = edges[m + 1], hi = edges[m + 2];
            melCenterHz[m] = static_cast<float>(c0);
            size_t b0 = static_cast<size_t>(std::floor(lo / binHz));
            size_t b1 = std::min(bins - 1, static_cast<size_t>(std::ceil(hi / binHz)));
            Filter f;
            f.firstBin = b0;
            for (size_t b = b0; b <= b1; ++b) {
                const double hz = static_cast<double>(b) * binHz;
                double w = hz < c0 ? (hz - lo) / (c0 - lo) : (hi - hz) / (hi - c0);
                f.w.push_back(static_cast<float>(std::max(0.0, w)));
            }
            // Narrow low filters can fall between bins; guarantee some weight.
            if (std::accumulate(f.w.begin(), f.w.end(), 0.f) < 1e-3f) {
                f.firstBin = std::min(bins - 1, static_cast<size_t>(std::lround(c0 / binHz)));
                f.w.assign(1, 1.f);
            }
            mel[m] = std::move(f);
        }
        dct = dctBasis(13, cfg.numMel);
        for (size_t i = 0; i < kBandEdgesHz.size(); ++i)
            bandBin[i] = std::min(bins - 1, static_cast<size_t>(std::lround(kBandEdgesHz[i] / binHz)));
        chromaOf.assign(bins, -1);
        binLogFreq.assign(bins, 0.f);
        for (size_t b = 1; b < bins; ++b) {
            const double hz = static_cast<double>(b) * binHz;
            binLogFreq[b] = static_cast<float>(std::log2(hz / 20.0) / std::log2(16000.0 / 20.0));
            if (hz >= 130.0 && hz <= 3500.0) {
                const double pc = std::fmod(std::lround(12.0 * std::log2(hz / 440.0) + 69.0), 12.0);
                chromaOf[b] = static_cast<int>(pc < 0 ? pc + 12 : pc);
            }
        }
        for (size_t m = 0; m < cfg.numMel; ++m) {
            if (melCenterHz[m] < 200.f) lowMelEnd = m + 1;
            if (melCenterHz[m] < 4000.f) midMelEnd = m + 1;
        }
    }
};

FeatureExtractor::FeatureExtractor(const AnalysisConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
FeatureExtractor::~FeatureExtractor() = default;

ChunkFeatures FeatureExtractor::process(size_t startFrame, size_t count, const float* segL,
                                        const float* segR) {
    Impl& I = *impl_;
    const AnalysisConfig& cfg = I.cfg;
    const size_t ctx = cfg.context;
    const size_t total = count + 2 * ctx;  // frames actually transformed
    const size_t N = cfg.fftSize, bins = N / 2 + 1, M = cfg.numMel;

    ChunkFeatures out;
    out.startFrame = startFrame;
    out.count = count;
    for (auto& c : out.ch) c.assign(count, 0.f);
    out.chromaSum.assign(12, 0.f);

    std::vector<float> logMel(total * M);        // log-compressed mel spectrogram
    std::vector<float> mono(N), mag(bins);
    std::vector<float> chroma(count * 12, 0.f);
    std::vector<float> cep(count * 13, 0.f);
    std::vector<float> bandDb(count * kNumBands, 0.f);

    for (size_t t = 0; t < total; ++t) {
        const float* l = segL + t * cfg.hop;
        const float* r = segR + t * cfg.hop;
        for (size_t i = 0; i < N; ++i) mono[i] = 0.5f * (l[i] + r[i]);
        I.stft.magnitude(mono.data(), mag.data());
        for (size_t m = 0; m < M; ++m) {
            const Filter& f = I.mel[m];
            float e = 0.f;
            for (size_t k = 0; k < f.w.size(); ++k) e += f.w[k] * mag[f.firstBin + k];
            logMel[t * M + m] = std::log10(1.f + 1000.f * e);
        }
        if (t < ctx || t >= ctx + count) continue;
        const size_t o = t - ctx;  // output frame index

        // ---- spectral shape, bands, chroma, cepstrum ----
        double sumMag = 0, sumMagF = 0, logSum = 0;
        size_t nb = 0;
        for (size_t k = 1; k < bins; ++k) {
            const double a = mag[k];
            sumMag += a;
            sumMagF += a * I.binLogFreq[k];
            if (a > 1e-7) {
                logSum += std::log(a);
                ++nb;
            }
            if (I.chromaOf[k] >= 0) chroma[o * 12 + static_cast<size_t>(I.chromaOf[k])] += static_cast<float>(a * a);
        }
        out.ch[static_cast<size_t>(Ch::Centroid)][o] = sumMag > 1e-9 ? static_cast<float>(sumMagF / sumMag) : 0.f;
        const double geo = nb ? std::exp(logSum / static_cast<double>(nb)) : 0.0;
        const double ari = sumMag / static_cast<double>(bins - 1);
        out.ch[static_cast<size_t>(Ch::Flatness)][o] = ari > 1e-9 ? static_cast<float>(std::min(1.0, geo / ari)) : 0.f;

        for (size_t b = 0; b < kNumBands; ++b) {
            double e = 0;
            for (size_t k = I.bandBin[b]; k < std::max(I.bandBin[b + 1], I.bandBin[b] + 1); ++k) e += mag[k] * mag[k];
            const float db = 10.f * std::log10(static_cast<float>(e) + 1e-12f);
            bandDb[o * kNumBands + b] = std::clamp((db - kBandDbFloor) / kBandDbSpan, 0.f, 1.f);
            out.ch[static_cast<size_t>(Ch::Band0) + b][o] = bandDb[o * kNumBands + b];
        }
        for (size_t k = 0; k < 13; ++k) {
            float s = 0;
            for (size_t n = 0; n < M; ++n) s += I.dct[k * M + n] * logMel[t * M + n];
            cep[o * 13 + k] = s;
        }

        // ---- time-domain: rms, stereo ----
        const size_t c0 = N / 2 - cfg.hop / 2;  // the hop samples centred on this frame
        double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
        for (size_t i = c0; i < c0 + cfg.hop; ++i) {
            const double a = l[i], b = r[i];
            ll += a * a;
            rr += b * b;
            lr += a * b;
            const double m = 0.5 * (a + b), s = 0.5 * (a - b);
            mm += m * m;
            ss += s * s;
            out.peakLin = std::max(out.peakLin, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        const double hopN = static_cast<double>(cfg.hop);
        out.sumSq += 0.5 * (ll + rr);
        const float rms = static_cast<float>(std::sqrt(0.5 * (ll + rr) / hopN));
        out.ch[static_cast<size_t>(Ch::Rms)][o] = std::clamp(std::sqrt(rms) * 1.4f, 0.f, 1.f);
        out.ch[static_cast<size_t>(Ch::Correlation)][o] =
            (ll > 1e-12 && rr > 1e-12) ? static_cast<float>(lr / std::sqrt(ll * rr)) : 1.f;
        out.ch[static_cast<size_t>(Ch::Width)][o] = (mm + ss) > 1e-12 ? static_cast<float>(ss / (mm + ss)) : 0.f;
        out.ch[static_cast<size_t>(Ch::Pan)][o] = (ll + rr) > 1e-12 ? static_cast<float>((rr - ll) / (rr + ll)) : 0.f;
    }

    // ---- onset strength: positive log-mel flux, full band and by range ----
    for (size_t o = 0; o < count; ++o) {
        const size_t t = o + ctx;
        float fl = 0, fm = 0, fh = 0;
        for (size_t m = 0; m < M; ++m) {
            const float d = std::max(0.f, logMel[t * M + m] - logMel[(t - 1) * M + m]);
            if (m < I.lowMelEnd) fl += d;
            else if (m < I.midMelEnd) fm += d;
            else fh += d;
        }
        // Per-band averages so the three ranges are comparable regardless of how many mel
        // bands each one spans (mel spacing puts most bands in the highs).
        const size_t nLow = std::max<size_t>(I.lowMelEnd, 1);
        const size_t nMid = std::max<size_t>(I.midMelEnd - I.lowMelEnd, 1);
        const size_t nHigh = std::max<size_t>(M - I.midMelEnd, 1);
        out.ch[static_cast<size_t>(Ch::OnsetLow)][o] = fl / static_cast<float>(nLow);
        out.ch[static_cast<size_t>(Ch::OnsetMid)][o] = fm / static_cast<float>(nMid);
        out.ch[static_cast<size_t>(Ch::OnsetHigh)][o] = fh / static_cast<float>(nHigh);
        out.ch[static_cast<size_t>(Ch::Onset)][o] = (fl + fm + fh) / static_cast<float>(M) * 8.f;
    }

    // ---- HPSS (median filtering on the mel spectrogram) -> percussive share per frame ----
    {
        const long hw = 4;                      // horizontal (time) half-width: harmonic
        const long vw = 4;                      // vertical (frequency) half-width: percussive
        std::vector<float> win;
        for (size_t o = 0; o < count; ++o) {
            const size_t t = o + ctx;
            double harm = 0, perc = 0;
            for (size_t m = 0; m < M; ++m) {
                win.clear();
                for (long d = -hw; d <= hw; ++d) {
                    const long tt = static_cast<long>(t) + d;
                    if (tt >= 0 && tt < static_cast<long>(total)) win.push_back(logMel[static_cast<size_t>(tt) * M + m]);
                }
                const float h = median(win);
                win.clear();
                for (long d = -vw; d <= vw; ++d) {
                    const long mm2 = static_cast<long>(m) + d;
                    if (mm2 >= 0 && mm2 < static_cast<long>(M)) win.push_back(logMel[t * M + static_cast<size_t>(mm2)]);
                }
                const float p = median(win);
                harm += h * h;
                perc += p * p;
            }
            out.ch[static_cast<size_t>(Ch::Percussive)][o] =
                (harm + perc) > 1e-12 ? static_cast<float>(perc / (harm + perc)) : 0.5f;
        }
    }

    // ---- chroma normalisation + pooled section vectors ----
    for (size_t o = 0; o < count; ++o) {
        float s = 0;
        for (size_t c = 0; c < 12; ++c) s += chroma[o * 12 + c];
        for (size_t c = 0; c < 12; ++c) {
            chroma[o * 12 + c] = s > 1e-12f ? chroma[o * 12 + c] / s : 0.f;
            out.chromaSum[c] += chroma[o * 12 + c] * std::min(1.f, s * 50.f);
        }
    }
    const size_t groups = count / cfg.groupFrames;
    out.groupVectors.assign(groups * kSectionDims, 0.f);
    for (size_t g = 0; g < groups; ++g) {
        float* v = &out.groupVectors[g * kSectionDims];
        const size_t o0 = g * cfg.groupFrames;
        for (size_t i = 0; i < cfg.groupFrames; ++i) {
            const size_t o = o0 + i;
            for (size_t c = 0; c < 12; ++c) v[c] += chroma[o * 12 + c];
            for (size_t c = 0; c < 13; ++c) v[12 + c] += cep[o * 13 + c];
            v[25] += out.ch[static_cast<size_t>(Ch::Rms)][o];
            for (size_t b = 0; b < kNumBands; ++b) v[26 + b] += bandDb[o * kNumBands + b];
        }
        const float inv = 1.f / static_cast<float>(cfg.groupFrames);
        for (size_t d = 0; d < kSectionDims; ++d) v[d] *= inv;
    }
    return out;
}

}  // namespace av
