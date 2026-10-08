#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "av/core/spsc_ring.h"

struct AMediaExtractor;
struct AMediaCodec;
struct AMediaFormat;

namespace av {

/// Streaming audio decoder (AMediaExtractor + AMediaCodec). Decodes on its own thread into a
/// ring buffer of interleaved stereo float PCM, so memory use is independent of track length.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Opens the file (the fd is duplicated; caller keeps ownership) and starts the decoder
    /// thread. Blocks until the real output format is known. Returns false and sets error.
    bool open(int fd, std::string* error);
    void close();

    int sampleRate() const { return sampleRate_; }
    int64_t durationUs() const { return durationUs_; }
    bool eof() const { return eof_.load(std::memory_order_acquire); }
    const std::atomic<bool>& eofFlag() const { return eof_; }
    SpscRing<float>& ring() { return ring_; }

private:
    void run();
    void publishFormat(AMediaFormat* fmt);

    AMediaExtractor* extractor_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> eof_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    bool formatReady_ = false;
    bool failed_ = false;

    int sampleRate_ = 44100;
    int channels_ = 2;
    int pcmEncoding_ = 2;  // ENCODING_PCM_16BIT
    int64_t durationUs_ = 0;
    SpscRing<float> ring_;  // interleaved stereo, ~2 s at 48 kHz
};

}  // namespace av
