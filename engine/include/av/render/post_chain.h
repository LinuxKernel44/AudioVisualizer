#pragma once

#include <map>
#include <utility>

#include "av/core/visual_state.h"
#include "av/render/vk_context.h"

namespace av {

/// HDR scene targets plus the post-processing passes: optional scene transition mix, optional
/// feedback trails, dual-filter bloom and the final composite (tone mapping, chromatic
/// aberration, glitch, grain, vignette) into the swapchain image.
class PostChain {
public:
    bool create(const vk::Context& ctx, VkShaderModule fullscreenVert, VkDescriptorSetLayout frameLayout);
    void destroy();

    /// (Re)creates the size-dependent targets. `w`/`h` are the internal render size.
    bool resize(uint32_t w, uint32_t h);
    /// The composite pass renders into swapchain images of this render pass.
    bool setSwapchainPass(VkRenderPass pass);

    VkRenderPass scenePass() const { return rpScene_; }
    VkExtent2D extent() const { return {w_, h_}; }
    VkFormat hdrFormat() const { return hdrFormat_; }

    void beginScenePass(VkCommandBuffer cmd, uint32_t slot);
    void endScenePass(VkCommandBuffer cmd);

    /// Records every pass after the scene(s) were drawn. `useTransition` mixes scene slot 0
    /// (outgoing) and 1 (incoming) first.
    void record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const Look& look, bool useTransition,
                float transition, VkFramebuffer swapFb, VkExtent2D swapExtent, VkRenderPass swapPass);

private:
    struct Target {
        vk::Image img;
        VkFramebuffer fbScene = VK_NULL_HANDLE;  // compatible with rpScene_/rpPost_/rpAdd_
    };
    struct Pipelines {
        VkPipeline down0 = VK_NULL_HANDLE, down = VK_NULL_HANDLE, up = VK_NULL_HANDLE,
                   mix = VK_NULL_HANDLE, feedback = VK_NULL_HANDLE, composite = VK_NULL_HANDLE;
    };

    bool createPass(VkRenderPass* out, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout fin);
    bool createTarget(Target* t, uint32_t w, uint32_t h);
    void destroyTarget(Target* t);
    VkDescriptorSet pairSet(VkImageView a, VkImageView b);
    void fullscreenPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkExtent2D ext,
                        VkPipeline pipe, VkDescriptorSet frameSet, VkDescriptorSet tex, const float pc[4],
                        bool clear = false);
    void clearTargets();

    const vk::Context* ctx_ = nullptr;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkFormat hdrFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass rpScene_ = VK_NULL_HANDLE, rpPost_ = VK_NULL_HANDLE, rpAdd_ = VK_NULL_HANDLE;
    Pipelines pipes_;
    VkRenderPass swapPass_ = VK_NULL_HANDLE;

    static constexpr int kBloomLevels = 4;
    Target scene_[2], mix_, fb_[2], bloom_[kBloomLevels];
    int fbCur_ = 0;
    uint32_t w_ = 0, h_ = 0;
    std::map<std::pair<VkImageView, VkImageView>, VkDescriptorSet> sets_;
};

}  // namespace av
