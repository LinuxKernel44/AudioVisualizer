#include "av/dsp/spectrum_worker.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "av/sim/front_end.h"

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace av {

SpectrumWorker::SpectrumWorker(SpscRing<float>& tap, TripleBuffer<VisualState>& out, int sampleRate,
                               Playback playback, const AnalysisJob* job)
    : tap_(tap), out_(out), sampleRate_(sampleRate), playback_(std::move(playback)), job_(job) {}

SpectrumWorker::~SpectrumWorker() { stop(); }

void SpectrumWorker::start() {
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void SpectrumWorker::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void SpectrumWorker::run() {
#if defined(__ANDROID__)
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -4);
#endif
    using Clock = std::chrono::steady_clock;
    FrontEnd fe(sampleRate_);
    const size_t hop = static_cast<size_t>(sampleRate_ / 100);  // ~100 analysis steps per second
    std::vector<float> chunk(hop);
    auto lastData = Clock::now();
    auto lastPublish = lastData;
    auto lastPoll = lastData;
    bool timelineSet = false;
    int appliedManual = -2;
    uint32_t appliedSettings = ~0u;

    auto publish = [&] {
        const auto now = Clock::now();
        const double dt = std::min(0.1, std::chrono::duration<double>(now - lastPublish).count());
        lastPublish = now;
        if (!timelineSet && job_ && now - lastPoll > std::chrono::milliseconds(100)) {
            lastPoll = now;
            if (job_->state() == AnalysisJob::State::Done) {
                if (auto t = job_->timeline()) {
                    fe.setTimeline(std::move(t));
                    timelineSet = true;
                }
            }
        }
        const uint32_t gen = settingsGen_.load();
        if (gen != appliedSettings) {
            std::lock_guard<std::mutex> lk(settingsMu_);
            fe.setSettings(settings_);
            appliedSettings = gen;
        }
        const int manual = manualScene_.load();
        if (manual != appliedManual) {
            fe.setManualScene(manual);
            appliedManual = manual;
        }
        const double pos = playback_.positionSec ? playback_.positionSec() : 0.0;
        const bool playing = playback_.playing ? playback_.playing() : true;
        fe.fill(pos, dt, playing, &out_.writeSlot());
        out_.publish();
    };

    while (!stop_.load(std::memory_order_relaxed)) {
        if (tap_.readAvailable() >= hop) {
            tap_.read(chunk.data(), hop);
            fe.pushSamples(chunk.data(), hop);
            fe.step(static_cast<double>(hop) / sampleRate_);
            publish();
            lastData = Clock::now();
        } else if (Clock::now() - lastData > std::chrono::milliseconds(40)) {
            fe.decay(0.016);  // paused or starved: fall to silence, keep scenes/transitions alive
            publish();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

}  // namespace av
