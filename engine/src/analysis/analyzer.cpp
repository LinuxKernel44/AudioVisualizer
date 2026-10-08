#include "av/analysis/analyzer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "av/analysis/features.h"
#include "av/analysis/loudness.h"
#include "av/analysis/rhythm.h"
#include "av/analysis/sections.h"
#include "av/core/thread_pool.h"

namespace av {

namespace {

void normaliseP95(std::vector<float>& v) {
    if (v.empty()) return;
    std::vector<float> s(v);
    const size_t k = static_cast<size_t>(0.95 * static_cast<double>(s.size() - 1));
    std::nth_element(s.begin(), s.begin() + static_cast<long>(k), s.end());
    const float p = s[k];
    if (p > 1e-9f)
        for (float& x : v) x /= p;
}

}  // namespace

bool analyzeAudio(PcmSource& src, const AnalyzerOptions& opts, Timeline* out, std::string* error) {
    const double sr = src.sampleRate();
    if (sr < 8000 || sr > 192000) {
        if (error) *error = "unsupported sample rate";
        return false;
    }
    const AnalysisConfig cfg = AnalysisConfig::forSampleRate(sr);
    const size_t threads = opts.threads ? opts.threads : std::max(1u, std::thread::hardware_concurrency());
    ThreadPool pool(threads);
    LoudnessMeter loud(sr, cfg.hop);

    // Sample buffers: absolute sample index of buf[0] is `base`.
    std::vector<float> bufL, bufR;
    long long base = 0;
    size_t totalSamples = 0;

    std::mutex mu;
    std::condition_variable cv;
    size_t inFlight = 0;
    const size_t maxInFlight = threads * 2 + 2;
    std::map<size_t, ChunkFeatures> results;  // keyed by start frame
    size_t nextFrame = 0;

    auto submit = [&](size_t start, size_t count, bool finalChunk) {
        const SampleRange r = chunkSampleRange(cfg, start, count);
        const size_t len = static_cast<size_t>(r.end - r.begin);
        std::vector<float> L(len, 0.f), R(len, 0.f);
        for (long long s = std::max(r.begin, base); s < std::min<long long>(r.end, base + static_cast<long long>(bufL.size())); ++s) {
            L[static_cast<size_t>(s - r.begin)] = bufL[static_cast<size_t>(s - base)];
            R[static_cast<size_t>(s - r.begin)] = bufR[static_cast<size_t>(s - base)];
        }
        (void)finalChunk;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return inFlight < maxInFlight; });
            ++inFlight;
        }
        pool.submit([&, start, count, L = std::move(L), R = std::move(R)] {
            FeatureExtractor fx(cfg);
            ChunkFeatures cf = fx.process(start, count, L.data(), R.data());
            std::lock_guard<std::mutex> lk(mu);
            results.emplace(start, std::move(cf));
            --inFlight;
            cv.notify_all();
        });
    };
    auto dropBefore = [&](long long keepFrom) {
        if (keepFrom > base) {
            const size_t drop = static_cast<size_t>(std::min<long long>(keepFrom - base, static_cast<long long>(bufL.size())));
            bufL.erase(bufL.begin(), bufL.begin() + static_cast<long>(drop));
            bufR.erase(bufR.begin(), bufR.begin() + static_cast<long>(drop));
            base += static_cast<long long>(drop);
        }
    };

    std::vector<float> block(2 * 16384);
    for (;;) {
        if (opts.cancel && opts.cancel->load()) {
            pool.waitIdle();
            if (error) *error = "cancelled";
            return false;
        }
        const size_t n = src.read(block.data(), 16384);
        if (n == 0) break;
        loud.process(block.data(), n);
        for (size_t i = 0; i < n; ++i) {
            bufL.push_back(block[2 * i]);
            bufR.push_back(block[2 * i + 1]);
        }
        totalSamples += n;
        // Submit every chunk whose samples are fully buffered.
        for (;;) {
            const SampleRange r = chunkSampleRange(cfg, nextFrame, cfg.chunkFrames);
            if (r.end > base + static_cast<long long>(bufL.size())) break;
            submit(nextFrame, cfg.chunkFrames, false);
            nextFrame += cfg.chunkFrames;
            dropBefore(chunkSampleRange(cfg, nextFrame, cfg.chunkFrames).begin);
        }
        if (opts.progress && opts.expectedDurationSec > 0)
            opts.progress(static_cast<float>(std::min(0.95, static_cast<double>(totalSamples) / sr / opts.expectedDurationSec)));
    }
    loud.finish();
    if (totalSamples == 0) {
        if (error) *error = "no audio";
        return false;
    }
    const size_t F = cfg.framesFor(totalSamples);
    while (nextFrame < F) {
        const size_t count = std::min(cfg.chunkFrames, F - nextFrame);
        submit(nextFrame, count, true);
        nextFrame += count;
    }
    pool.waitIdle();

    // ---- assemble -------------------------------------------------------------------------
    Timeline t;
    t.sampleRate = sr;
    t.hop = static_cast<uint32_t>(cfg.hop);
    t.frameRate = cfg.frameRate();
    t.numFrames = F;
    t.durationSec = static_cast<double>(totalSamples) / sr;
    for (auto& c : t.ch) c.reserve(F);
    std::vector<float> groupVecs;
    std::array<double, 12> chroma{};
    float peakLin = 0.f;
    double sumSq = 0;
    for (auto& kv : results) {
        const ChunkFeatures& c = kv.second;
        for (size_t i = 0; i < kNumChannels; ++i) t.ch[i].insert(t.ch[i].end(), c.ch[i].begin(), c.ch[i].end());
        groupVecs.insert(groupVecs.end(), c.groupVectors.begin(), c.groupVectors.end());
        for (size_t k = 0; k < 12; ++k) chroma[k] += c.chromaSum[k];
        peakLin = std::max(peakLin, c.peakLin);
        sumSq += c.sumSq;
    }
    results.clear();

    // Loudness channel (sequential stage), aligned to F frames.
    std::vector<float> mom = loud.momentary();
    mom.resize(F, mom.empty() ? -70.f : mom.back());
    t.ch[static_cast<size_t>(Ch::Loudness)] = mom;
    t.integratedLufs = loud.integrated();
    t.loudnessRange = loud.range();
    t.peakDb = loud.samplePeakDb();
    const double meanSq = sumSq / static_cast<double>(totalSamples);
    t.crestDb = static_cast<float>(20.0 * std::log10(std::max(peakLin, 1e-6f)) - 10.0 * std::log10(std::max(meanSq, 1e-12)));

    // Onset channels. Beat tracking follows the low/mid-weighted flux (what people tap to);
    // hats and cymbals contribute less so off-beat hats do not capture the beat phase.
    std::vector<float> rawOnset(F, 0.f);
    {
        const auto& lo = t.ch[static_cast<size_t>(Ch::OnsetLow)];
        const auto& mi = t.ch[static_cast<size_t>(Ch::OnsetMid)];
        const auto& hi = t.ch[static_cast<size_t>(Ch::OnsetHigh)];
        for (size_t i = 0; i < F; ++i) rawOnset[i] = 1.0f * lo[i] + 0.6f * mi[i] + 0.15f * hi[i];
    }
    for (Ch c : {Ch::Onset, Ch::OnsetLow, Ch::OnsetMid, Ch::OnsetHigh}) normaliseP95(t.ch[static_cast<size_t>(c)]);

    // Tonality.
    double cs = 0;
    for (double v : chroma) cs += v;
    for (size_t k = 0; k < 12; ++k) t.chromaMean[k] = cs > 0 ? static_cast<float>(chroma[k] / cs) : 0.f;
    const KeyResult kr = estimateKey(t.chromaMean);
    t.key = kr.key;
    t.minor = kr.minor;
    t.keyConfidence = kr.confidence;

    // Rhythm.
    const std::vector<float> env = conditionOnsetEnvelope(rawOnset, t.frameRate);
    const TempoResult tempo = estimateTempo(env, t.frameRate);
    t.bpm = tempo.bpm;
    t.bpmConfidence = tempo.confidence;
    t.tempoCurve = tempo.curve;
    t.tempoCurveStepSec = tempo.stepSec;
    if (tempo.bpm > 0) {
        t.beats = trackBeats(env, t.frameRate, tempo);
        std::vector<float> bassEnergy(F);
        const auto& b0 = t.ch[static_cast<size_t>(Ch::Band0)];
        const auto& b1 = t.ch[static_cast<size_t>(Ch::Band1)];
        for (size_t i = 0; i < F; ++i)  // undo the dB mapping of the band channels
            bassEnergy[i] = std::pow(10.f, (b0[i] * kBandDbSpan + kBandDbFloor) / 10.f) + std::pow(10.f, (b1[i] * kBandDbSpan + kBandDbFloor) / 10.f);
        const DownbeatResult db = findDownbeats(t.beats, t.frameRate, bassEnergy, t.ch[static_cast<size_t>(Ch::OnsetLow)],
                                                t.ch[static_cast<size_t>(Ch::Onset)]);
        t.beatBar = db.barPos;
        t.beatsPerBar = db.beatsPerBar;
        t.downbeatConfidence = db.confidence;
    }
    t.onsets = pickOnsets(t);

    // Structure: loudness per group + energy curve.
    const size_t G = groupVecs.size() / kSectionDims;
    std::vector<float> groupLoud(G, 0.f);
    for (size_t g = 0; g < G; ++g) {
        double s = 0;
        for (size_t i = 0; i < cfg.groupFrames; ++i) s += t.ch[static_cast<size_t>(Ch::Loudness)][g * cfg.groupFrames + i];
        groupLoud[g] = static_cast<float>(s / static_cast<double>(cfg.groupFrames));
    }
    SectionInput si;
    si.vectors = &groupVecs;
    si.dims = kSectionDims;
    si.groups = G;
    si.groupSec = static_cast<double>(cfg.groupFrames) / t.frameRate;
    si.loudness = &groupLoud;
    si.beats = &t.beats;
    si.durationSec = t.durationSec;
    t.sections = segmentStructure(si);

    const auto& lufs = t.ch[static_cast<size_t>(Ch::Loudness)];
    const size_t secs = static_cast<size_t>(std::ceil(t.durationSec));
    t.energyCurve.assign(secs, 0.f);
    if (!lufs.empty()) {
        std::vector<float> s(lufs);
        std::sort(s.begin(), s.end());
        const float lo = s[static_cast<size_t>(0.10 * static_cast<double>(s.size() - 1))];
        const float hi = s[static_cast<size_t>(0.95 * static_cast<double>(s.size() - 1))];
        for (size_t i = 0; i < secs; ++i) {
            const size_t a = static_cast<size_t>(static_cast<double>(i) * t.frameRate);
            const size_t b = std::min(lufs.size(), static_cast<size_t>(static_cast<double>(i + 1) * t.frameRate));
            double m = 0;
            for (size_t k = a; k < b; ++k) m += lufs[k];
            m /= static_cast<double>(std::max<size_t>(b - a, 1));
            t.energyCurve[i] = hi > lo ? std::clamp(static_cast<float>((m - lo) / (hi - lo)), 0.f, 1.f) : 0.5f;
        }
    }
    if (opts.progress) opts.progress(1.f);
    *out = std::move(t);
    return true;
}

}  // namespace av
