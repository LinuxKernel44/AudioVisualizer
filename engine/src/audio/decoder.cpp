#include "av/audio/decoder.h"

#include <android/log.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.decoder", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.decoder", __VA_ARGS__)

namespace av {

namespace {
constexpr int kPcmFloat = 4;
constexpr int kPcm8 = 3;
constexpr int kMaxBatchFrames = 31;       // extra compressed frames packed after the first
constexpr size_t kMaxBatchBytes = 12288;  // and an upper bound on the packed size

// Stereo downmix matrices for Android's default channel orders (ITU-R BS.775 coefficients,
// normalised per side so a full-scale signal on every channel cannot clip).
constexpr size_t kMaxDownmixChannels = 8;
struct Downmix {
    float l[kMaxDownmixChannels] = {};
    float r[kMaxDownmixChannels] = {};
};
/// Opens the file behind `fd` again, with its own file offset. The platform extractor reads with
/// lseek()+read() on a dup of the descriptor it is given, and all dups share one offset: two
/// decoders of the same file (playback, analysis, beat model) would corrupt each other's reads.
int reopenIndependently(int fd) {
    char path[48];
    std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const int own = ::open(path, O_RDONLY | O_CLOEXEC);
    if (own >= 0) return own;
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);  // not reopenable (unusual providers): best effort
}

Downmix downmixFor(int channels) {
    constexpr float k = 0.7071f;
    Downmix d;
    // Per channel: weight into L, weight into R (FL FR FC LFE BL BR SL SR, depending on count).
    auto set = [&](std::initializer_list<std::pair<float, float>> w) {
        size_t i = 0;
        for (const auto& p : w) {
            d.l[i] = p.first;
            d.r[i] = p.second;
            ++i;
        }
    };
    switch (channels) {
        case 1:
            set({{1.f, 1.f}});
            return d;
        case 2:
            set({{1.f, 0.f}, {0.f, 1.f}});
            return d;
        case 3:
            set({{1.f, 0.f}, {0.f, 1.f}, {k, k}});
            break;  // FL FR FC
        case 4:
            set({{1.f, 0.f}, {0.f, 1.f}, {k, 0.f}, {0.f, k}});
            break;  // quad
        case 5:
            set({{1.f, 0.f}, {0.f, 1.f}, {k, k}, {k, 0.f}, {0.f, k}});
            break;  // 5.0
        case 6:
            set({{1.f, 0.f}, {0.f, 1.f}, {k, k}, {0.f, 0.f}, {k, 0.f}, {0.f, k}});
            break;  // 5.1
        case 7:     // 6.1: FL FR FC LFE BC SL SR
            set({{1.f, 0.f}, {0.f, 1.f}, {k, k}, {0.f, 0.f}, {0.5f, 0.5f}, {k, 0.f}, {0.f, k}});
            break;
        default:  // 7.1 (FL FR FC LFE BL BR SL SR); anything larger keeps its first eight
            set({{1.f, 0.f},
                 {0.f, 1.f},
                 {k, k},
                 {0.f, 0.f},
                 {k, 0.f},
                 {0.f, k},
                 {k, 0.f},
                 {0.f, k}});
            break;
    }
    float sl = 0.f, sr = 0.f;
    for (size_t i = 0; i < kMaxDownmixChannels; ++i) {
        sl += d.l[i];
        sr += d.r[i];
    }
    for (size_t i = 0; i < kMaxDownmixChannels; ++i) {
        d.l[i] /= sl;
        d.r[i] /= sr;
    }
    return d;
}
}  // namespace

Decoder::Decoder() : ring_(1 << 18) {}  // 262144 floats = 131072 stereo frames (~2.7 s @ 48k)

Decoder::~Decoder() { close(); }

