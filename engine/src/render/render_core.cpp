#include "av/render/render_core.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>

#include "av/render/frame_data.h"
#include "fullscreen_vert.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.core", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.core", __VA_ARGS__)

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
void fillFrameData(FrameData* d, const VisualState& vs, VkExtent2D ext, float time, float dt, float overlayOpacity, float backgroundOpacity) {
    std::memset(d, 0, sizeof(*d));
    d->res[0] = static_cast<float>(ext.width);
    d->res[1] = static_cast<float>(ext.height);
    d->res[2] = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    d->res[3] = time;
    d->audio[0] = vs.rms;
    d->audio[1] = vs.kick;
    d->audio[2] = vs.beatPulse;
    d->audio[3] = vs.beatPhase;
    d->audio2[0] = static_cast<float>(vs.barBeat);
    d->audio2[1] = vs.sectionEnergy;
    d->audio2[2] = vs.bpm;
    d->audio2[3] = vs.hasTimeline ? 1.f : 0.f;
    d->fx[0] = vs.look.bloom;
    d->fx[1] = vs.look.chroma;
    d->fx[2] = vs.look.grain;
    d->fx[3] = vs.look.feedback;
    d->fx2[0] = vs.look.vignette;
    d->fx2[1] = vs.look.glitch;
    d->fx2[2] = vs.look.exposure;
    d->fx2[3] = dt;
    std::memcpy(d->pal, vs.look.palette, sizeof(d->pal));
    std::memcpy(d->bands, vs.bands, sizeof(d->bands));
    std::memcpy(d->bars, vs.bars, sizeof(d->bars));
    std::memcpy(d->wave, vs.wave, sizeof(d->wave));
    d->ctl[0] = vs.transition;
    d->ctl[1] = vs.sceneTime;
    d->ctl[2] = static_cast<float>(std::max<uint32_t>(vs.numBars, 1));
    d->brand[0] = overlayOpacity;
    d->brand[1] = backgroundOpacity;
}
}  // namespace

bool RenderCore::create(vk::Context* ctx) {
    ctx_ = ctx;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(ctx->dev, &dl, nullptr, &frameLayout_));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 32;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VK_TRY(vkCreateDescriptorPool(ctx->dev, &dp, nullptr, &dpool_));

    for (Slot& s : slots_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = ctx->cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(ctx->dev, &ai, &s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_TRY(vkCreateFence(ctx->dev, &fi, nullptr, &s.fence));
        if (!vk::createBuffer(*ctx, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &s.ubo)) return false;
        std::memset(s.ubo.mapped, 0, sizeof(FrameData));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &frameLayout_;
        VK_TRY(vkAllocateDescriptorSets(ctx->dev, &dai, &s.set));
        VkDescriptorBufferInfo dbi{s.ubo.buf, 0, sizeof(FrameData)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(ctx->dev, 1, &w, 0, nullptr);
    }
    fullscreenVs_ = vk::createShader(*ctx, shaders::fullscreen_vert, shaders::fullscreen_vertSize);
    if (!fullscreenVs_) return false;
    if (!post_.create(*ctx, fullscreenVs_, frameLayout_)) return false;

    scenes_ = createBuiltinScenes();
    SceneEnv env{ctx, post_.scenePass(), frameLayout_, dpool_, fullscreenVs_};
    for (auto& s : scenes_) {
        if (!s->create(env)) {
            LOGE("scene '%s' failed to create", s->name());
            return false;
        }
    }
    LOGI("%zu scene(s) ready", scenes_.size());
    return true;
}

void RenderCore::destroy() {
    if (!ctx_ || !ctx_->dev) return;
    vkDeviceWaitIdle(ctx_->dev);
    for (auto& s : scenes_) s->destroy(*ctx_);
    scenes_.clear();
    post_.destroy();
    for (Slot& s : slots_) {
        vk::destroy(*ctx_, s.ubo);
        if (s.fence) vkDestroyFence(ctx_->dev, s.fence, nullptr);
        s = Slot{};
    }
    if (fullscreenVs_) vkDestroyShaderModule(ctx_->dev, fullscreenVs_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(ctx_->dev, dpool_, nullptr);
    if (frameLayout_) vkDestroyDescriptorSetLayout(ctx_->dev, frameLayout_, nullptr);
    fullscreenVs_ = VK_NULL_HANDLE;
    dpool_ = VK_NULL_HANDLE;
    frameLayout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void RenderCore::record(uint32_t i, const VisualState& vs, float timeSec, float dt, VkFramebuffer fb,
                        VkExtent2D extent, VkRenderPass pass, float quality) {
    Slot& s = slot(i);
    const VkExtent2D internal = post_.extent();
    fillFrameData(static_cast<FrameData*>(s.ubo.mapped), vs, internal, timeSec, dt, overlayOpacity_, backgroundOpacity_);

    Scene* a = scenes_[vs.sceneA % scenes_.size()].get();
    Scene* b = scenes_[vs.sceneB % scenes_.size()].get();
    const bool transitioning = vs.transition > 0.001f && a != b;
    FrameInfo fi;
    fi.frameIndex = i % kSlots;
    fi.frameSet = s.set;
    fi.extent = internal;
    fi.time = timeSec;
    fi.dt = dt;
    fi.sceneTime = vs.sceneTime;
    fi.quality = quality;

    a->prepare(s.cmd, fi);
    if (transitioning) b->prepare(s.cmd, fi);
    post_.beginScenePass(s.cmd, 0);
    a->draw(s.cmd, fi);
    post_.endScenePass(s.cmd);
    if (transitioning) {
        post_.beginScenePass(s.cmd, 1);
        b->draw(s.cmd, fi);
        post_.endScenePass(s.cmd);
    }
    post_.record(s.cmd, s.set, vs.look, transitioning, vs.transition, fb, extent, pass);
}

}  // namespace av
