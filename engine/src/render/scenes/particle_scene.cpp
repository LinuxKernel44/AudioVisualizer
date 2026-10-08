#include <android/log.h>

#include "av/render/scene.h"
#include "particles_comp.h"
#include "particles_frag.h"
#include "particles_vert.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.particles", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.particles", __VA_ARGS__)

namespace av {

namespace {

constexpr uint32_t kParticleCount =
    262'144;  // 2^18 (maximum; the active count follows FrameInfo::quality)
constexpr VkDeviceSize kParticleBytes = 32;  // two vec4

/// GPU particle storm: a compute pass advances ~260k particles through a flow field with
/// audio-driven forces; a graphics pass draws them as additive soft sprites.
class ParticleScene final : public Scene {
   public:
    const char* name() const override { return "particle-storm"; }

    bool create(const SceneEnv& env) override {
        const vk::Context& ctx = *env.ctx;
        if (!vk::createBuffer(ctx, kParticleCount * kParticleBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &buffer_))
            return false;

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &b;
        if (vkCreateDescriptorSetLayout(ctx.dev, &dl, nullptr, &setLayout_) != VK_SUCCESS)
            return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = env.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        if (vkAllocateDescriptorSets(ctx.dev, &ai, &set_) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo bi{buffer_.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx.dev, 1, &w, 0, nullptr);

        VkDescriptorSetLayout layouts[2] = {env.frameLayout, setLayout_};
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo cpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cpl.setLayoutCount = 2;
        cpl.pSetLayouts = layouts;
        cpl.pushConstantRangeCount = 1;
        cpl.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(ctx.dev, &cpl, nullptr, &computeLayout_) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo gpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        gpl.setLayoutCount = 2;
        gpl.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(ctx.dev, &gpl, nullptr, &drawLayout_) != VK_SUCCESS)
            return false;

        VkShaderModule cs =
            vk::createShader(ctx, shaders::particles_comp, shaders::particles_compSize);
        VkShaderModule vs =
            vk::createShader(ctx, shaders::particles_vert, shaders::particles_vertSize);
        VkShaderModule fs =
            vk::createShader(ctx, shaders::particles_frag, shaders::particles_fragSize);
        if (!cs || !vs || !fs) return false;
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    cs,
                    "main",
                    nullptr};
        cp.layout = computeLayout_;
        const VkResult cr =
            vkCreateComputePipelines(ctx.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &computePipe_);
        drawPipe_ = makeFullscreenPipeline(ctx, vs, fs, env.pass, drawLayout_, true);
        vkDestroyShaderModule(ctx.dev, cs, nullptr);
        vkDestroyShaderModule(ctx.dev, vs, nullptr);
        vkDestroyShaderModule(ctx.dev, fs, nullptr);
        if (cr != VK_SUCCESS || !drawPipe_) {
            LOGE("pipeline creation failed (%d)", cr);
            return false;
        }
        return true;
    }

    void destroy(const vk::Context& ctx) override {
        if (computePipe_) vkDestroyPipeline(ctx.dev, computePipe_, nullptr);
        if (drawPipe_) vkDestroyPipeline(ctx.dev, drawPipe_, nullptr);
        if (computeLayout_) vkDestroyPipelineLayout(ctx.dev, computeLayout_, nullptr);
        if (drawLayout_) vkDestroyPipelineLayout(ctx.dev, drawLayout_, nullptr);
        if (setLayout_) vkDestroyDescriptorSetLayout(ctx.dev, setLayout_, nullptr);
        vk::destroy(ctx, buffer_);
        computePipe_ = drawPipe_ = VK_NULL_HANDLE;
        computeLayout_ = drawLayout_ = VK_NULL_HANDLE;
        setLayout_ = VK_NULL_HANDLE;
    }

    static uint32_t activeCount(float quality) {
        const float q = quality < 0.25f ? 0.25f : quality > 1.f ? 1.f : quality;
        return static_cast<uint32_t>(kParticleCount * (0.25f + 0.75f * (q - 0.25f) / 0.75f)) &
               ~255u;
    }

    void prepare(VkCommandBuffer cmd, const FrameInfo& f) override {
        active_ = activeCount(f.quality);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipe_);
        VkDescriptorSet sets[2] = {f.frameSet, set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 2, sets, 0,
                                nullptr);
        if (!initialised_) {
            dispatch(cmd, 0);
            barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            initialised_ = true;
        }
        // Previous frame's vertex reads must finish before this frame's compute writes.
        barrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        dispatch(cmd, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }

    void draw(VkCommandBuffer cmd, const FrameInfo& f) override {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipe_);
        VkDescriptorSet sets[2] = {f.frameSet, set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout_, 0, 2, sets, 0,
                                nullptr);
        vkCmdDraw(cmd, 6, active_, 0, 0);
    }

   private:
    void dispatch(VkCommandBuffer cmd, uint32_t mode) {
        const uint32_t pc[2] = {mode, mode == 0 ? kParticleCount : active_};
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        vkCmdDispatch(cmd, ((mode == 0 ? kParticleCount : active_) + 255) / 256, 1, 1);
    }
    static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, src, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }

    vk::Buffer buffer_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout_ = VK_NULL_HANDLE, drawLayout_ = VK_NULL_HANDLE;
    VkPipeline computePipe_ = VK_NULL_HANDLE, drawPipe_ = VK_NULL_HANDLE;
    bool initialised_ = false;
    uint32_t active_ = kParticleCount;
};

}  // namespace

std::unique_ptr<Scene> createParticleScene() { return std::make_unique<ParticleScene>(); }

}  // namespace av
