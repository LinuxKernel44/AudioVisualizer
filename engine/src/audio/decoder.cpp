#include "av/audio/decoder.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.decoder", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.decoder", __VA_ARGS__)

namespace av {

namespace {
constexpr int kPcmFloat = 4;
constexpr int kPcm8 = 3;
}  // namespace

Decoder::Decoder() : ring_(1 << 18) {}  // 262144 floats = 131072 stereo frames (~2.7 s @ 48k)

Decoder::~Decoder() { close(); }

bool Decoder::open(int fd, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        close();
        return false;
    };
    fd_ = dup(fd);
    if (fd_ < 0) return fail("dup failed");
    struct stat st{};
    if (fstat(fd_, &st) != 0) return fail("fstat failed");

    extractor_ = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, st.st_size) != AMEDIA_OK)
        return fail("Unsupported or unreadable audio file");

    const size_t tracks = AMediaExtractor_getTrackCount(extractor_);
    AMediaFormat* trackFmt = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < tracks; ++i) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(extractor_, i);
        const char* m = nullptr;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
            strncmp(m, "audio/", 6) == 0) {
            trackFmt = f;
            mime = m;
            AMediaExtractor_selectTrack(extractor_, i);
            break;
        }
        AMediaFormat_delete(f);
    }
    if (!trackFmt) return fail("No audio track found");

    int32_t v = 0;
    if (AMediaFormat_getInt32(trackFmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v)) sampleRate_ = v;
    if (AMediaFormat_getInt32(trackFmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v)) channels_ = v;
    int64_t dur = 0;
    if (AMediaFormat_getInt64(trackFmt, AMEDIAFORMAT_KEY_DURATION, &dur)) durationUs_ = dur;

    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        AMediaFormat_delete(trackFmt);
        return fail("No decoder available for this format");
    }
    const media_status_t cs = AMediaCodec_configure(codec_, trackFmt, nullptr, nullptr, 0);
    AMediaFormat_delete(trackFmt);
    if (cs != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK)
        return fail("Decoder failed to start");

    thread_ = std::thread([this] { run(); });

    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::seconds(5), [this] { return formatReady_ || failed_; }) ||
        failed_) {
        lk.unlock();
        return fail("Decoding failed");
    }
    LOGI("opened: %d Hz, %d ch, %lld us", sampleRate_, channels_,
         static_cast<long long>(durationUs_));
    return true;
}

void Decoder::close() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (extractor_) {
        AMediaExtractor_delete(extractor_);
        extractor_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Decoder::beginSeek(int64_t targetUs) {
    std::unique_lock<std::mutex> lk(mu_);
    seekTargetUs_ = std::clamp<int64_t>(targetUs, 0, durationUs_ > 0 ? durationUs_ : targetUs);
    parked_ = false;
    resume_ = false;
    seekPending_.store(true, std::memory_order_release);
    return cv_.wait_for(lk, std::chrono::seconds(2), [this] { return parked_; });
}

void Decoder::endSeek() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        resume_ = true;
    }
    cv_.notify_all();
}

void Decoder::publishFormat(AMediaFormat* fmt) {
    int32_t v = 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) sampleRate_ = v;
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) channels_ = v;
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_PCM_ENCODING, &v)) pcmEncoding_ = v;
    formatReady_ = true;
    cv_.notify_all();
}

void Decoder::run() {
    bool inputEos = false;
    bool drained = false;
    int64_t discardUntilUs = -1;
    std::vector<float> stereo;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (seekPending_.load(std::memory_order_acquire)) {
            int64_t target;
            {
                std::lock_guard<std::mutex> lk(mu_);
                target = seekTargetUs_;
            }
            AMediaExtractor_seekTo(extractor_, target, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            AMediaCodec_flush(codec_);
            inputEos = false;
            drained = false;
            eof_.store(false, std::memory_order_release);
            discardUntilUs = target;  // decode from the previous sync point, drop up to target
            std::unique_lock<std::mutex> lk(mu_);
            parked_ = true;
            cv_.notify_all();
            cv_.wait(lk, [this] { return resume_ || stop_.load(); });
            parked_ = false;
            resume_ = false;
            seekPending_.store(false, std::memory_order_release);
            continue;
        }
        if (drained) {  // end of stream reached: stay alive so the user can seek back
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!inputEos) {
            const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 0);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
                const ssize_t n = AMediaExtractor_readSampleData(extractor_, buf, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inputEos = true;
                } else {
                    const int64_t pts = AMediaExtractor_getSampleTime(extractor_);
                    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0,
                                                 static_cast<size_t>(n), static_cast<uint64_t>(pts),
                                                 0);
                    AMediaExtractor_advance(extractor_);
                }
            }
        }

        AMediaCodecBufferInfo info;
        const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 5000);
        if (out >= 0) {
            size_t sz = 0;
            const uint8_t* data =
                AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(out), &sz);
            if (data && info.size > 0) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    formatReady_ = true;
                }
                cv_.notify_all();
                data += info.offset;
                const int ch = std::max(channels_, 1);
                size_t frames = 0;
                size_t bytesPerSample = pcmEncoding_ == kPcmFloat ? 4
                                        : pcmEncoding_ == kPcm8   ? 1
                                                                  : 2;
                frames =
                    static_cast<size_t>(info.size) / (bytesPerSample * static_cast<size_t>(ch));
                size_t skipFrames = 0;
                if (discardUntilUs >= 0) {
                    if (info.presentationTimeUs >= discardUntilUs) {
                        discardUntilUs = -1;
                    } else {
                        skipFrames = static_cast<size_t>(
                            (discardUntilUs - info.presentationTimeUs) * sampleRate_ / 1000000);
                        if (skipFrames >= frames) {
                            skipFrames = frames;  // whole buffer precedes the seek target
                        } else {
                            discardUntilUs = -1;
                        }
                    }
                }
                stereo.resize(frames * 2);
                auto sampleAt = [&](size_t f, int c) -> float {
                    const uint8_t* p =
                        data +
                        (f * static_cast<size_t>(ch) + static_cast<size_t>(c)) * bytesPerSample;
                    if (pcmEncoding_ == kPcmFloat) {
                        float x;
                        memcpy(&x, p, 4);
                        return x;
                    }
                    if (pcmEncoding_ == kPcm8) return (static_cast<int>(*p) - 128) / 128.f;
                    int16_t s;
                    memcpy(&s, p, 2);
                    return static_cast<float>(s) * (1.f / 32768.f);
                };
                for (size_t f = skipFrames; f < frames; ++f) {
                    const float l = sampleAt(f, 0);
                    stereo[2 * f] = l;
                    stereo[2 * f + 1] = ch > 1 ? sampleAt(f, 1) : l;
                }
                size_t done = skipFrames * 2;
                while (done < stereo.size() && !stop_.load(std::memory_order_relaxed) &&
                       !seekPending_.load(std::memory_order_relaxed)) {
                    const size_t n = ring_.write(stereo.data() + done, stereo.size() - done);
                    done += n;
                    if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                drained = true;
                eof_.store(true, std::memory_order_release);
            }
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            publishFormat(f);
            AMediaFormat_delete(f);
        } else if (out != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGE("dequeueOutputBuffer error %zd", out);
            std::lock_guard<std::mutex> lk(mu_);
            failed_ = true;
            cv_.notify_all();
            drained = true;
            eof_.store(true, std::memory_order_release);
        }
    }
}

}  // namespace av
