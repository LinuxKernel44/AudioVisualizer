#pragma once

#include <android/native_window.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
    /// Upper bound for the internal render resolution (fraction of the screen, 0.25..1).
    void setRenderScale(float s) { renderScale_.store(s); }
    /// Display refresh rate the dynamic-quality controller aims for.
    void setTargetFps(float fps) { targetFps_.store(fps > 10.f ? fps : 60.f); }
    /// Android thermal status (0 none .. 5 shutdown): caps the render scale when the device heats up.
    void setThermalLevel(int level) { thermal_.store(level); }
    void setAutoScale(bool on) { autoScale_.store(on); }

    /// Branding layers (title/logo overlay, background image), stretched over the frame. The pixel
    /// data is premultiplied (overlay) / plain (background) sRGB RGBA8; empty clears the layer.
    void setOverlayImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h);
    void setBackgroundImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h);
    void setBrandingOpacity(float overlay, float background) {
        overlayOpacity_.store(overlay);
        backgroundOpacity_.store(background);
    }

    struct Perf {
        float frameMs = 0.f;  // presentation interval
        float gpuMs = 0.f;    // GPU time of the last measured frame (0 if unsupported)
        float scale = 1.f;    // current internal resolution scale
        float targetFps = 60.f;
    };
    Perf perf() const { return {lastFrameMs_.load(), gpuMs_.load(), scaleNow_.load(), targetFps_.load()}; }

private:
    struct Vk;
    void run();

    TripleBuffer<VisualState>& state_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> lastFrameMs_{0.f};
    std::atomic<float> renderScale_{1.0f};
    std::atomic<float> targetFps_{60.f};
    std::atomic<float> gpuMs_{0.f};
    std::atomic<float> scaleNow_{1.0f};
    std::atomic<int> thermal_{0};
    std::atomic<bool> autoScale_{true};
    struct Layer {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        bool dirty = false;
    };
    std::mutex layerMu_;
    Layer overlay_, background_;
    std::atomic<bool> layersDirty_{false};
    std::atomic<float> overlayOpacity_{1.f}, backgroundOpacity_{0.f};

    std::mutex mu_;
    std::condition_variable cv_;
    ANativeWindow* pending_ = nullptr;
    bool surfaceDirty_ = false;
};

}  // namespace av
