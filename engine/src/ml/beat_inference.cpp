#include "av/ml/beat_inference.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace av {

bool ChunkedBeatInference::runChunk(long long start) {
    const long long L = static_cast<long long>(total_);
    chunkIn_.assign(kChunk * kMels, 0.f);
    // Frames [start, start+kChunk) clipped to the piece; zero padding outside (left border / short end).
    const long long lo = std::max<long long>(start, 0);
    const long long hi = std::min<long long>(start + static_cast<long long>(kChunk), L);
    long long padRight = 0;
    if (start + static_cast<long long>(kChunk) > L) padRight = std::min<long long>(kBorder, start + static_cast<long long>(kChunk) - L);
    const long long leftPad = std::max<long long>(0, -start);
    const long long len = std::max<long long>(0, hi - lo) + leftPad + padRight;  // frames actually fed
    for (long long f = lo; f < hi; ++f)
        std::copy(frameAt(f), frameAt(f) + kMels, chunkIn_.data() + static_cast<size_t>(f - lo + leftPad) * kMels);
    chunkBeat_.assign(static_cast<size_t>(len), 0.f);
    chunkDown_.assign(static_cast<size_t>(len), 0.f);
    if (!predict_(chunkIn_.data(), static_cast<size_t>(len), chunkBeat_.data(), chunkDown_.data())) return false;
    // Aggregate: drop `kBorder` frames on both sides; earlier chunks win ("keep_first").
    const long long b = static_cast<long long>(kBorder);
    for (long long i = b; i < len - b; ++i) {
        const long long frame = start + i;
        if (frame < 0 || frame >= L) continue;
        if (!set_[static_cast<size_t>(frame)]) {
            beat_[static_cast<size_t>(frame)] = chunkBeat_[static_cast<size_t>(i)];
            down_[static_cast<size_t>(frame)] = chunkDown_[static_cast<size_t>(i)];
            set_[static_cast<size_t>(frame)] = 1;
        }
    }
    return true;
}

bool ChunkedBeatInference::push(const float* frames, size_t n) {
    if (failed_) return false;
    buf_.insert(buf_.end(), frames, frames + n * kMels);
    total_ += n;
    beat_.resize(total_, -1000.f);
    down_.resize(total_, -1000.f);
    set_.resize(total_, 0);
    // A regular chunk is final once the piece is known to extend at least to start+kChunk and one
    // more regular chunk follows (the very last chunk is repositioned by finish()).
    const long long step = static_cast<long long>(kChunk - 2 * kBorder);
    while (nextStart_ + static_cast<long long>(kChunk) <= static_cast<long long>(total_) &&
           nextStart_ + static_cast<long long>(kChunk - kBorder) < static_cast<long long>(total_)) {
        if (!runChunk(nextStart_)) {
            failed_ = true;
            return false;
        }
        nextStart_ += step;
        // Drop spectrogram frames no later chunk can need. The final chunk may be moved left to
        // L - (chunk - border) >= (last regular start) + border, so keep everything from there on.
        const long long keep = std::max<long long>(nextStart_ - step + static_cast<long long>(kBorder), 0);
        if (keep > base_ + 4096) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>((keep - base_) * static_cast<long long>(kMels)));
            base_ = keep;
        }
    }
    return true;
}

bool ChunkedBeatInference::finish(BeatActivations* out) {
    if (failed_) return false;
    const long long L = static_cast<long long>(total_);
    const long long step = static_cast<long long>(kChunk - 2 * kBorder);
    if (L == 0) {
        out->beat.clear();
        out->downbeat.clear();
        return true;
    }
    // The start list Beat This! would have produced for a piece of length L:
    //   starts = arange(-6, L - 6, step); if L > step: starts[-1] = L - (kChunk - 6)
    std::vector<long long> starts;
    for (long long s = -static_cast<long long>(kBorder); s < L - static_cast<long long>(kBorder); s += step) starts.push_back(s);
    if (starts.empty()) starts.push_back(-static_cast<long long>(kBorder));
    if (L > step) starts.back() = L - static_cast<long long>(kChunk - kBorder);
    for (long long s : starts) {
        // Chunks that were already run as "regular" ones have exactly this start.
        if (s < nextStart_ && s >= -static_cast<long long>(kBorder) && ((s + static_cast<long long>(kBorder)) % step) == 0 && s != starts.back()) continue;
        if (!runChunk(s)) return false;
    }
    out->beat = beat_;
    out->downbeat = down_;
    out->fps = 50.0;
    return true;
}

// ---------------------------------------------------------------------------------------------

namespace {

// Replaces groups of peaks at most `width` frames apart by their mean (Beat This! deduplicate_peaks).
std::vector<double> dedupe(const std::vector<int>& peaks, int width) {
    std::vector<double> result;
    if (peaks.empty()) return result;
    double p = peaks[0];
    int c = 1;
    for (size_t i = 1; i < peaks.size(); ++i) {
        const double p2 = peaks[i];
        if (p2 - p <= width) {
            ++c;
            p += (p2 - p) / c;
        } else {
            result.push_back(p);
            p = p2;
            c = 1;
        }
    }
    result.push_back(p);
    return result;
}

std::vector<int> pickPeaks(const std::vector<float>& x) {
    std::vector<int> peaks;
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i) {
        float m = -std::numeric_limits<float>::infinity();  // max_pool1d(kernel 7, stride 1, pad 3)
        for (int k = std::max(0, i - 3); k <= std::min(n - 1, i + 3); ++k) m = std::max(m, x[static_cast<size_t>(k)]);
        if (x[static_cast<size_t>(i)] == m && x[static_cast<size_t>(i)] > 0.f) peaks.push_back(i);
    }
    return peaks;
}

}  // namespace

BeatTimes postprocessMinimal(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits, double fps) {
    BeatTimes r;
    for (double f : dedupe(pickPeaks(beatLogits), 1)) r.beats.push_back(f / fps);
    std::vector<double> down;
    for (double f : dedupe(pickPeaks(downbeatLogits), 1)) down.push_back(f / fps);
    if (!r.beats.empty()) {
        for (double& d : down) {
            size_t best = 0;
            for (size_t i = 1; i < r.beats.size(); ++i)
                if (std::fabs(r.beats[i] - d) < std::fabs(r.beats[best] - d)) best = i;
            d = r.beats[best];
        }
    }
    std::sort(down.begin(), down.end());
    down.erase(std::unique(down.begin(), down.end()), down.end());
    r.downbeats = std::move(down);
    return r;
}

}  // namespace av
