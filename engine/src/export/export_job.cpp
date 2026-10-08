#include "av/export/export_job.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "av/analysis/analysis_job.h"
#include "av/audio/decoder.h"
#include "av/export/encoders.h"
#include "av/export/mux_sink.h"
#include "av/render/offscreen_renderer.h"
#include "av/sim/front_end.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.export", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.export", __VA_ARGS__)

namespace av {

namespace {

constexpr double kVisualLookaheadSec = 0.02;  // the FFT window ends ~half a window before "now"
constexpr double kAudioLeadSec = 0.5;         // keep the AAC encoder fed this far ahead of the video

/// Sliding buffer of decoded stereo PCM addressed by absolute frame index.
class PcmWindow {
public:
    void append(const float* stereo, size_t frames) { buf_.insert(buf_.end(), stereo, stereo + frames * 2); }
    int64_t endFrame() const { return base_ + static_cast<int64_t>(buf_.size() / 2); }
    const float* at(int64_t frame) const { return buf_.data() + (frame - base_) * 2; }
    void dropBefore(int64_t frame) {
        if (frame <= base_) return;
        const size_t n = std::min(buf_.size() / 2, static_cast<size_t>(frame - base_));
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n * 2));
        base_ += static_cast<int64_t>(n);
    }

private:
    std::vector<float> buf_;
    int64_t base_ = 0;
};

}  // namespace

ExportJob::ExportJob(ExportParams p) : p_(std::move(p)) {
    p_.audioFd = dup(p_.audioFd);
    p_.outFd = dup(p_.outFd);
}

ExportJob::~ExportJob() {
    cancel();
    if (p_.audioFd >= 0) ::close(p_.audioFd);
    if (p_.outFd >= 0) ::close(p_.outFd);
}

void ExportJob::start() { thread_ = std::thread([this] { run(); }); }

void ExportJob::cancel() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
}

std::string ExportJob::error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}
std::string ExportJob::phase() const {
    std::lock_guard<std::mutex> lk(mu_);
    return phase_;
}
void ExportJob::setPhase(const char* p) {
    std::lock_guard<std::mutex> lk(mu_);
    phase_ = p;
}

void ExportJob::run() {
    std::string err;
    const bool ok = runImpl(&err);
    if (cancel_.load()) {
        state_.store(State::Cancelled);
    } else if (!ok) {
        LOGE("export failed: %s", err.c_str());
        {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = err;
        }
        state_.store(State::Failed);
    } else {
        progress_.store(1.f);
        state_.store(State::Done);
    }
}

