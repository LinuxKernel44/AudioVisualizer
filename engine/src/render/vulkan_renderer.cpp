#include "av/render/vulkan_renderer.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "av/render/frame_data.h"
#include "av/render/post_chain.h"
#include "av/render/scene.h"
#include "av/render/vk_context.h"
#include "fullscreen_vert.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.vk", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.vk", __VA_ARGS__)

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
constexpr uint32_t kFramesInFlight = 2;

void fillFrameData(FrameData* d, const VisualState& vs, VkExtent2D ext, float time, float dt) {
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
}
}  // namespace

struct VulkanRenderer::Vk {
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        vk::Buffer ubo;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    vk::Context ctx;
    bool deviceReady = false;
    VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkShaderModule fullscreenVs = VK_NULL_HANDLE;
    Frame frames[kFramesInFlight];
    PostChain post;
    std::vector<std::unique_ptr<Scene>> scenes;
    bool resourcesReady = false;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone;
    VkRenderPass swapPass = VK_NULL_HANDLE;
    VkFormat swapPassFormat = VK_FORMAT_UNDEFINED;
    ANativeWindow* window = nullptr;
    uint32_t frameIndex = 0;
    float lastScale = 0.f;

    bool createDeviceResources();
    bool createSurface(ANativeWindow* w);
    bool createSwapchain(float scale);
    bool ensureSwapPass();
    void destroySwapchain();
    void destroySurface();
    void destroyAll();
    bool recreateSwapchain(float scale);
    bool drawFrame(const VisualState& vs, float timeSec, float dt, float scale);
};

bool VulkanRenderer::Vk::createSurface(ANativeWindow* w) {
    window = w;
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = w;
    VK_TRY(vkCreateAndroidSurfaceKHR(ctx.instance, &ci, nullptr, &surface));
    return true;
}

bool VulkanRenderer::Vk::createDeviceResources() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(ctx.dev, &dl, nullptr, &frameLayout));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 32;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VK_TRY(vkCreateDescriptorPool(ctx.dev, &dp, nullptr, &dpool));

    for (Frame& f : frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = ctx.cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(ctx.dev, &ai, &f.cmd));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(ctx.dev, &si, nullptr, &f.acquire));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_TRY(vkCreateFence(ctx.dev, &fi, nullptr, &f.fence));
        if (!vk::createBuffer(ctx, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &f.ubo)) return false;
        std::memset(f.ubo.mapped, 0, sizeof(FrameData));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &frameLayout;
        VK_TRY(vkAllocateDescriptorSets(ctx.dev, &dai, &f.set));
        VkDescriptorBufferInfo dbi{f.ubo.buf, 0, sizeof(FrameData)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(ctx.dev, 1, &w, 0, nullptr);
    }
    fullscreenVs = vk::createShader(ctx, shaders::fullscreen_vert, shaders::fullscreen_vertSize);
    if (!fullscreenVs) return false;
    if (!post.create(ctx, fullscreenVs, frameLayout)) return false;

    scenes = createBuiltinScenes();
    SceneEnv env{&ctx, post.scenePass(), frameLayout, dpool, fullscreenVs};
    for (auto& s : scenes) {
        if (!s->create(env)) {
            LOGE("scene '%s' failed to create", s->name());
            return false;
        }
    }
    LOGI("%zu scene(s) ready", scenes.size());
    resourcesReady = true;
    return true;
}

bool VulkanRenderer::Vk::ensureSwapPass() {
    if (swapPass && swapPassFormat == format) return true;
    if (swapPass) vkDestroyRenderPass(ctx.dev, swapPass, nullptr);
    swapPass = VK_NULL_HANDLE;
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the composite pass overwrites every pixel
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK_TRY(vkCreateRenderPass(ctx.dev, &rpi, nullptr, &swapPass));
    swapPassFormat = format;
    return true;
}

