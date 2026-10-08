#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "av/analysis/timeline.h"
#include "av/dsp/stft.h"

namespace av {

/// Fixed analysis geometry derived from the sample rate. Frame f is centred on sample f*hop.
struct AnalysisConfig {
    double sampleRate = 44100;
    size_t fftSize = 2048;
    size_t hop = 441;
    size_t numMel = 64;
    size_t chunkFrames = 500;  // frames per parallel work unit (multiple of groupFrames)
    size_t groupFrames = 25;   // frames pooled into one structure-analysis vector (0.25 s)
    size_t context = 9;        // extra frames each side so chunks are self-contained (HPSS, flux)

    static AnalysisConfig forSampleRate(double sr);
    double frameRate() const { return sampleRate / static_cast<double>(hop); }
    /// Number of feature frames covering `totalSamples`.
    size_t framesFor(size_t totalSamples) const { return totalSamples / hop + 1; }
};

constexpr size_t kSectionDims = 12 + 13 + 1 + kNumBands;  // chroma, cepstrum, loudness, bands

/// Features of `count` consecutive frames starting at `startFrame`.
struct ChunkFeatures {
    size_t startFrame = 0;
    size_t count = 0;
    std::array<std::vector<float>, kNumChannels> ch;  // everything except Loudness (filled later)
    std::vector<float> chromaSum;                     // 12 values accumulated over the chunk
    std::vector<float> groupVectors;                  // (count/groupFrames) * kSectionDims
    float peakLin = 0.f;
    double sumSq = 0.0;  // for crest factor
};

/// Computes the features of one chunk. `segL/segR` contain the samples for frames
/// [startFrame - context, startFrame + count + context) (zero padded outside the file); see
/// chunkSampleRange(). Pure function of its inputs, so results are independent of threading.
class FeatureExtractor {
   public:
    explicit FeatureExtractor(const AnalysisConfig& cfg);
    ~FeatureExtractor();
    ChunkFeatures process(size_t startFrame, size_t count, const float* segL, const float* segR);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Absolute sample range [begin, end) required by process() for a chunk; `begin` may be negative.
struct SampleRange {
    long long begin;
    long long end;
};
SampleRange chunkSampleRange(const AnalysisConfig& cfg, size_t startFrame, size_t count);

}  // namespace av
