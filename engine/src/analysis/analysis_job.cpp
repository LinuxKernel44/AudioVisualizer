#include "av/analysis/analysis_job.h"

#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "av/analysis/analyzer.h"
#include "av/audio/decoder.h"
#include "av/audio/decoder_pcm_source.h"
#include "av/ml/beat_pass.h"
#include "av/ml/beat_timeline.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.analysis", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.analysis", __VA_ARGS__)

namespace av {

/// Bump whenever the analysis algorithms change in a way that alters results, so stale cache
/// entries are ignored. (Rev 3: ML beat gates for regularity and onset alignment.)
constexpr int kAnalysisRevision = 3;

uint64_t fingerprintFd(int fd) {
    struct stat st{};
    if (fstat(fd, &st) != 0) return 0;
    uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(st.st_size);
    auto mix = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    constexpr size_t kBlock = 64 * 1024;
    constexpr int kBlocks = 16;
    std::vector<uint8_t> buf(kBlock);
    const off_t size = st.st_size;
    for (int i = 0; i < kBlocks; ++i) {
        const off_t off =
            size > static_cast<off_t>(kBlock) * kBlocks
                ? static_cast<off_t>((static_cast<double>(size - static_cast<off_t>(kBlock)) * i) /
                                     (kBlocks - 1))
                : static_cast<off_t>(i) * static_cast<off_t>(kBlock);
        if (off >= size) break;
        const ssize_t n = pread(fd, buf.data(), kBlock, off);
        if (n > 0) mix(buf.data(), static_cast<size_t>(n));
    }
    return h;
}

void AnalysisJob::start(int fd, const std::string& cacheDir, const std::string& modelPath) {
    cancel();
    cancel_.store(false);
    state_.store(State::Running);
    progress_.store(0.f);
    fromCache_.store(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_.reset();
        error_.clear();
    }
    const int own = dup(fd);
    thread_ = std::thread([this, own, cacheDir, modelPath] { run(own, cacheDir, modelPath); });
}

std::shared_ptr<const Timeline> analyzeFileCached(int fd, const std::string& cacheDir,
                                                  const std::atomic<bool>* cancel,
                                                  const std::function<void(float)>& progress,
                                                  bool* fromCache, std::string* error,
                                                  const std::string& modelPath) {
    if (fromCache) *fromCache = false;
    const bool useMl = !modelPath.empty() && beatModelAvailable();
    std::string cachePath;
    if (!cacheDir.empty()) {
        // The cache entry depends on the audio and on whether/which beat model produced the rhythm.
        char name[96];
        unsigned long long modelTag = 0;
        if (useMl) {
            if (FILE* mf = std::fopen(modelPath.c_str(), "rb")) {
                std::fseek(mf, 0, SEEK_END);
                modelTag = static_cast<unsigned long long>(std::ftell(mf));
                std::fclose(mf);
            }
        }
        std::snprintf(name, sizeof name, "/%016llx-%s%llu-r%d.avt",
                      static_cast<unsigned long long>(fingerprintFd(fd)), useMl ? "ml" : "dsp",
                      modelTag, kAnalysisRevision);
        cachePath = cacheDir + name;
        if (FILE* f = std::fopen(cachePath.c_str(), "rb")) {
            std::vector<uint8_t> bytes;
            uint8_t tmp[65536];
            size_t n;
            while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0)
                bytes.insert(bytes.end(), tmp, tmp + n);
            std::fclose(f);
            auto t = std::make_shared<Timeline>();
            std::string err;
            if (deserialize(bytes.data(), bytes.size(), t.get(), &err)) {
                LOGI("cache hit: %s", cachePath.c_str());
                if (fromCache) *fromCache = true;
                if (progress) progress(1.f);
                return t;
            }
            LOGE("ignoring unreadable cache entry: %s", err.c_str());
        }
    }

    Decoder dec;
    std::string err;
    if (!dec.open(fd, &err)) {
        if (error) *error = err;
        return nullptr;
    }
    DecoderPcmSource src(dec, cancel);
    AnalyzerOptions opts;
    const unsigned hc = std::max(2u, std::thread::hardware_concurrency());
    opts.threads = std::max<size_t>(2, hc > 3 ? hc - 2 : hc);  // leave cores for audio + render
    opts.expectedDurationSec = static_cast<double>(dec.durationUs()) / 1e6;
    opts.cancel = cancel;
    // With the beat model the DSP pass covers the first 60 % of the progress bar, the model the
    // rest.
    opts.progress = [&](float f) {
        if (progress) progress(useMl ? f * 0.6f : f);
    };
    if (useMl) {
        opts.refineRhythm = [&](Timeline& tl) {
            BeatTimes bt;
            std::string merr;
            if (!runBeatModelPass(
                    fd, modelPath, cancel,
                    [&](float p) {
                        if (progress) progress(0.6f + 0.39f * p);
                    },
                    &bt, &merr)) {
                LOGE("beat model unavailable, keeping the DSP rhythm: %s", merr.c_str());
                return;
            }
            if (!applyMlBeats(bt, &tl))
                LOGI("beat model found no usable beats, keeping the DSP rhythm");
        };
    }
    auto t = std::make_shared<Timeline>();
    const auto t0 = std::chrono::steady_clock::now();
    if (!analyzeAudio(src, opts, t.get(), &err)) {
        if (error) *error = err;
        return nullptr;
    }
    LOGI("analysed %.1f s of audio in %.2f s: %.1f BPM, %zu beats, %zu sections", t->durationSec,
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
         static_cast<double>(t->bpm), t->beats.size(), t->sections.size());

    if (!cachePath.empty()) {
        const std::vector<uint8_t> bytes = serialize(*t);
        const std::string tmp = cachePath + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
            std::fclose(f);
            if (ok)
                std::rename(tmp.c_str(), cachePath.c_str());
            else
                std::remove(tmp.c_str());
        }
    }
    return t;
}

void AnalysisJob::run(int fd, std::string cacheDir, std::string modelPath) {
    bool cached = false;
    std::string err;
    auto t = analyzeFileCached(
        fd, cacheDir, &cancel_, [this](float p) { progress_.store(p); }, &cached, &err, modelPath);
    ::close(fd);
    if (!t) {
        LOGE("analysis failed: %s", err.c_str());
        {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = err;
        }
        state_.store(State::Failed);
        return;
    }
    fromCache_.store(cached);
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_ = std::move(t);
    }
    progress_.store(1.f);
    state_.store(State::Done);
}

}  // namespace av
