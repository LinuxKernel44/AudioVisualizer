#pragma once

#include <memory>
#include <vector>

#include "av/render/vk_context.h"

namespace av {

/// What a scene needs to create its pipelines.
struct SceneEnv {
    const vk::Context* ctx = nullptr;
    VkRenderPass pass = VK_NULL_HANDLE;           // HDR scene render pass (one RGBA16F attachment)
    VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE;  // set 0: FrameData uniform block
    VkDescriptorPool pool = VK_NULL_HANDLE;       // for scene-owned descriptor sets
    VkShaderModule fullscreenVert = VK_NULL_HANDLE;
};

struct FrameInfo {
    uint32_t frameIndex = 0;                 // frame-in-flight slot
    VkDescriptorSet frameSet = VK_NULL_HANDLE;
    VkExtent2D extent{};
    float time = 0.f;
    float dt = 0.f;
    float sceneTime = 0.f;
    float quality = 1.f;  // 0.25..1: scenes may scale their workload (e.g. particle count)
};

/// A visual scene. prepare() runs outside any render pass (compute), draw() inside the HDR pass.
class Scene {
public:
    virtual ~Scene() = default;
    virtual const char* name() const = 0;
    virtual bool create(const SceneEnv& env) = 0;
    virtual void destroy(const vk::Context& ctx) = 0;
    virtual void prepare(VkCommandBuffer, const FrameInfo&) {}
    virtual void draw(VkCommandBuffer cmd, const FrameInfo& f) = 0;
};

/// A scene that is a single fragment shader over the whole screen.
class FullscreenScene : public Scene {
public:
    FullscreenScene(const char* name, const uint32_t* spirv, size_t bytes)
        : name_(name), spirv_(spirv), bytes_(bytes) {}
    const char* name() const override { return name_; }
    bool create(const SceneEnv& env) override;
    void destroy(const vk::Context& ctx) override;
    void draw(VkCommandBuffer cmd, const FrameInfo& f) override;

private:
    const char* name_;
    const uint32_t* spirv_;
    size_t bytes_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

std::unique_ptr<Scene> createParticleScene();

/// All built-in scenes, in director order.
std::vector<std::unique_ptr<Scene>> createBuiltinScenes();

/// Shared helper: fullscreen-triangle graphics pipeline (no vertex input, dynamic viewport).
VkPipeline makeFullscreenPipeline(const vk::Context& ctx, VkShaderModule vs, VkShaderModule fs,
                                  VkRenderPass pass, VkPipelineLayout layout, bool additive);

}  // namespace av
