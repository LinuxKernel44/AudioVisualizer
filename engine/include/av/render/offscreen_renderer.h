#pragma once

#include <cstdint>
#include <functional>

#include "av/core/visual_state.h"
#include "av/render/render_core.h"
#include "av/render/vk_context.h"

namespace av {

/// Renders VisualState frames at an arbitrary resolution without a window and converts them to
/// NV12 on the GPU. Frames are pipelined two deep: while frame N renders, frame N-1's pixels are
/// handed to the sink. Output is a pure function of the VisualState and the supplied time.
class OffscreenRenderer {
public:
    using FrameSink = std::function<void(int64_t frameIndex, const uint8_t* nv12)>;

    ~OffscreenRenderer() { destroy(); }
    /// `width` must be a multiple of 4 and `height` even. `internalScale` scales the HDR
    /// working resolution relative to the output (1.0 = native).
    bool create(uint32_t width, uint32_t height, float internalScale = 1.f);
    void destroy();

    bool render(int64_t frameIndex, const VisualState& vs, float timeSec, float dt, const FrameSink& sink);
    /// Delivers the frame still in flight (call after the last render()).
    bool flush(const FrameSink& sink);

    size_t frameBytes() const { return static_cast<size_t>(w_) * h_ * 3 / 2; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    struct Slot {
        vk::Image final;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vk::Buffer storage;   // device local, written by the compute shader
        vk::Buffer readback;  // host visible, filled by a copy
        VkDescriptorSet set = VK_NULL_HANDLE;
        int64_t frame = -1;   // frame currently in flight in this slot
        bool pending = false;
    };
    bool deliver(Slot& s, const FrameSink& sink);

    vk::Context ctx_;
    RenderCore core_;
    Slot slots_[RenderCore::kSlots];
    VkRenderPass finalPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cl_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t w_ = 0, h_ = 0;
    bool ready_ = false;
};

}  // namespace av
