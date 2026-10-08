#pragma once

#include <chrono>
#include <thread>

#include "av/analysis/pcm_source.h"
#include "av/audio/decoder.h"

namespace av {

/// Adapts a streaming Decoder to the analysis PcmSource interface (a dedicated Decoder instance
/// is used, so analysis never competes with playback for the ring buffer).
class DecoderPcmSource : public PcmSource {
   public:
    DecoderPcmSource(Decoder& d, const std::atomic<bool>* cancel) : dec_(d), cancel_(cancel) {}
    double sampleRate() const override { return dec_.sampleRate(); }
    bool cancelled() const override { return cancel_ && cancel_->load(); }
    size_t read(float* out, size_t frames) override {
        for (;;) {
            const size_t got = dec_.ring().read(out, frames * 2);
            if (got > 0) return got / 2;
            if (dec_.eof() && dec_.ring().readAvailable() == 0) return 0;
            if (cancelled()) return 0;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

   private:
    Decoder& dec_;
    const std::atomic<bool>* cancel_;
};

}  // namespace av