bool Decoder::open(int fd, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        close();
        return false;
    };
    fd_ = reopenIndependently(fd);
    if (fd_ < 0) return fail("Cannot open the audio file");
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

    // Only MP3 frames are self-delimiting (sync words), so only MP3 may be packed several to a
    // buffer. AAC (raw), Opus, Vorbis, FLAC... need exactly one access unit per input buffer.
    batchFrames_ = std::strcmp(mime, "audio/mpeg") == 0;
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        AMediaFormat_delete(trackFmt);
        return fail("No decoder available for this format");
    }
    // Best-effort scheduling: by default a decoder is paced for real-time playback, which would cap
    // offline analysis at a small multiple of real time. The ring buffer keeps playback fed anyway.
    AMediaFormat_setInt32(trackFmt, "priority", 1);
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
    {
        std::lock_guard<std::mutex> lk(mu_);  // wakes a thread parked by a seek
        stop_.store(true);
    }
    cv_.notify_all();
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

    // Converts one decoded buffer to interleaved stereo float and pushes it into the ring.
    auto handleOutput = [&](ssize_t out, const AMediaCodecBufferInfo& info) {
        size_t sz = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(out), &sz);
        if (data && info.size > 0) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                formatReady_ = true;
            }
            cv_.notify_all();
            data += info.offset;
            const int ch = std::max(channels_, 1);
            const size_t bytesPerSample = pcmEncoding_ == kPcmFloat ? 4
                                          : pcmEncoding_ == kPcm8   ? 1
                                                                    : 2;
            const size_t frames =
                static_cast<size_t>(info.size) / (bytesPerSample * static_cast<size_t>(ch));
            size_t skipFrames = 0;
            if (discardUntilUs >= 0) {
                if (info.presentationTimeUs >= discardUntilUs) {
                    discardUntilUs = -1;
                } else {
                    skipFrames = static_cast<size_t>((discardUntilUs - info.presentationTimeUs) *
                                                     sampleRate_ / 1000000);
                    if (skipFrames >= frames) {
                        skipFrames = frames;  // whole buffer precedes the seek target
                    } else {
                        discardUntilUs = -1;
                    }
                }
            }
            stereo.resize(frames * 2);
            const size_t stride = static_cast<size_t>(ch);
            auto sample = [&](size_t f, size_t c) -> float {
                const size_t i = f * stride + c;
                if (pcmEncoding_ == kPcmFloat) {
                    float v;
                    std::memcpy(&v, data + i * 4, 4);
                    return v;
                }
                if (pcmEncoding_ == kPcm8) return (static_cast<int>(data[i]) - 128) / 128.f;
                int16_t v;
                std::memcpy(&v, data + i * 2, 2);
                return static_cast<float>(v) * (1.f / 32768.f);
            };
            if (ch <= 2) {  // fast paths: mono / stereo
                if (pcmEncoding_ == kPcmFloat) {
                    const float* p =
                        reinterpret_cast<const float*>(data);  // codec buffers are aligned
                    for (size_t f = skipFrames; f < frames; ++f) {
                        const float l = p[f * stride];
                        stereo[2 * f] = l;
                        stereo[2 * f + 1] = ch > 1 ? p[f * stride + 1] : l;
                    }
                } else if (pcmEncoding_ == kPcm8) {
                    for (size_t f = skipFrames; f < frames; ++f) {
                        const float l = (static_cast<int>(data[f * stride]) - 128) / 128.f;
                        stereo[2 * f] = l;
                        stereo[2 * f + 1] =
                            ch > 1 ? (static_cast<int>(data[f * stride + 1]) - 128) / 128.f : l;
                    }
                } else {
                    const int16_t* p = reinterpret_cast<const int16_t*>(data);
                    constexpr float k = 1.f / 32768.f;
                    for (size_t f = skipFrames; f < frames; ++f) {
                        const float l = static_cast<float>(p[f * stride]) * k;
                        stereo[2 * f] = l;
                        stereo[2 * f + 1] = ch > 1 ? static_cast<float>(p[f * stride + 1]) * k : l;
                    }
                }
            } else {  // surround: fold every channel into stereo (a plain L/R pick loses the
                      // centre)
                const Downmix dm = downmixFor(ch);
                const size_t used = std::min(stride, kMaxDownmixChannels);
                for (size_t f = skipFrames; f < frames; ++f) {
                    float l = 0.f, r = 0.f;
                    for (size_t c = 0; c < used; ++c) {
                        const float v = sample(f, c);
                        l += dm.l[c] * v;
                        r += dm.r[c] * v;
                    }
                    stereo[2 * f] = l;
                    stereo[2 * f + 1] = r;
                }
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
    };

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

        // Feed as many input buffers as the codec will take: one buffer per round trip would cap
        // the decode speed at a small multiple of real time (each round trip costs about a
        // millisecond).
        bool fed = false;
        for (int i = 0; i < 16 && !inputEos; ++i) {
            const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 0);
            if (idx < 0) break;
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
            ssize_t n = AMediaExtractor_readSampleData(extractor_, buf, cap);
            if (n < 0) {
                AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                inputEos = true;
            } else {
                const int64_t pts = AMediaExtractor_getSampleTime(extractor_);
                AMediaExtractor_advance(extractor_);
                // Small compressed frames (MP3/AAC are a few hundred bytes) are packed several per
                // input buffer: the codec's per-buffer latency, not its arithmetic, limits decode
                // speed, so batching makes decoding many times faster than real time. The buffer
                // carries the timestamp of its first frame, and the output stays contiguous.
                size_t total = static_cast<size_t>(n);
                for (int k = 0; batchFrames_ && k < kMaxBatchFrames && total < cap / 2 &&
                                total < kMaxBatchBytes;
                     ++k) {
                    const ssize_t next = AMediaExtractor_getSampleSize(extractor_);
                    if (next <= 0 ||
                        total + static_cast<size_t>(next) > std::min(cap, kMaxBatchBytes))
                        break;
                    const ssize_t got =
                        AMediaExtractor_readSampleData(extractor_, buf + total, cap - total);
                    if (got <= 0) break;
                    total += static_cast<size_t>(got);
                    AMediaExtractor_advance(extractor_);
                }
                AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, total,
                                             static_cast<uint64_t>(pts), 0);
            }
            fed = true;
        }

        // Drain every output buffer that is ready: wait briefly for the first, then take the rest
        // without waiting.
        int64_t timeoutUs = fed ? 1000 : 5000;
        for (int i = 0; i < 64 && !stop_.load(std::memory_order_relaxed); ++i) {
            AMediaCodecBufferInfo info;
            const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeoutUs);
            timeoutUs = 0;
            if (out >= 0) {
                handleOutput(out, info);
                if (drained || seekPending_.load(std::memory_order_relaxed)) break;
            } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
                publishFormat(f);
                AMediaFormat_delete(f);
            } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
                       out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                break;
            } else {
                LOGE("dequeueOutputBuffer error %zd", out);
                std::lock_guard<std::mutex> lk(mu_);
                failed_ = true;
                cv_.notify_all();
                drained = true;
                eof_.store(true, std::memory_order_release);
                break;
            }
        }
    }
}

}  // namespace av
