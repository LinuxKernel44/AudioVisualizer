#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace av {

/// Runs the beat model on `frames` log-mel frames (frames x 128, row major) and writes `frames`
/// beat and downbeat logits. Returns false on failure.
using BeatPredictor = std::function<bool(const float* spect, size_t frames, float* beatLogits, float* downbeatLogits)>;

struct BeatActivations {
    std::vector<float> beat;      // logits per frame
    std::vector<float> downbeat;  // logits per frame
    double fps = 50.0;
};

/// Splits a long log-mel stream into overlapping chunks exactly like Beat This!'s
/// `split_piece` / `aggregate_prediction` (chunk 1500, border 6, "keep_first"), but streaming,
/// so only about two chunks of spectrogram are kept in memory regardless of track length.
class ChunkedBeatInference {
public:
    static constexpr size_t kMels = 128;
    static constexpr size_t kChunk = 1500;
    static constexpr size_t kBorder = 6;

    explicit ChunkedBeatInference(BeatPredictor predictor) : predict_(std::move(predictor)) {}
    /// Appends `n` frames (n * 128 floats) and runs any chunk that is now final.
    bool push(const float* frames, size_t n);
    /// Runs the remaining chunks. After this `out` holds one logit per frame.
    bool finish(BeatActivations* out);
    size_t framesReceived() const { return total_; }

private:
    bool runChunk(long long start);
    float* frameAt(long long f) { return buf_.data() + static_cast<size_t>(f - base_) * kMels; }

    BeatPredictor predict_;
    std::vector<float> buf_;  // spectrogram frames from absolute frame base_
    long long base_ = 0;
    size_t total_ = 0;
    long long nextStart_ = -static_cast<long long>(kBorder);  // start of the next regular chunk
    std::vector<float> beat_, down_;
    std::vector<char> set_;
    std::vector<float> chunkIn_, chunkBeat_, chunkDown_;
    bool failed_ = false;
};

struct BeatTimes {
    std::vector<double> beats;      // seconds
    std::vector<double> downbeats;  // seconds (each coincides with a beat)
};

/// Beat This!'s "minimal" post-processing: +-3-frame maxima above logit 0, adjacent peaks merged,
/// downbeats snapped to the nearest beat.
BeatTimes postprocessMinimal(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits,
                             double fps = 50.0);

}  // namespace av
