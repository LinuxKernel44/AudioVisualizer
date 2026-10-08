#include "av/dsp/spectrum_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "av/dsp/bar_spectrum.h"

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace av {

namespace {
constexpr size_t kBars = 64;
constexpr float kAttack = 0.65f;   // fraction of the gap closed per hop when rising
constexpr float kRelease = 0.90f;  // multiplicative decay per hop when falling
}  // namespace

SpectrumWorker::SpectrumWorker(SpscRing<float>& tap, TripleBuffer<VisualState>& out,
                               int sampleRate, Playback playback, const AnalysisJob* job)
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
    BarSpectrum analyzer(sampleRate_, kBars);
    const size_t kFftSize = analyzer.window();
    const size_t hop = static_cast<size_t>(sampleRate_ / 100);  // ~100 updates per second
    std::vector<float> window(kFftSize, 0.f), bars(kBars), smooth(kBars, 0.f);
    float rms = 0.f;
    auto lastData = std::chrono::steady_clock::now();

    std::unique_ptr<TimelineSampler> sampler;
    Director director;
    auto lastPoll = std::chrono::steady_clock::now();
    auto lastPublish = lastPoll;
    int appliedManual = -2;
    auto publish = [&] {
        if (!sampler && job_ && std::chrono::steady_clock::now() - lastPoll > std::chrono::milliseconds(100)) {
            lastPoll = std::chrono::steady_clock::now();
            if (job_->state() == AnalysisJob::State::Done) {
                if (auto t = job_->timeline()) {
                    director.setTimeline(t);
                    sampler = std::make_unique<TimelineSampler>(std::move(t));
                }
            }
        }
        VisualState& s = out_.writeSlot();
        s.numBars = kBars;
        std::memcpy(s.bars, smooth.data(), kBars * sizeof(float));
        s.rms = rms;
        if (sampler && playback_.positionSec) {
            sampler->sample(playback_.positionSec(), &s);
            if (playback_.playing && !playback_.playing()) {
                s.beatPulse = 0.f;
                s.kick = 0.f;
            }
        } else {
            s.hasTimeline = false;
            s.beatPulse = s.kick = s.beatPhase = s.sectionEnergy = s.bpm = 0.f;
            s.barBeat = 0;
        }
        // Waveform for oscilloscope-style scenes: peak-decimate the newest 2048 samples to 256.
        {
            const size_t block = 2048 / VisualState::kWaveSamples;
            const float* w = window.data() + kFftSize - 2048;
            for (size_t i = 0; i < VisualState::kWaveSamples; ++i) {
                float peak = 0.f;
                for (size_t k = 0; k < block; ++k)
                    if (std::fabs(w[i * block + k]) > std::fabs(peak)) peak = w[i * block + k];
                s.wave[i] = peak;
            }
        }
        // Scene selection / look from the director (position driven in automatic mode).
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastPublish).count();
        lastPublish = now;
        const int manual = manualScene_.load();
        if (manual != appliedManual) {
            director.setManualScene(manual);
            appliedManual = manual;
        }
        const double pos = playback_.positionSec ? playback_.positionSec() : 0.0;
        director.update(pos, std::min(dt, 0.1), &s);
        out_.publish();
    };

    while (!stop_.load(std::memory_order_relaxed)) {
        if (tap_.readAvailable() >= hop) {
            std::memmove(window.data(), window.data() + hop, (kFftSize - hop) * sizeof(float));
            float* fresh = window.data() + kFftSize - hop;
            tap_.read(fresh, hop);
            double acc = 0.0;
            for (size_t i = 0; i < hop; ++i) acc += static_cast<double>(fresh[i]) * fresh[i];
            const float inst = static_cast<float>(std::sqrt(acc / static_cast<double>(hop)));
            rms += (inst - rms) * (inst > rms ? 0.5f : 0.1f);

            analyzer.analyze(window.data(), bars.data());
            for (size_t b = 0; b < kBars; ++b) {
                if (bars[b] > smooth[b])
                    smooth[b] += (bars[b] - smooth[b]) * kAttack;
                else
                    smooth[b] *= kRelease;
            }
            publish();
            lastData = std::chrono::steady_clock::now();
        } else {
            const auto idle = std::chrono::steady_clock::now() - lastData;
            if (idle > std::chrono::milliseconds(40)) {  // paused or starved: decay to silence
                bool any = rms > 1e-4f;
                for (float& v : smooth) {
                    v *= kRelease;
                    any = any || v > 1e-3f;
                }
                rms *= kRelease;
                if (any) publish();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
}

}  // namespace av