bool VulkanRenderer::Vk::createSwapchain(float scale) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.phys, surface, &caps));
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys, surface, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
    for (const auto& f : fmts) {
        // UNORM: the composite shader does its own sRGB encoding.
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    format = chosen.format;
    extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
        extent.height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    }
    if (extent.width == 0 || extent.height == 0) return false;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface;
    sc.minImageCount = imageCount;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // We render upright into the window-oriented extent and let the compositor rotate.
    // (Pre-rotation would save a compositor pass; tracked for the performance milestone.)
    sc.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    sc.compositeAlpha = alpha;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VK_TRY(vkCreateSwapchainKHR(ctx.dev, &sc, nullptr, &swapchain));
    LOGI("swapchain %ux%u fmt=%d images=%u", extent.width, extent.height, format, imageCount);

    if (!ensureSwapPass()) return false;
    // Internal render size (dimensions kept even for the bloom chain).
    const auto even = [](float v) { return std::max(2u, static_cast<uint32_t>(v) & ~1u); };
    const uint32_t iw = even(static_cast<float>(extent.width) * scale);
    const uint32_t ih = even(static_cast<float>(extent.height) * scale);
    if (!post.resize(iw, ih)) return false;
    lastScale = scale;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.dev, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(ctx.dev, swapchain, &n, images.data());
    views.resize(n);
    framebuffers.resize(n);
    renderDone.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_TRY(vkCreateImageView(ctx.dev, &vi, nullptr, &views[i]));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = swapPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &views[i];
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        VK_TRY(vkCreateFramebuffer(ctx.dev, &fb, nullptr, &framebuffers[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(ctx.dev, &si, nullptr, &renderDone[i]));
    }
    return true;
}

