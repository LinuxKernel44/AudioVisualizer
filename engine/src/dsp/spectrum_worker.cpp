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
                               int sampleRate)
    : tap_(tap), out_(out), sampleRate_(sampleRate) {}

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

    auto publish = [&] {
        VisualState& s = out_.writeSlot();
        s.numBars = kBars;
        std::memcpy(s.bars, smooth.data(), kBars * sizeof(float));
        s.rms = rms;
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
