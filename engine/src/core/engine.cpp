#include "av/engine.h"

#include <algorithm>

#include "av/analysis/summary.h"

namespace av {

Engine::Engine() { renderer_.start(); }

Engine::~Engine() {
    renderer_.stop();
    unload();
}

std::string Engine::summaryJson() const {
    if (analysis_.state() != AnalysisJob::State::Done) return {};
    const auto t = analysis_.timeline();
    return t ? timelineSummaryJson(*t) : std::string();
}

void Engine::unload() {
    analysis_.cancel();
    if (worker_) worker_->stop();
    worker_.reset();
    player_.reset();  // closes the AAudio stream before the decoder ring disappears
    decoder_.reset();
    tap_.skip(tap_.readAvailable());
}

bool Engine::load(int fd, std::string* error) {
    unload();
    auto decoder = std::make_unique<Decoder>();
    if (!decoder->open(fd, error)) return false;
    auto player = std::make_unique<Player>(decoder->ring(), tap_, decoder->eofFlag());
    if (!player->open(decoder->sampleRate())) {
        if (error) *error = "Could not open the audio output";
        return false;
    }
    decoder_ = std::move(decoder);
    player_ = std::move(player);
    analysis_.start(fd, cacheDir_);
    SpectrumWorker::Playback pb;
    pb.positionSec = [this] { return static_cast<double>(player_->positionMs()) / 1000.0; };
    pb.playing = [this] { return player_->isPlaying(); };
    worker_ = std::make_unique<SpectrumWorker>(tap_, state_, decoder_->sampleRate(), pb, &analysis_);
    worker_->setManualScene(manualScene_.load());
    worker_->start();
    return true;
}

bool Engine::play() { return player_ && player_->play(); }
bool Engine::seek(int64_t ms) {
    if (!decoder_ || !player_) return false;
    const int64_t dur = durationMs();
    ms = std::max<int64_t>(0, dur > 0 ? std::min(ms, dur) : ms);
    const bool wasPlaying = player_->isPlaying();
    player_->pauseAndWait();  // from here on this thread is the sole consumer of both rings
    if (!decoder_->beginSeek(ms * 1000)) return false;
    decoder_->ring().skip(decoder_->ring().readAvailable());
    tap_.skip(tap_.readAvailable());
    player_->setPositionMs(ms);
    decoder_->endSeek();
    if (wasPlaying) player_->play();
    return true;
}

bool Engine::pause() { return player_ && player_->pause(); }
void Engine::setSurface(ANativeWindow* window) { renderer_.setSurface(window); }

}  // namespace av
