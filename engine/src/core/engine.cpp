#include "av/engine.h"

#include <algorithm>

#include "av/analysis/summary.h"

namespace av {

Engine::Engine() { renderer_.start(); }

Engine::~Engine() {
    renderer_.stop();
    std::lock_guard<std::mutex> lk(mu_);
    unloadLocked();
}

std::string Engine::summaryJson() const {
    if (analysis_.state() != AnalysisJob::State::Done) return {};
    const auto t = analysis_.timeline();
    return t ? timelineSummaryJson(*t) : std::string();
}

void Engine::unloadLocked() {
    analysis_.reset();
    if (worker_) worker_->stop();
    worker_.reset();
    player_.reset();  // closes the AAudio stream before the decoder ring disappears
    decoder_.reset();
    tap_.skip(tap_.readAvailable());  // no worker is running, so this thread may consume
    snapshotLocked();
}

bool Engine::load(int fd, std::string* error) {
    std::lock_guard<std::mutex> lk(mu_);
    unloadLocked();
    auto decoder = std::make_unique<Decoder>();
    if (!decoder->open(fd, error)) return false;
    auto player = std::make_unique<Player>(decoder->ring(), tap_, decoder->eofFlag());
    if (!player->open(decoder->sampleRate())) {
        if (error) *error = "Could not open the audio output";
        return false;
    }
    decoder_ = std::move(decoder);
    player_ = std::move(player);
    analysis_.start(fd, cacheDir_, modelPath_);
    SpectrumWorker::Playback pb;
    // The worker is stopped before player_ is replaced, so it may use it without the lock.
    pb.positionSec = [this] { return static_cast<double>(player_->positionMs()) / 1000.0; };
    pb.playing = [this] { return player_->isPlaying(); };
    worker_ = std::make_unique<SpectrumWorker>(tap_, state_, decoder_->sampleRate(), controls_, pb,
                                               &analysis_);
    worker_->start();
    snapshotLocked();
    return true;
}

bool Engine::play() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!player_) return false;
    if (player_->finished() && !seekLocked(0)) return false;  // replay from the start
    const bool ok = player_->play();
    snapshotLocked();
    return ok;
}

bool Engine::pause() {
    std::lock_guard<std::mutex> lk(mu_);
    const bool ok = player_ && player_->pause();
    snapshotLocked();
    return ok;
}

bool Engine::seek(int64_t ms) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool ok = seekLocked(ms);
    snapshotLocked();
    return ok;
}

bool Engine::seekLocked(int64_t ms) {
    if (!decoder_ || !player_) return false;
    const int64_t dur = decoder_->durationUs() / 1000;
    ms = std::max<int64_t>(0, dur > 0 ? std::min(ms, dur) : ms);
    const bool wasPlaying = player_->isPlaying();
    player_->pauseAndWait();  // from here on this thread is the sole consumer of the decoder ring
    const bool parked = decoder_->beginSeek(ms * 1000);
    if (parked) {
        decoder_->ring().skip(decoder_->ring().readAvailable());
        player_->setPositionMs(ms);
        controls_.flushGen.fetch_add(1);  // the worker drops the tap samples of the old position
    }
    decoder_->endSeek();  // always: a decoder that parks late must not wait forever
    if (wasPlaying) player_->play();
    return parked;
}

void Engine::snapshotLocked() const {
    cLoaded_.store(decoder_ != nullptr);
    cPlaying_.store(player_ && player_->isPlaying());
    cFinished_.store(player_ && player_->finished());
    cPositionMs_.store(player_ ? player_->positionMs() : 0);
    cDurationMs_.store(decoder_ ? decoder_->durationUs() / 1000 : 0);
    cLatencyMs_.store(player_ ? player_->latencyMs() : 0);
}

namespace {
/// Runs `snapshot` if the engine lock is free; a getter never waits for a transport call.
template <typename F>
void trySnapshot(std::mutex& mu, F&& snapshot) {
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
    if (lk.owns_lock()) snapshot();
}
}  // namespace

bool Engine::loaded() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cLoaded_.load();
}
bool Engine::isPlaying() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cPlaying_.load();
}
bool Engine::finished() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cFinished_.load();
}
int64_t Engine::positionMs() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cPositionMs_.load();
}
int64_t Engine::durationMs() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cDurationMs_.load();
}
int Engine::audioLatencyMs() const {
    trySnapshot(mu_, [this] { snapshotLocked(); });
    return cLatencyMs_.load();
}

void Engine::setSurface(ANativeWindow* window) { renderer_.setSurface(window); }

}  // namespace av
