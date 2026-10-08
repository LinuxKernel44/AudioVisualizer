#pragma once

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "av/export/mux_sink.h"

namespace av {

/// Shared plumbing: a MediaCodec encoder plus a thread draining its output into a MuxSink.
class EncoderBase {
   public:
    virtual ~EncoderBase();
    /// Queues end-of-stream, drains everything and joins the output thread.
    bool finish();
    /// Stops immediately (error / cancel path).
    void abort();
    const std::string& error() const { return error_; }

   protected:
    bool startCodec(AMediaFormat* fmt, const char* mime);
    /// Blocks until an input buffer is free (or cancelled). Returns its index or -1.
    ssize_t waitInputBuffer();
    void queueEos(int64_t ptsUs);
    void drainLoop();

    AMediaCodec* codec_ = nullptr;
    MuxSink* mux_ = nullptr;
    int track_ = -1;
    std::thread drain_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    bool finished_ = false;
    std::string error_;
    const char* tag_ = "encoder";
};

/// H.264 encoder fed with tightly packed NV12 frames (Y plane then interleaved UV plane).
class VideoEncoder : public EncoderBase {
   public:
    bool create(int width, int height, int fps, int bitrateBps, MuxSink* mux);
    /// Copies one NV12 frame into the codec (honouring its stride / slice height) with `ptsUs`.
    bool queueFrame(const uint8_t* nv12, int64_t ptsUs);
    bool finishVideo(int64_t lastPtsUs);

   private:
    int w_ = 0, h_ = 0, stride_ = 0, sliceHeight_ = 0;
};

/// AAC-LC encoder fed with interleaved 16-bit stereo PCM.
class AudioEncoder : public EncoderBase {
   public:
    bool create(int sampleRate, int bitrateBps, MuxSink* mux);
    bool queuePcm(const int16_t* interleaved, size_t frames, int64_t firstFramePts);
    bool finishAudio(int64_t endPtsUs);

   private:
    int sr_ = 44100;
};

}  // namespace av
