#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace av {

/// Streaming log-mel front end of the Beat This! model: 22.05 kHz mono in, 128-band frames at
/// 50 fps out. Reproduces torchaudio's MelSpectrogram(n_fft=1024, hop=441, f=30..11000 Hz,
/// slaney mel scale, power=1, normalized="frame_length", center/reflect padding) followed by
/// log1p(1000 x).
class LogMel {
   public:
    static constexpr size_t kSampleRate = 22050;
    static constexpr size_t kFft = 1024;
    static constexpr size_t kHop = 441;
    static constexpr size_t kMels = 128;

    LogMel();
    ~LogMel();
    LogMel(const LogMel&) = delete;
    LogMel& operator=(const LogMel&) = delete;

    /// Appends samples and emits every frame that is now complete (kMels floats each).
    void push(const float* x, size_t n, std::vector<float>* out);
    /// Emits the remaining frames, padding the end by reflection. Call once at end of stream.
    void finish(std::vector<float>* out);
    size_t framesEmitted() const { return next_; }
    /// Frame count for a signal of `samples` samples: 1 + samples / hop.
    static size_t frameCount(size_t samples) { return 1 + samples / kHop; }

   private:
    void emitFrame(size_t f, const float* window, std::vector<float>* out);
    float sampleAt(long long idx, long long total) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<float> buf_;  // samples from absolute index base_
    long long base_ = 0;
    long long total_ = 0;
    size_t next_ = 0;
    bool finished_ = false;
};

}  // namespace av
