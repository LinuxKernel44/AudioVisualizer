#include "av/export/mux_sink.h"

#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.mux", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.mux", __VA_ARGS__)

namespace av {

bool MuxSink::open(int fd, int expectedTracks) {
    muxer_ = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    expected_ = expectedTracks;
    return muxer_ != nullptr;
}

int MuxSink::addTrack(AMediaFormat* format) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!muxer_ || started_) return -1;
    const ssize_t idx = AMediaMuxer_addTrack(muxer_, format);
    if (idx < 0) {
        LOGE("addTrack failed: %zd", idx);
        return -1;
    }
    if (++added_ == expected_) {
        if (AMediaMuxer_start(muxer_) != AMEDIA_OK) {
            LOGE("muxer start failed");
            return -1;
        }
        started_ = true;
        LOGI("muxer started with %d tracks, flushing %zu queued samples", added_, pending_.size());
        for (const Pending& p : pending_) {
            AMediaCodecBufferInfo info = p.info;
            info.offset = 0;
            AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(p.track), p.data.data(), &info);
        }
        pending_.clear();
        pending_.shrink_to_fit();
        cv_.notify_all();
    }
    return static_cast<int>(idx);
}

bool MuxSink::write(int track, const uint8_t* data, const AMediaCodecBufferInfo& info) {
    std::lock_guard<std::mutex> lk(mu_);
    if (aborted_ || !muxer_) return false;
    if (!started_) {
        Pending p;
        p.track = track;
        p.info = info;
        p.data.assign(data + info.offset, data + info.offset + info.size);
        pending_.push_back(std::move(p));
        return true;
    }
    return AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(track), data, &info) == AMEDIA_OK;
}

bool MuxSink::finish() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!muxer_) return false;
    const bool ok = started_ && AMediaMuxer_stop(muxer_) == AMEDIA_OK;
    AMediaMuxer_delete(muxer_);
    muxer_ = nullptr;
    return ok;
}

void MuxSink::abort() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        aborted_ = true;
        if (muxer_) {
            if (started_) AMediaMuxer_stop(muxer_);
            AMediaMuxer_delete(muxer_);
            muxer_ = nullptr;
        }
    }
    cv_.notify_all();
}

}  // namespace av
