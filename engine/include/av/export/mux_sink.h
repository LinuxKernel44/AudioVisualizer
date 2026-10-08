#pragma once

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace av {

/// Thread-safe MP4 muxer wrapper. Tracks are added as the encoders report their output formats;
/// the muxer starts once all expected tracks exist, and write() blocks until then.
class MuxSink {
public:
    ~MuxSink() { abort(); }
    /// `fd` must be seekable and opened read/write; it stays owned by the caller.
    bool open(int fd, int expectedTracks);
    int addTrack(AMediaFormat* format);  // -1 on failure
    /// Writes a sample. Before the muxer has started (it needs every track's format first) samples
    /// are queued in memory instead of blocking the encoder's output thread, which would stall the
    /// encoder and, in turn, the producer that has to supply the other track's first frame.
    bool write(int track, const uint8_t* data, const AMediaCodecBufferInfo& info);
    /// Stops the muxer and finalises the file. Returns false if it never started.
    bool finish();
    /// Wakes writers and releases the muxer without finalising (error / cancel path).
    void abort();

private:
    AMediaMuxer* muxer_ = nullptr;
    int expected_ = 2;
    int added_ = 0;
    bool started_ = false;
    bool aborted_ = false;
    struct Pending {
        int track;
        AMediaCodecBufferInfo info;
        std::vector<uint8_t> data;
    };
    std::vector<Pending> pending_;
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace av