void VulkanRenderer::Vk::destroySwapchain() {
    if (!ctx.dev) return;
    vkDeviceWaitIdle(ctx.dev);
    for (VkSemaphore s : renderDone) vkDestroySemaphore(ctx.dev, s, nullptr);
    for (VkFramebuffer f : framebuffers) vkDestroyFramebuffer(ctx.dev, f, nullptr);
    for (VkImageView v : views) vkDestroyImageView(ctx.dev, v, nullptr);
    renderDone.clear();
    framebuffers.clear();
    views.clear();
    images.clear();
    if (swapchain) vkDestroySwapchainKHR(ctx.dev, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void VulkanRenderer::Vk::destroySurface() {
    destroySwapchain();
    if (surface) vkDestroySurfaceKHR(ctx.instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    window = nullptr;
}

bool VulkanRenderer::Vk::recreateSwapchain(float scale) {
    destroySwapchain();
    return createSwapchain(scale);
}

void VulkanRenderer::Vk::destroyAll() {
    destroySurface();
    if (ctx.dev) {
        vkDeviceWaitIdle(ctx.dev);
        for (auto& s : scenes) s->destroy(ctx);
        scenes.clear();
        post.destroy();
        if (swapPass) vkDestroyRenderPass(ctx.dev, swapPass, nullptr);
        for (Frame& f : frames) {
            vk::destroy(ctx, f.ubo);
            if (f.acquire) vkDestroySemaphore(ctx.dev, f.acquire, nullptr);
            if (f.fence) vkDestroyFence(ctx.dev, f.fence, nullptr);
        }
        if (fullscreenVs) vkDestroyShaderModule(ctx.dev, fullscreenVs, nullptr);
        if (dpool) vkDestroyDescriptorPool(ctx.dev, dpool, nullptr);
        if (frameLayout) vkDestroyDescriptorSetLayout(ctx.dev, frameLayout, nullptr);
    }
    ctx.destroy();
}

bool VulkanRenderer::Vk::drawFrame(const VisualState& vs, float timeSec, float dt, float scale) {
    if (std::fabs(scale - lastScale) > 0.01f && !recreateSwapchain(scale)) return false;
    Frame& f = frames[frameIndex];
    vkWaitForFences(ctx.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);

    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(ctx.dev, swapchain, UINT64_MAX, f.acquire, VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreateSwapchain(scale);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed: %d", r);
        return false;
    }
    vkResetFences(ctx.dev, 1, &f.fence);

    const VkExtent2D internal = post.extent();
    fillFrameData(static_cast<FrameData*>(f.ubo.mapped), vs, internal, timeSec, dt);

    const size_t nScenes = scenes.size();
    Scene* a = scenes[vs.sceneA % nScenes].get();
    Scene* b = scenes[vs.sceneB % nScenes].get();
    const bool transitioning = vs.transition > 0.001f && a != b;
    FrameInfo fi;
    fi.frameIndex = frameIndex;
    fi.frameSet = f.set;
    fi.extent = internal;
    fi.time = timeSec;
    fi.dt = dt;
    fi.sceneTime = vs.sceneTime;

    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    a->prepare(f.cmd, fi);
    if (transitioning) b->prepare(f.cmd, fi);
    post.beginScenePass(f.cmd, 0);
    a->draw(f.cmd, fi);
    post.endScenePass(f.cmd);
    if (transitioning) {
        post.beginScenePass(f.cmd, 1);
        b->draw(f.cmd, fi);
        post.endScenePass(f.cmd);
    }
    post.record(f.cmd, f.set, vs.look, transitioning, vs.transition, framebuffers[img], extent, swapPass);
    vkEndCommandBuffer(f.cmd);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquire;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[img];
    if (vkQueueSubmit(ctx.queue, 1, &si, f.fence) != VK_SUCCESS) return false;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &img;
    r = vkQueuePresentKHR(ctx.queue, &pi);
    frameIndex = (frameIndex + 1) % kFramesInFlight;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return recreateSwapchain(scale);
    return r == VK_SUCCESS;
}

// ---------------------------------------------------------------------------------------------

VulkanRenderer::VulkanRenderer(TripleBuffer<VisualState>& state) : state_(state) {}

VulkanRenderer::~VulkanRenderer() { stop(); }

void VulkanRenderer::start() {
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void VulkanRenderer::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VulkanRenderer::setSurface(ANativeWindow* window) {
    if (window) ANativeWindow_acquire(window);
    std::unique_lock<std::mutex> lk(mu_);
    if (pending_ && surfaceDirty_) ANativeWindow_release(pending_);  // superseded, never applied
    pending_ = window;
    surfaceDirty_ = true;
    cv_.notify_all();
    cv_.wait(lk, [this] { return !surfaceDirty_ || stop_.load(); });
}

void VulkanRenderer::run() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
    Vk vk;
    ANativeWindow* current = nullptr;
    bool instanceOk = vk.ctx.createInstance();
    bool active = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastFrame = t0;
    int statFrames = 0;
    double statSum = 0.0;

    while (instanceOk) {
        ANativeWindow* incoming = nullptr;
        bool changed = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!active && !surfaceDirty_ && !stop_.load())
                cv_.wait(lk, [this] { return surfaceDirty_ || stop_.load(); });
            if (stop_.load()) break;
            if (surfaceDirty_) {
                incoming = pending_;
                pending_ = nullptr;
                changed = true;
            }
        }
        if (changed) {
            active = false;
            vk.destroySurface();
            if (current) ANativeWindow_release(current);
            current = incoming;
            if (current && vk.createSurface(current)) {
                if (!vk.deviceReady) vk.deviceReady = vk.ctx.createDevice(vk.surface) && vk.createDeviceResources();
                active = vk.deviceReady && vk.createSwapchain(renderScale_.load());
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                surfaceDirty_ = false;
            }
            cv_.notify_all();
        }
        if (active) {
            const auto now = std::chrono::steady_clock::now();
            const float t = std::chrono::duration<float>(now - t0).count();
            const float dt = std::chrono::duration<float>(now - lastFrame).count();
            lastFrameMs_ = dt * 1000.f;
            lastFrame = now;
            statFrames++;
            statSum += dt;
            if (statSum >= 3.0) {  // periodic cadence report (average frame interval and fps)
                LOGI("frame interval %.2f ms (%.1f fps) at render scale %.2f", statSum / statFrames * 1000.0,
                     statFrames / statSum, static_cast<double>(renderScale_.load()));
                statFrames = 0;
                statSum = 0.0;
            }
            if (!vk.drawFrame(state_.read(), t, std::min(dt, 0.1f), renderScale_.load())) {
                LOGE("frame failed; waiting for a new surface");
                active = false;
                vk.destroySurface();
            }
        }
    }

    vk.destroyAll();
    if (current) ANativeWindow_release(current);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_) ANativeWindow_release(pending_);
        pending_ = nullptr;
        surfaceDirty_ = false;
    }
    cv_.notify_all();
}

}  // namespace av
