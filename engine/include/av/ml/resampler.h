#pragma once

#include <cstddef>
#include <vector>

namespace av {

/// Streaming mono windowed-sinc (Kaiser) resampler for arbitrary rate ratios. Used to bring decoded
/// audio to the 22.05 kHz the beat model expects. Output is delayed by nothing: output sample j
/// corresponds to input time j * inRate / outRate.
class Resampler {
public:
    Resampler(double inRate, double outRate);
    /// Appends input; appends every output sample that can be computed to `out`.
    void process(const float* in, size_t n, std::vector<float>* out);
    /// Flushes the tail by assuming the input continues with silence. Call once at the end.
    void finish(std::vector<float>* out);
    size_t outputCount() const { return produced_; }

private:
    void drain(std::vector<float>* out, bool final);
    double ratio_;      // input samples per output sample
    double cutoff_;     // low-pass cutoff relative to the input Nyquist (<= 1)
    int halfWidth_;     // kernel half width in input samples
    std::vector<float> table_;  // kernel sampled at kTableRes points per input sample
    std::vector<float> buf_;
    long long base_ = 0;   // absolute index of buf_[0]
    long long total_ = 0;  // samples received
    size_t produced_ = 0;
    static constexpr int kTableRes = 256;
};

}  // namespace av
