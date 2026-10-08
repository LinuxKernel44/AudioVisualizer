#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace av {

/// Sequential source of interleaved stereo float PCM (mono sources are duplicated).
class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual double sampleRate() const = 0;
    /// Reads up to `frames` stereo frames into `out` (2*frames floats). Returns the number of
    /// frames produced; 0 means end of stream. May block while data is being produced.
    virtual size_t read(float* out, size_t frames) = 0;
    /// Returns true if analysis should stop early.
    virtual bool cancelled() const { return false; }
};

/// In-memory source, used by tests and host tools.
class VectorPcmSource : public PcmSource {
public:
    VectorPcmSource(std::vector<float> interleavedStereo, double sampleRate)
        : data_(std::move(interleavedStereo)), sr_(sampleRate) {}
    double sampleRate() const override { return sr_; }
    size_t read(float* out, size_t frames) override;

private:
    std::vector<float> data_;
    double sr_;
    size_t pos_ = 0;  // in frames
};

/// Loads a PCM WAV file (8/16/24/32-bit int or 32-bit float) into stereo float. Host tools only.
bool loadWav(const std::string& path, std::vector<float>* interleavedStereo, double* sampleRate,
             std::string* error);

}  // namespace av
