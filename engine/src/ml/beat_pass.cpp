#include "av/ml/beat_pass.h"

#include <android/log.h>

#include <chrono>
#include <thread>
#include <vector>

#include "av/audio/decoder.h"
#include "av/ml/log_mel.h"
#include "av/ml/resampler.h"

#if defined(AV_HAS_ORT)
#include "av/ml/onnx_beat_model.h"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.ml", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.ml", __VA_ARGS__)

namespace av {

#if defined(AV_HAS_ORT)

bool beatModelAvailable() { return true; }

bool runBeatModelPass(int fd, const std::string& modelPath, const std::atomic<bool>* cancel,
                      const std::function<void(float)>& progress, BeatTimes* out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        LOGE("%s", m.c_str());
        return false;
    };
    OnnxBeatModel model;
    if (!model.load(modelPath, error)) return false;

    Decoder dec;
    std::string derr;
    if (!dec.open(fd, &derr)) return fail("beat pass: " + derr);

    const auto t0 = std::chrono::steady_clock::now();
    size_t calls = 0;
    ChunkedBeatInference inf([&](const float* s, size_t T, float* b, float* d) {
        ++calls;
        return model.run(s, T, b, d);
    });
    LogMel mel;
    const bool resample = dec.sampleRate() != static_cast<int>(LogMel::kSampleRate);
    Resampler rs(dec.sampleRate(), static_cast<double>(LogMel::kSampleRate));

    std::vector<float> stereo(2 * 8192), mono, rsOut, frames;
    const double durationSec = static_cast<double>(dec.durationUs()) / 1e6;
    int64_t consumed = 0;
    for (;;) {
        if (cancel && cancel->load()) return fail("cancelled");
        const size_t got = dec.ring().read(stereo.data(), stereo.size());
        if (got == 0) {
            if (dec.eof() && dec.ring().readAvailable() == 0) break;
            std::this_thread::sleep_for(std::chrono::microseconds(400));
            continue;
        }
        const size_t n = got / 2;
        mono.resize(n);
        for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
        frames.clear();
        if (resample) {
            rsOut.clear();
            rs.process(mono.data(), n, &rsOut);
            mel.push(rsOut.data(), rsOut.size(), &frames);
        } else {
            mel.push(mono.data(), n, &frames);
        }
        if (!frames.empty() && !inf.push(frames.data(), frames.size() / ChunkedBeatInference::kMels))
            return fail("beat model inference failed: " + model.lastError());
        consumed += static_cast<int64_t>(n);
        if (progress && durationSec > 0) progress(static_cast<float>(std::min(0.98, static_cast<double>(consumed) / dec.sampleRate() / durationSec)));
    }
    frames.clear();
    if (resample) {
        rsOut.clear();
        rs.finish(&rsOut);
        mel.push(rsOut.data(), rsOut.size(), &frames);
    }
    mel.finish(&frames);
    if (!frames.empty() && !inf.push(frames.data(), frames.size() / ChunkedBeatInference::kMels))
        return fail("beat model inference failed: " + model.lastError());
    BeatActivations act;
    if (!inf.finish(&act)) return fail("beat model inference failed: " + model.lastError());
    *out = postprocessMinimal(act.beat, act.downbeat, act.fps);
    LOGI("beat model: %zu beats, %zu downbeats in %.2f s (%zu chunk(s)) for %.1f s of audio", out->beats.size(),
         out->downbeats.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), calls, durationSec);
    return true;
}

#else

bool beatModelAvailable() { return false; }
bool runBeatModelPass(int, const std::string&, const std::atomic<bool>*, const std::function<void(float)>&, BeatTimes*,
                      std::string* error) {
    if (error) *error = "this build has no ONNX Runtime";
    return false;
}

#endif

}  // namespace av
