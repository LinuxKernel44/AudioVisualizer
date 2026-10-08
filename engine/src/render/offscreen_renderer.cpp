#include "av/render/offscreen_renderer.h"

#include <android/log.h>

#include <algorithm>

#include "nv12_comp.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.offscreen", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.offscreen", __VA_ARGS__)

#define VK_TRY(expr)                                                      \
    do {                                                                  \
        const VkResult r_ = (expr);                                       \
        if (r_ != VK_SUCCESS) {                                           \
            LOGE("%s failed: %d (%s:%d)", #expr, r_, __FILE__, __LINE__); \
            return false;                                                 \
        }                                                                 \
    } while (0)

namespace av {

bool OffscreenRenderer::create(uint32_t width, uint32_t height, float internalScale) {
    if (width % 4 != 0 || height % 2 != 0 || width < 16 || height < 16) {
        LOGE("unsupported size %ux%u", width, height);
        return false;
    }
    w_ = width;
    h_ = height;
    if (!ctx_.createInstance() || !ctx_.createDevice()) return false;
    if (!core_.create(&ctx_)) return false;
    const auto even = [](float v) { return std::max(2u, static_cast<uint32_t>(v) & ~1u); };
    if (!core_.resize(even(static_cast<float>(w_) * internalScale),
                      even(static_cast<float>(h_) * internalScale)))
        return false;

    // Final pass: RGBA8, left in SHADER_READ_ONLY so the compute shader can sample it.
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VK_TRY(vkCreateRenderPass(ctx_.dev, &rpi, nullptr, &finalPass_));

    // Compute pipeline: sampler2D + storage buffer, push constants (w, h).
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_TRY(vkCreateSampler(ctx_.dev, &si, nullptr, &sampler_));
    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding = 0;
    lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lb[0].descriptorCount = 1;
    lb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    lb[1].binding = 1;
    lb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lb[1].descriptorCount = 1;
    lb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = lb;
    VK_TRY(vkCreateDescriptorSetLayout(ctx_.dev, &dl, nullptr, &cl_));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &cl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_TRY(vkCreatePipelineLayout(ctx_.dev, &pl, nullptr, &pl_));
    VkShaderModule cs = vk::createShader(ctx_, shaders::nv12_comp, shaders::nv12_compSize);
    if (!cs) return false;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                cs,
                "main",
                nullptr};
    cp.layout = pl_;
    const VkResult cr =
        vkCreateComputePipelines(ctx_.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.dev, cs, nullptr);
    if (cr != VK_SUCCESS) {
        LOGE("compute pipeline failed: %d", cr);
        return false;
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RenderCore::kSlots},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RenderCore::kSlots}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = RenderCore::kSlots;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VK_TRY(vkCreateDescriptorPool(ctx_.dev, &dp, nullptr, &pool_));

    for (Slot& s : slots_) {
        if (!vk::createImage2D(ctx_, w_, h_, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               &s.final))
            return false;
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = finalPass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &s.final.view;
        fb.width = w_;
        fb.height = h_;
        fb.layers = 1;
        VK_TRY(vkCreateFramebuffer(ctx_.dev, &fb, nullptr, &s.fb));
        if (!vk::createBuffer(ctx_, frameBytes(),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              false, &s.storage))
            return false;
        if (!vk::createBuffer(ctx_, frameBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                              &s.readback, true))
            return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &cl_;
        VK_TRY(vkAllocateDescriptorSets(ctx_.dev, &ai, &s.set));
        VkDescriptorImageInfo ii{sampler_, s.final.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi{s.storage.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = w[1].dstSet = s.set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &ii;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx_.dev, 2, w, 0, nullptr);
    }
    ready_ = true;
    LOGI("offscreen %ux%u ready on %s", w_, h_, ctx_.props.deviceName);
    return true;
}

void OffscreenRenderer::destroy() {
    if (ctx_.dev) {
        vkDeviceWaitIdle(ctx_.dev);
        for (Slot& s : slots_) {
            if (s.fb) vkDestroyFramebuffer(ctx_.dev, s.fb, nullptr);
            vk::destroy(ctx_, s.final);
            vk::destroy(ctx_, s.storage);
            vk::destroy(ctx_, s.readback);
            s = Slot{};
        }
        if (pipeline_) vkDestroyPipeline(ctx_.dev, pipeline_, nullptr);
        if (pl_) vkDestroyPipelineLayout(ctx_.dev, pl_, nullptr);
        if (pool_) vkDestroyDescriptorPool(ctx_.dev, pool_, nullptr);
        if (cl_) vkDestroyDescriptorSetLayout(ctx_.dev, cl_, nullptr);
        if (sampler_) vkDestroySampler(ctx_.dev, sampler_, nullptr);
        if (finalPass_) vkDestroyRenderPass(ctx_.dev, finalPass_, nullptr);
        core_.destroy();
    }
    ctx_.destroy();
    pipeline_ = VK_NULL_HANDLE;
    pl_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    cl_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    finalPass_ = VK_NULL_HANDLE;
    ready_ = false;
}

bool OffscreenRenderer::deliver(Slot& s, const FrameSink& sink) {
    if (!s.pending) return true;
    RenderCore::Slot& rs = core_.slot(static_cast<uint32_t>(&s - slots_));
    VK_TRY(vkWaitForFences(ctx_.dev, 1, &rs.fence, VK_TRUE, UINT64_MAX));
    if (!s.readback.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s.readback.mem, 0,
                                  VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(ctx_.dev, 1, &range);
    }
    sink(s.frame, static_cast<const uint8_t*>(s.readback.mapped));
    s.pending = false;
    return true;
}

bool OffscreenRenderer::render(int64_t frameIndex, const VisualState& vs, float timeSec, float dt,
                               const FrameSink& sink) {
    if (!ready_) return false;
    const uint32_t si = static_cast<uint32_t>(frameIndex % RenderCore::kSlots);
    Slot& s = slots_[si];
    RenderCore::Slot& rs = core_.slot(si);
    // This slot's previous frame must have been delivered before its buffers are reused.
    if (!deliver(s, sink)) return false;
    VK_TRY(vkWaitForFences(ctx_.dev, 1, &rs.fence, VK_TRUE, UINT64_MAX));
    vkResetFences(ctx_.dev, 1, &rs.fence);

    vkResetCommandBuffer(rs.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(rs.cmd, &bi);
    core_.record(si, vs, timeSec, dt, s.fb, {w_, h_}, finalPass_);

    vkCmdBindPipeline(rs.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(rs.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl_, 0, 1, &s.set, 0, nullptr);
    const uint32_t pc[2] = {w_, h_};
    vkCmdPushConstants(rs.cmd, pl_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(rs.cmd, (w_ / 4 + 15) / 16, (h_ + 7) / 8, 1);

    VkBufferMemoryBarrier toCopy{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.buffer = s.storage.buf;
    toCopy.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(rs.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &toCopy, 0, nullptr);
    VkBufferCopy region{0, 0, frameBytes()};
    vkCmdCopyBuffer(rs.cmd, s.storage.buf, s.readback.buf, 1, &region);
    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = s.readback.buf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(rs.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(rs.cmd);

    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &rs.cmd;
    VK_TRY(vkQueueSubmit(ctx_.queue, 1, &sub, rs.fence));
    s.frame = frameIndex;
    s.pending = true;

    // While this frame renders, hand over the previous one.
    Slot& prev = slots_[(si + RenderCore::kSlots - 1) % RenderCore::kSlots];
    return deliver(prev, sink);
}

bool OffscreenRenderer::flush(const FrameSink& sink) {
    // Deliver in frame order.
    Slot* order[RenderCore::kSlots];
    for (uint32_t i = 0; i < RenderCore::kSlots; ++i) order[i] = &slots_[i];
    std::sort(order, order + RenderCore::kSlots,
              [](Slot* a, Slot* b) { return a->frame < b->frame; });
    for (Slot* s : order)
        if (!deliver(*s, sink)) return false;
    return true;
}

}  // namespace av
