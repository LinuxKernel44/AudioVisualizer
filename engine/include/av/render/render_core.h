#pragma once

#include <memory>
#include <vector>

#include "av/core/visual_state.h"
#include "av/render/post_chain.h"
#include "av/render/scene.h"
#include "av/render/vk_context.h"

namespace av {

/// Device-level rendering state shared by the on-screen renderer and the offline exporter:
/// per-frame uniform buffers and command buffers, the HDR PostChain and all scenes. It records
/// "scene(s) + post chain into a given framebuffer"; presenting or reading back is the caller's job.
class RenderCore {
public:
    static constexpr uint32_t kSlots = 2;  // frames in flight

    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;  // created signalled
        vk::Buffer ubo;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool create(vk::Context* ctx);
    void destroy();
    /// Internal HDR render size.
    bool resize(uint32_t w, uint32_t h) { return post_.resize(w, h); }
    VkExtent2D internalExtent() const { return post_.extent(); }

    Slot& slot(uint32_t i) { return slots_[i % kSlots]; }
    /// Fills the frame uniforms and records every scene and post pass into `cmd` (slot `i`'s
    /// command buffer, already begun). The composite pass renders into `fb` (compatible with `pass`).
    void record(uint32_t i, const VisualState& vs, float timeSec, float dt, VkFramebuffer fb,
                VkExtent2D extent, VkRenderPass pass);

    size_t sceneCount() const { return scenes_.size(); }
    const char* sceneName(size_t i) const { return scenes_[i % scenes_.size()]->name(); }
    VkDescriptorSetLayout frameLayout() const { return frameLayout_; }
    VkDescriptorPool descriptorPool() const { return dpool_; }

private:
    vk::Context* ctx_ = nullptr;
    VkDescriptorSetLayout frameLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkShaderModule fullscreenVs_ = VK_NULL_HANDLE;
    Slot slots_[kSlots];
    PostChain post_;
    std::vector<std::unique_ptr<Scene>> scenes_;
};

}  // namespace av
