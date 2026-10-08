#pragma once

#include <android/native_window.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "av/core/triple_buffer.h"
#include "av/core/visual_state.h"

namespace av {

/// Owns the render thread and the Vulkan state. The surface can come and go (Android
/// lifecycle); setSurface() blocks until the renderer has fully applied the change, so the
/// caller may release the ANativeWindow as soon as it returns.
class VulkanRenderer {
public:
    explicit VulkanRenderer(TripleBuffer<VisualState>& state);
    ~VulkanRenderer();

    void start();
    void stop();
    void setSurface(ANativeWindow* window);  // nullptr = surface destroyed

    float lastFrameMs() const { return lastFrameMs_.load(); }

private:
    struct Vk;
    void run();

    TripleBuffer<VisualState>& state_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> lastFrameMs_{0.f};

    std::mutex mu_;
    std::condition_variable cv_;
    ANativeWindow* pending_ = nullptr;
    bool surfaceDirty_ = false;
};

}  // namespace av
