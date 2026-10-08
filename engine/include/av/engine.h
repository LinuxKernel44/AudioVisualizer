#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <string>

#include "av/audio/decoder.h"
#include "av/audio/player.h"
#include "av/core/spsc_ring.h"
#include "av/core/triple_buffer.h"
#include "av/core/visual_state.h"
#include "av/dsp/spectrum_worker.h"
#include "av/render/vulkan_renderer.h"

namespace av {

/// Top-level native object: wires decoder -> player -> analysis worker -> renderer.
/// All public methods are called from the Android main/worker threads and are not real-time.
class Engine {
public:
    Engine();
    ~Engine();

    /// Loads an audio file from an open fd (duplicated internally). Returns false with error.
    bool load(int fd, std::string* error);
    bool play();
    bool pause();
    bool seek(int64_t ms);
    void setSurface(ANativeWindow* window);

    bool loaded() const { return decoder_ != nullptr; }
    bool isPlaying() const { return player_ && player_->isPlaying(); }
    bool finished() const { return player_ && player_->finished(); }
    int64_t positionMs() const { return player_ ? player_->positionMs() : 0; }
    int64_t durationMs() const { return decoder_ ? decoder_->durationUs() / 1000 : 0; }
    float frameMs() const { return renderer_.lastFrameMs(); }

private:
    void unload();

    TripleBuffer<VisualState> state_;  // declared first: outlives worker and renderer
    SpscRing<float> tap_{1 << 15};
    VulkanRenderer renderer_{state_};
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<SpectrumWorker> worker_;
};

}  // namespace av
