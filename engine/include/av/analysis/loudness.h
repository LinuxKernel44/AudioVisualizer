#pragma once

#include <cstddef>
#include <vector>

namespace av {

/// ITU-R BS.1770-4 K-weighted loudness. Feed stereo PCM sequentially; block energies are
/// accumulated per hop so momentary / short-term / integrated values can be derived afterwards.
class LoudnessMeter {
   public:
    LoudnessMeter(double sampleRate, size_t hop);

    /// Processes stereo frames. Frame f's block covers samples [f*hop - hop/2, f*hop + hop/2).
    void process(const float* interleavedStereo, size_t frames);
    /// Flushes the final partial block (zero padded). Call once at the end of the stream.
    void finish();

    size_t numBlocks() const { return blocks_.size(); }
    /// Mean-square energy of each hop block (sum over channels, K-weighted).
    const std::vector<double>& blocks() const { return blocks_; }

    /// Momentary loudness (400 ms) per block, in LUFS.
    std::vector<float> momentary() const;
    /// Gated integrated loudness over everything processed.
    float integrated() const;
    /// Loudness range (EBU R128) in LU.
    float range() const;
    /// Peak sample magnitude (dBFS, not oversampled).
    float samplePeakDb() const;

   private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double step(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    double sr_;
    size_t hop_;
    Biquad shelf_[2], hp_[2];
    std::vector<double> blocks_;
    double acc_ = 0;
    size_t inBlock_ = 0;
    size_t firstBlockLen_;  // frame 0 is centred on sample 0, so it only holds hop/2 samples
    bool first_ = true;
    float peak_ = 0.f;
};

}  // namespace av
