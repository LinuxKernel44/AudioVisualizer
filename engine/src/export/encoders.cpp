#include "av/export/encoders.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.enc", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.enc", __VA_ARGS__)

namespace av {

namespace {
// MediaFormat keys / values that the NDK does not expose as constants on every API level.
constexpr const char* kKeyBitrateMode = "bitrate-mode";
constexpr const char* kKeyProfile = "profile";
constexpr const char* kKeyColorStandard = "color-standard";
constexpr const char* kKeyColorRange = "color-range";
constexpr const char* kKeyColorTransfer = "color-transfer";
constexpr const char* kKeyStride = "stride";
constexpr const char* kKeySliceHeight = "slice-height";
constexpr const char* kKeyPriority = "priority";
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;
constexpr int32_t kAvcProfileHigh = 8;
constexpr int32_t kBitrateModeVbr = 1;
constexpr int32_t kAacProfileLc = 2;
}  // namespace

EncoderBase::~EncoderBase() { abort(); }

void EncoderBase::fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(errorMu_);
        if (error_.empty()) error_ = msg;
    }
    failed_ = true;
}

bool EncoderBase::startCodec(AMediaFormat* fmt, const char* mime) {
    codec_ = AMediaCodec_createEncoderByType(mime);
    if (!codec_) {
        fail(std::string("no encoder available for ") + mime);
        return false;
    }
    if (AMediaCodec_configure(codec_, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) !=
        AMEDIA_OK) {
        fail(std::string("encoder rejected the configuration for ") + mime);
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        fail(std::string("encoder failed to start for ") + mime);
        return false;
    }
    drain_ = std::thread([this] { drainLoop(); });
    return true;
}

ssize_t EncoderBase::waitInputBuffer() {
    while (!stop_.load() && !failed_.load()) {
        const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10'000);
        if (idx >= 0) return idx;
        if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) fail("dequeueInputBuffer failed");
    }
    return -1;
}

void EncoderBase::queueEos(int64_t ptsUs) {
    const ssize_t idx = waitInputBuffer();
    if (idx >= 0)
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0,
                                     static_cast<uint64_t>(ptsUs),
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
}

void EncoderBase::drainLoop() {
    AMediaCodecBufferInfo info;
    while (!stop_.load()) {
        const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10'000);
        if (out >= 0) {
            size_t cap = 0;
            const uint8_t* data =
                AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(out), &cap);
            const bool config = info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
            const bool eos = info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
            if (data && info.size > 0 && !config && track_ >= 0) {
                if (!mux_->write(track_, data, info)) {
                    if (!stop_.load()) fail("muxer write failed");
                    AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out), false);
                    break;
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out), false);
            if (eos) break;
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            LOGI("%s encoder output format ready: %s", tag_, AMediaFormat_toString(f));
            track_ = mux_->addTrack(f);
            AMediaFormat_delete(f);
            if (track_ < 0) {
                fail("could not add the encoder track to the muxer");
                break;
            }
        } else if (out != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            fail("dequeueOutputBuffer failed");
            break;
        }
    }
}

bool EncoderBase::finish() {
    if (finished_) return !failed_;
    finished_ = true;
    if (drain_.joinable()) drain_.join();
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    return !failed_.load();
}

void EncoderBase::abort() {
    stop_ = true;
    finished_ = true;
    if (drain_.joinable()) drain_.join();
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
}

// ----------------------------------------------------------------------------------------------

