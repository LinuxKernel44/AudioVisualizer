#include "av/render/post_chain.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "av/render/scene.h"
#include "post_composite_frag.h"
#include "post_down0_frag.h"
#include "post_down_frag.h"
#include "post_feedback_frag.h"
#include "post_mix_frag.h"
#include "post_up_frag.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.post", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.post", __VA_ARGS__)

#define VK_TRY(expr)                                                      \
    do {                                                                  \
        const VkResult r_ = (expr);                                       \
        if (r_ != VK_SUCCESS) {                                           \
            LOGE("%s failed: %d (%s:%d)", #expr, r_, __FILE__, __LINE__); \
            return false;                                                 \
        }                                                                 \
    } while (0)

namespace av {

namespace {
constexpr VkImageUsageFlags kTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

bool PostChain::createPass(VkRenderPass* out, VkAttachmentLoadOp load, VkImageLayout initial,
                           VkImageLayout fin) {
    VkAttachmentDescription att{};
    att.format = hdrFormat_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = load;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = initial;
    att.finalLayout = fin;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VK_TRY(vkCreateRenderPass(ctx_->dev, &rpi, nullptr, out));
    return true;
}

bool PostChain::create(const vk::Context& ctx, VkShaderModule vs, VkDescriptorSetLayout frameLayout) {
    ctx_ = &ctx;
    vs_ = vs;
    frameLayout_ = frameLayout;

    // HDR format: RGBA16F if it can be blended and filtered, else packed float, else 8-bit.
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (VkFormat f : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM}) {
        if (ctx.formatSupports(f, need)) {
            hdrFormat_ = f;
            break;
        }
    }
    if (hdrFormat_ == VK_FORMAT_UNDEFINED) {
        LOGE("no usable render-target format");
        return false;
    }
    LOGI("HDR format %d", hdrFormat_);

    if (!createPass(&rpScene_, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return false;
    if (!createPass(&rpPost_, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return false;
    if (!createPass(&rpAdd_, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    VK_TRY(vkCreateSampler(ctx.dev, &si, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    VK_TRY(vkCreateDescriptorSetLayout(ctx.dev, &dl, nullptr, &texLayout_));

    VkDescriptorSetLayout layouts[3] = {frameLayout_, texLayout_, texLayout_};  // set 2: branding layers
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 3;
    pl.pSetLayouts = layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_TRY(vkCreatePipelineLayout(ctx.dev, &pl, nullptr, &layout_));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 64};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 64;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(ctx.dev, &dp, nullptr, &pool_));

    auto pipe = [&](const uint32_t* code, size_t bytes, VkRenderPass rp, bool additive, VkPipeline* out) {
        VkShaderModule fs = vk::createShader(ctx, code, bytes);
        if (!fs) return false;
        *out = makeFullscreenPipeline(ctx, vs_, fs, rp, layout_, additive);
        vkDestroyShaderModule(ctx.dev, fs, nullptr);
        return *out != VK_NULL_HANDLE;
    };
    if (!pipe(shaders::post_down0_frag, shaders::post_down0_fragSize, rpPost_, false, &pipes_.down0)) return false;
    if (!pipe(shaders::post_down_frag, shaders::post_down_fragSize, rpPost_, false, &pipes_.down)) return false;
    if (!pipe(shaders::post_up_frag, shaders::post_up_fragSize, rpAdd_, true, &pipes_.up)) return false;
    if (!pipe(shaders::post_mix_frag, shaders::post_mix_fragSize, rpPost_, false, &pipes_.mix)) return false;
    if (!pipe(shaders::post_feedback_frag, shaders::post_feedback_fragSize, rpPost_, false, &pipes_.feedback)) return false;
    const uint8_t clear[4] = {0, 0, 0, 0};  // empty branding layers until the app provides some
    return setOverlay(clear, 1, 1) && setBackground(clear, 1, 1);
}

bool PostChain::uploadRgba(vk::Image* img, const uint8_t* rgba, uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(ctx_->dev);  // the old image may still be referenced by frames in flight
    sets_.clear();
    vkResetDescriptorPool(ctx_->dev, pool_, 0);
    vk::destroy(*ctx_, *img);
    if (!vk::createImage2D(*ctx_, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img)) return false;
    vk::Buffer staging;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    if (!vk::createBuffer(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &staging)) return false;
    std::memcpy(staging.mapped, rgba, bytes);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx_->cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(ctx_->dev, &ai, &cmd) != VK_SUCCESS) {
        vk::destroy(*ctx_, staging);
        return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    br.image = img->img;
    br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buf, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const bool ok = vkQueueSubmit(ctx_->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
    if (ok) vkQueueWaitIdle(ctx_->queue);
    vkFreeCommandBuffers(ctx_->dev, ctx_->cmdPool, 1, &cmd);
    vk::destroy(*ctx_, staging);
    return ok;
}

bool PostChain::setOverlay(const uint8_t* rgba, uint32_t w, uint32_t h) {
    static const uint8_t clear[4] = {0, 0, 0, 0};
    return rgba && w && h ? uploadRgba(&overlay_, rgba, w, h) : uploadRgba(&overlay_, clear, 1, 1);
}

bool PostChain::setBackground(const uint8_t* rgba, uint32_t w, uint32_t h) {
    static const uint8_t clear[4] = {0, 0, 0, 0};
    return rgba && w && h ? uploadRgba(&background_, rgba, w, h) : uploadRgba(&background_, clear, 1, 1);
}

bool PostChain::setSwapchainPass(VkRenderPass pass) {
    if (pass == swapPass_ && pipes_.composite) return true;
    if (pipes_.composite) vkDestroyPipeline(ctx_->dev, pipes_.composite, nullptr);
    pipes_.composite = VK_NULL_HANDLE;
    swapPass_ = pass;
    VkShaderModule fs = vk::createShader(*ctx_, shaders::post_composite_frag, shaders::post_composite_fragSize);
    if (!fs) return false;
    pipes_.composite = makeFullscreenPipeline(*ctx_, vs_, fs, pass, layout_, false);
    vkDestroyShaderModule(ctx_->dev, fs, nullptr);
    return pipes_.composite != VK_NULL_HANDLE;
}

bool PostChain::createTarget(Target* t, uint32_t w, uint32_t h) {
    if (!vk::createImage2D(*ctx_, w, h, hdrFormat_, kTargetUsage, &t->img)) return false;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = rpPost_;  // compatible with the other two passes
    fb.attachmentCount = 1;
    fb.pAttachments = &t->img.view;
    fb.width = w;
    fb.height = h;
    fb.layers = 1;
    VK_TRY(vkCreateFramebuffer(ctx_->dev, &fb, nullptr, &t->fbScene));
    return true;
}

void PostChain::destroyTarget(Target* t) {
    if (t->fbScene) vkDestroyFramebuffer(ctx_->dev, t->fbScene, nullptr);
    vk::destroy(*ctx_, t->img);
    *t = Target{};
}

bool PostChain::resize(uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(ctx_->dev);
    for (auto& t : scene_) destroyTarget(&t);
    destroyTarget(&mix_);
    for (auto& t : fb_) destroyTarget(&t);
    for (auto& t : bloom_) destroyTarget(&t);
    vkResetDescriptorPool(ctx_->dev, pool_, 0);
    sets_.clear();
    w_ = std::max(w, 1u);
    h_ = std::max(h, 1u);
    for (auto& t : scene_) if (!createTarget(&t, w_, h_)) return false;
    if (!createTarget(&mix_, w_, h_)) return false;
    for (auto& t : fb_) if (!createTarget(&t, w_, h_)) return false;
    for (int i = 0; i < kBloomLevels; ++i)
        if (!createTarget(&bloom_[i], std::max(1u, w_ >> (i + 1)), std::max(1u, h_ >> (i + 1)))) return false;
    clearTargets();
    return true;
}

// Puts every target into SHADER_READ_ONLY with defined (black) contents, so the first frame can
// sample history/bloom images safely.
void PostChain::clearTargets() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx_->cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(ctx_->dev, &ai, &cmd) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    std::vector<Target*> all = {&scene_[0], &scene_[1], &mix_, &fb_[0], &fb_[1]};
    for (auto& b : bloom_) all.push_back(&b);
    for (Target* t : all) {
        VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        br.image = t->img.img;
        br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
        VkClearColorValue zero{};
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, t->img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
        br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx_->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx_->queue);
    vkFreeCommandBuffers(ctx_->dev, ctx_->cmdPool, 1, &cmd);
}

void PostChain::destroy() {
    if (!ctx_ || !ctx_->dev) return;
    vkDeviceWaitIdle(ctx_->dev);
    for (auto& t : scene_) destroyTarget(&t);
    destroyTarget(&mix_);
    for (auto& t : fb_) destroyTarget(&t);
    for (auto& t : bloom_) destroyTarget(&t);
    vk::destroy(*ctx_, overlay_);
    vk::destroy(*ctx_, background_);
    for (VkPipeline p : {pipes_.down0, pipes_.down, pipes_.up, pipes_.mix, pipes_.feedback, pipes_.composite})
        if (p) vkDestroyPipeline(ctx_->dev, p, nullptr);
    pipes_ = Pipelines{};
    for (VkRenderPass rp : {rpScene_, rpPost_, rpAdd_})
        if (rp) vkDestroyRenderPass(ctx_->dev, rp, nullptr);
    rpScene_ = rpPost_ = rpAdd_ = VK_NULL_HANDLE;
    if (pool_) vkDestroyDescriptorPool(ctx_->dev, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(ctx_->dev, layout_, nullptr);
    if (texLayout_) vkDestroyDescriptorSetLayout(ctx_->dev, texLayout_, nullptr);
    if (sampler_) vkDestroySampler(ctx_->dev, sampler_, nullptr);
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    texLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    sets_.clear();
    swapPass_ = VK_NULL_HANDLE;
}

VkDescriptorSet PostChain::pairSet(VkImageView a, VkImageView b) {
    const auto key = std::make_pair(a, b);
    auto it = sets_.find(key);
    if (it != sets_.end()) return it->second;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &texLayout_;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(ctx_->dev, &ai, &s) != VK_SUCCESS) {
        LOGE("descriptor pool exhausted");
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo ii[2] = {{sampler_, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {sampler_, b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = s;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(ctx_->dev, 2, w, 0, nullptr);
    sets_[key] = s;
    return s;
}

void PostChain::beginScenePass(VkCommandBuffer cmd, uint32_t slot) {
    VkClearValue clear{};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = rpScene_;
    rp.framebuffer = scene_[slot & 1].fbScene;
    rp.renderArea = {{0, 0}, {w_, h_}};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.f, 0.f, static_cast<float>(w_), static_cast<float>(h_), 0.f, 1.f};
    VkRect2D sc{{0, 0}, {w_, h_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void PostChain::endScenePass(VkCommandBuffer cmd) { vkCmdEndRenderPass(cmd); }

void PostChain::fullscreenPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkExtent2D ext,
                               VkPipeline pipe, VkDescriptorSet frameSet, VkDescriptorSet tex,
                               const float pc[4], bool clear, VkDescriptorSet extra) {
    VkClearValue cv{};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = rp;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, ext};
    rb.clearValueCount = clear ? 1 : 0;
    rb.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.f, 0.f, static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 1.f};
    VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[3] = {frameSet, tex, extra};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, extra ? 3 : 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void PostChain::record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const Look& look, bool useTransition,
                       float transition, VkFramebuffer swapFb, VkExtent2D swapExtent, VkRenderPass swapPass) {
    setSwapchainPass(swapPass);
    VkImageView src = scene_[0].img.view;
    const VkExtent2D full{w_, h_};

    if (useTransition) {
        const float pc[4] = {transition, 0, 0, 0};
        fullscreenPass(cmd, rpPost_, mix_.fbScene, full, pipes_.mix, frameSet, pairSet(scene_[0].img.view, scene_[1].img.view), pc);
        src = mix_.img.view;
    }
    if (look.feedback > 0.001f) {
        const int prev = fbCur_ ^ 1;
        const float pc[4] = {look.feedback, 0, 0, 0};
        fullscreenPass(cmd, rpPost_, fb_[fbCur_].fbScene, full, pipes_.feedback, frameSet, pairSet(src, fb_[prev].img.view), pc);
        src = fb_[fbCur_].img.view;
        fbCur_ = prev;
    }

    // Bloom: threshold + 13-tap into level 0, dual-filter chain down, additive upsample back up.
    if (look.bloom > 0.001f) {
        float pc[4] = {1.f / static_cast<float>(w_), 1.f / static_cast<float>(h_), 0.85f, 0.4f};
        fullscreenPass(cmd, rpPost_, bloom_[0].fbScene, {bloom_[0].img.w, bloom_[0].img.h}, pipes_.down0, frameSet, pairSet(src, src), pc);
        for (int i = 1; i < kBloomLevels; ++i) {
            const Target& s = bloom_[i - 1];
            const float p[4] = {1.f / static_cast<float>(s.img.w), 1.f / static_cast<float>(s.img.h), 0, 0};
            fullscreenPass(cmd, rpPost_, bloom_[i].fbScene, {bloom_[i].img.w, bloom_[i].img.h}, pipes_.down, frameSet, pairSet(s.img.view, s.img.view), p);
        }
        for (int i = kBloomLevels - 1; i > 0; --i) {
            const Target& s = bloom_[i];
            const float p[4] = {1.f / static_cast<float>(s.img.w), 1.f / static_cast<float>(s.img.h), 0.6f, 0};
            fullscreenPass(cmd, rpAdd_, bloom_[i - 1].fbScene, {bloom_[i - 1].img.w, bloom_[i - 1].img.h}, pipes_.up, frameSet, pairSet(s.img.view, s.img.view), p);
        }
    }

    const float pc[4] = {0, 0, 0, 0};
    fullscreenPass(cmd, swapPass, swapFb, swapExtent, pipes_.composite, frameSet, pairSet(src, bloom_[0].img.view), pc, false,
                   pairSet(overlay_.view, background_.view));
}

}  // namespace av
