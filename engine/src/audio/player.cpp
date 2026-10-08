#include "av/audio/player.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.player", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.player", __VA_ARGS__)

namespace av {

Player::Player(SpscRing<float>& source, SpscRing<float>& tap, const std::atomic<bool>& sourceEof)
    : source_(source), tap_(tap), sourceEof_(sourceEof) {}

Player::~Player() { close(); }

bool Player::open(int sampleRate) {
    sampleRate_ = sampleRate;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, sampleRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, &Player::onData, this);
    AAudioStreamBuilder_setErrorCallback(b, &Player::onError, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("openStream failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    AAudioStream_setBufferSizeInFrames(stream_, AAudioStream_getFramesPerBurst(stream_) * 2);
    LOGI("stream: %d Hz, burst %d, buffer %d", AAudioStream_getSampleRate(stream_),
         AAudioStream_getFramesPerBurst(stream_), AAudioStream_getBufferSizeInFrames(stream_));
    return true;
}

void Player::close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);  // waits for the callback to finish
    stream_ = nullptr;
    playing_ = false;
}

bool Player::play() {
    if (!stream_ || finished_.load()) return false;
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) return false;
    playing_ = true;
    return true;
}

bool Player::pause() {
    if (!stream_) return false;
    playing_ = false;
    return AAudioStream_requestPause(stream_) == AAUDIO_OK;
}

void Player::pauseAndWait() {
    if (!stream_) return;
    playing_ = false;
    aaudio_stream_state_t s = AAudioStream_getState(stream_);
    if (s == AAUDIO_STREAM_STATE_STARTED || s == AAUDIO_STREAM_STATE_STARTING)
        AAudioStream_requestPause(stream_);
    for (int i = 0; i < 20; ++i) {  // up to ~2 s
        s = AAudioStream_getState(stream_);
        if (s == AAUDIO_STREAM_STATE_PAUSED || s == AAUDIO_STREAM_STATE_STOPPED ||
            s == AAUDIO_STREAM_STATE_OPEN || s == AAUDIO_STREAM_STATE_DISCONNECTED ||
            s == AAUDIO_STREAM_STATE_CLOSED || s == AAUDIO_STREAM_STATE_UNINITIALIZED)
            break;
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        AAudioStream_waitForStateChange(stream_, s, &next, 100'000'000);
    }
}

void Player::setPositionMs(int64_t ms) {
    framesPlayed_.store(ms * sampleRate_ / 1000, std::memory_order_relaxed);
    finished_.store(false);
}

int64_t Player::positionMs() const {
    return framesPlayed_.load(std::memory_order_relaxed) * 1000 / sampleRate_;
}

aaudio_data_callback_result_t Player::onData(AAudioStream*, void* user, void* audio,
                                             int32_t frames) {
    auto* self = static_cast<Player*>(user);
    auto* out = static_cast<float*>(audio);
    const size_t want = static_cast<size_t>(frames) * 2;
    const size_t got = self->source_.read(out, want);
    if (got < want) std::memset(out + got, 0, (want - got) * sizeof(float));

    // Mono tap for the analysis worker (dropped silently if the worker falls behind).
    float mono[256];
    const size_t gotFrames = got / 2;
    for (size_t i = 0; i < gotFrames;) {
        const size_t n = std::min<size_t>(256, gotFrames - i);
        for (size_t k = 0; k < n; ++k) mono[k] = 0.5f * (out[2 * (i + k)] + out[2 * (i + k) + 1]);
        self->tap_.write(mono, n);
        i += n;
    }
    self->framesPlayed_.fetch_add(static_cast<int64_t>(gotFrames), std::memory_order_relaxed);

    if (got < want && self->sourceEof_.load(std::memory_order_acquire) &&
        self->source_.readAvailable() == 0) {
        self->finished_.store(true);
        self->playing_.store(false);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void Player::onError(AAudioStream*, void*, aaudio_result_t error) {
    LOGE("stream error: %s", AAudio_convertResultToText(error));
}

}  // namespace av