bool VideoEncoder::create(int width, int height, int fps, int bitrateBps, MuxSink* mux) {
    w_ = width;
    h_ = height;
    mux_ = mux;
    tag_ = "video";
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, bitrateBps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatYuv420SemiPlanar);
    AMediaFormat_setInt32(f, kKeyProfile, kAvcProfileHigh);
    AMediaFormat_setInt32(f, kKeyBitrateMode, kBitrateModeVbr);
    AMediaFormat_setInt32(f, kKeyColorStandard, 1);  // COLOR_STANDARD_BT709
    AMediaFormat_setInt32(f, kKeyColorRange, 2);     // COLOR_RANGE_LIMITED
    AMediaFormat_setInt32(f, kKeyColorTransfer, 3);  // COLOR_TRANSFER_SDR_VIDEO
    AMediaFormat_setInt32(f, kKeyPriority, 1);       // non-real-time: encode as fast as possible
    const bool ok = startCodec(f, "video/avc");
    AMediaFormat_delete(f);
    if (!ok) return false;

    AMediaFormat* in = AMediaCodec_getInputFormat(codec_);
    int32_t v = 0;
    stride_ = AMediaFormat_getInt32(in, kKeyStride, &v) && v >= width ? v : width;
    sliceHeight_ = AMediaFormat_getInt32(in, kKeySliceHeight, &v) && v >= height ? v : height;
    AMediaFormat_delete(in);
    LOGI("video encoder %dx%d@%d, %d bps, input stride=%d slice=%d", width, height, fps, bitrateBps,
         stride_, sliceHeight_);
    return true;
}

bool VideoEncoder::queueFrame(const uint8_t* nv12, int64_t ptsUs) {
    const ssize_t idx = waitInputBuffer();
    if (idx < 0) return false;
    size_t cap = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
    const size_t need = static_cast<size_t>(stride_) * static_cast<size_t>(sliceHeight_) * 3 / 2;
    if (!dst || cap < need) {
        fail("video input buffer too small");
        return false;
    }
    const uint8_t* srcY = nv12;
    const uint8_t* srcUV = nv12 + static_cast<size_t>(w_) * static_cast<size_t>(h_);
    if (stride_ == w_ && sliceHeight_ == h_) {
        std::memcpy(dst, nv12, static_cast<size_t>(w_) * static_cast<size_t>(h_) * 3 / 2);
    } else {
        for (int r = 0; r < h_; ++r)
            std::memcpy(dst + static_cast<size_t>(r) * stride_, srcY + static_cast<size_t>(r) * w_,
                        static_cast<size_t>(w_));
        uint8_t* dstUV = dst + static_cast<size_t>(stride_) * static_cast<size_t>(sliceHeight_);
        for (int r = 0; r < h_ / 2; ++r)
            std::memcpy(dstUV + static_cast<size_t>(r) * stride_,
                        srcUV + static_cast<size_t>(r) * w_, static_cast<size_t>(w_));
    }
    return AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, need,
                                        static_cast<uint64_t>(ptsUs), 0) == AMEDIA_OK;
}

bool VideoEncoder::finishVideo(int64_t lastPtsUs) {
    queueEos(lastPtsUs);
    return finish();
}

// ----------------------------------------------------------------------------------------------

bool AudioEncoder::create(int sampleRate, int bitrateBps, MuxSink* mux) {
    sr_ = sampleRate;
    mux_ = mux;
    tag_ = "audio";
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, sampleRate);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, bitrateBps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_PROFILE, kAacProfileLc);
    const bool ok = startCodec(f, "audio/mp4a-latm");
    AMediaFormat_delete(f);
    return ok;
}

bool AudioEncoder::queuePcm(const int16_t* pcm, size_t frames, int64_t firstFramePts) {
    size_t done = 0;
    while (done < frames) {
        const ssize_t idx = waitInputBuffer();
        if (idx < 0) return false;
        size_t cap = 0;
        uint8_t* dst = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
        const size_t n = std::min(frames - done, cap / 4);
        if (!dst || n == 0) {
            fail("audio input buffer unavailable");
            return false;
        }
        std::memcpy(dst, pcm + done * 2, n * 4);
        const int64_t pts = (firstFramePts + static_cast<int64_t>(done)) * 1'000'000 / sr_;
        if (AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, n * 4,
                                         static_cast<uint64_t>(pts), 0) != AMEDIA_OK)
            return false;
        done += n;
    }
    return true;
}

bool AudioEncoder::finishAudio(int64_t endPtsUs) {
    queueEos(endPtsUs);
    return finish();
}

}  // namespace av