bool ExportJob::runImpl(std::string* err) {
    // ---- audio source + analysis ------------------------------------------------------------
    setPhase("Analyzing audio");
    auto timeline = analyzeFileCached(p_.audioFd, p_.cacheDir, &cancel_, [this](float f) { progress_.store(f * 0.15f); }, nullptr, err, p_.modelPath);
    if (!timeline) return false;
    if (cancel_.load()) return false;

    Decoder dec;
    if (!dec.open(p_.audioFd, err)) return false;
    const int sr = dec.sampleRate();
    double duration = static_cast<double>(dec.durationUs()) / 1e6;
    if (duration <= 0) duration = timeline->durationSec;
    if (duration <= 0.1) {
        *err = "the audio file is empty";
        return false;
    }
    const int fps = std::clamp(p_.fps, 1, 240);
    const int64_t totalFrames = static_cast<int64_t>(std::ceil(duration * fps));

    // ---- pipeline objects -------------------------------------------------------------------
    setPhase("Preparing");
    OffscreenRenderer renderer;
    if (!renderer.create(static_cast<uint32_t>(p_.width), static_cast<uint32_t>(p_.height), p_.renderScale)) {
        *err = "could not create the GPU renderer for this resolution";
        return false;
    }
    if (!p_.overlay.empty()) renderer.setOverlay(p_.overlay.data(), p_.overlayW, p_.overlayH);
    if (!p_.background.empty()) renderer.setBackground(p_.background.data(), p_.backgroundW, p_.backgroundH);
    renderer.setBrandingOpacity(p_.overlayOpacity, p_.backgroundOpacity);
    MuxSink mux;
    if (!mux.open(p_.outFd, 2)) {
        *err = "could not open the output file";
        return false;
    }
    VideoEncoder venc;
    AudioEncoder aenc;
    if (!venc.create(p_.width, p_.height, fps, p_.videoBitrate, &mux)) {
        *err = venc.error().empty() ? "video encoder failed" : venc.error();
        return false;
    }
    if (!aenc.create(sr, p_.audioBitrate, &mux)) {
        *err = aenc.error().empty() ? "audio encoder failed" : aenc.error();
        return false;
    }

    FrontEnd fe(sr);
    fe.setSettings(p_.settings);
    fe.setManualScene(p_.scene);
    fe.setTimeline(timeline);

    // ---- main loop --------------------------------------------------------------------------
    setPhase("Rendering");
    PcmWindow pcm;
    std::vector<float> chunk(2 * 4096), mono;
    std::vector<int16_t> pcm16;
    int64_t fePos = 0;   // frames already given to the front end
    int64_t aacPos = 0;  // frames already given to the AAC encoder
    bool decoderDone = false;

    auto decodeUntil = [&](int64_t frame) {
        while (!decoderDone && pcm.endFrame() < frame && !cancel_.load()) {
            const size_t got = dec.ring().read(chunk.data(), chunk.size());
            if (got > 0) {
                pcm.append(chunk.data(), got / 2);
            } else if (dec.eof() && dec.ring().readAvailable() == 0) {
                decoderDone = true;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        }
    };
    auto feedAac = [&](int64_t upTo) -> bool {
        upTo = std::min(upTo, pcm.endFrame());
        while (aacPos < upTo) {
            const size_t n = static_cast<size_t>(std::min<int64_t>(upTo - aacPos, 4096));
            const float* s = pcm.at(aacPos);
            pcm16.resize(n * 2);
            for (size_t i = 0; i < n * 2; ++i)
                pcm16[i] = static_cast<int16_t>(std::lrintf(std::clamp(s[i], -1.f, 1.f) * 32767.f));
            if (!aenc.queuePcm(pcm16.data(), n, aacPos)) return false;
            aacPos += static_cast<int64_t>(n);
        }
        return true;
    };

    const auto t0 = std::chrono::steady_clock::now();
    const double dt = 1.0 / fps;
    auto sink = [&](int64_t idx, const uint8_t* nv12) {
        if (!venc.queueFrame(nv12, idx * 1'000'000 / fps) && !cancel_.load()) {
            *err = venc.error().empty() ? "video encoder stalled" : venc.error();
        }
    };
    for (int64_t i = 0; i < totalFrames && !cancel_.load(); ++i) {
        const double t = static_cast<double>(i) * dt;
        const int64_t feTarget = static_cast<int64_t>((t + kVisualLookaheadSec) * sr);
        const int64_t aacTarget = static_cast<int64_t>((t + kAudioLeadSec) * sr);
        decodeUntil(std::max(feTarget, aacTarget));
        if (i < 3) LOGI("frame %lld: decoded to %lld, fe=%lld aac=%lld", static_cast<long long>(i),
                        static_cast<long long>(pcm.endFrame()), static_cast<long long>(fePos), static_cast<long long>(aacPos));

        const int64_t feEnd = std::min(feTarget, pcm.endFrame());
        if (feEnd > fePos) {
            mono.resize(static_cast<size_t>(feEnd - fePos));
            const float* s = pcm.at(fePos);
            for (size_t k = 0; k < mono.size(); ++k) mono[k] = 0.5f * (s[2 * k] + s[2 * k + 1]);
            fe.pushSamples(mono.data(), mono.size());
            fePos = feEnd;
        }
        if (!feedAac(aacTarget)) break;
        pcm.dropBefore(std::min(fePos, aacPos) - 1);

        fe.step(dt);
        VisualState vs;
        fe.fill(t, dt, true, &vs);
        if (!renderer.render(i, vs, static_cast<float>(t), static_cast<float>(dt), sink)) {
            *err = "GPU render failed";
            return false;
        }
        if (!err->empty()) return false;

        if ((i & 7) == 0) {
            progress_.store(0.15f + 0.85f * static_cast<float>(i) / static_cast<float>(totalFrames));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0.5) renderFps_.store(static_cast<float>(static_cast<double>(i + 1) / el));
        }
    }
    if (cancel_.load()) return false;
    if (!renderer.flush(sink) || !err->empty()) return false;

    // ---- finish -----------------------------------------------------------------------------
    setPhase("Finalizing");
    // Remaining audio (the loop stops when the video ends; the audio may be slightly longer).
    decodeUntil(static_cast<int64_t>(duration * sr));
    if (!feedAac(static_cast<int64_t>(duration * sr))) {
        *err = aenc.error().empty() ? "audio encoder stalled" : aenc.error();
        return false;
    }
    const bool vOk = venc.finishVideo(totalFrames * 1'000'000 / fps);
    const bool aOk = aenc.finishAudio(static_cast<int64_t>(duration * 1e6));
    if (!vOk || !aOk) {
        *err = !vOk ? (venc.error().empty() ? "video encoder failed to finish" : venc.error())
                    : (aenc.error().empty() ? "audio encoder failed to finish" : aenc.error());
        return false;
    }
    if (!mux.finish()) {
        *err = "could not finalize the MP4 file";
        return false;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("exported %lld frames (%.1f s of video) in %.1f s = %.2fx real time", static_cast<long long>(totalFrames),
         duration, total, duration / std::max(total, 1e-3));
    return true;
}

}  // namespace av
