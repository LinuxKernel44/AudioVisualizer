#include "av/engine.h"

namespace av {

Engine::Engine() { renderer_.start(); }

Engine::~Engine() {
    renderer_.stop();
    unload();
}

void Engine::unload() {
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
    worker_ = std::make_unique<SpectrumWorker>(tap_, state_, decoder_->sampleRate());
    worker_->start();
    return true;
}

bool Engine::play() { return player_ && player_->play(); }
bool Engine::pause() { return player_ && player_->pause(); }
void Engine::setSurface(ANativeWindow* window) { renderer_.setSurface(window); }

}  // namespace av
