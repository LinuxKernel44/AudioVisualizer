#include "av/render/vulkan_renderer.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "av/render/render_core.h"
#include "av/render/vk_context.h"

#include <android/performance_hint.h>

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

}  // namespace

struct VulkanRenderer::Vk {
    vk::Context ctx;
    bool deviceReady = false;
    RenderCore core;
    VkSemaphore acquire[RenderCore::kSlots] = {};
    VkQueryPool qpool = VK_NULL_HANDLE;
    bool slotUsed[RenderCore::kSlots] = {};
    float gpuMs = 0.f;

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
    bool applyScale(float scale);
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
    if (!core.create(&ctx)) return false;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& sem : acquire) VK_TRY(vkCreateSemaphore(ctx.dev, &si, nullptr, &sem));
    if (ctx.timestampValidBits > 0) {  // GPU time measurement for the dynamic-quality controller
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = RenderCore::kSlots * 2;
        VK_TRY(vkCreateQueryPool(ctx.dev, &qi, nullptr, &qpool));
    }
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
    if (!core.resize(iw, ih)) return false;
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
        for (VkSemaphore s : acquire)
            if (s) vkDestroySemaphore(ctx.dev, s, nullptr);
        if (qpool) vkDestroyQueryPool(ctx.dev, qpool, nullptr);
        core.destroy();
        if (swapPass) vkDestroyRenderPass(ctx.dev, swapPass, nullptr);
    }
    ctx.destroy();
}

bool VulkanRenderer::Vk::applyScale(float scale) {
    // Only the HDR working targets depend on the scale; the swapchain stays as it is.
    const auto even = [](float v) { return std::max(2u, static_cast<uint32_t>(v) & ~1u); };
    vkDeviceWaitIdle(ctx.dev);
    if (!core.resize(even(static_cast<float>(extent.width) * scale), even(static_cast<float>(extent.height) * scale))) return false;
    lastScale = scale;
    return true;
}

bool VulkanRenderer::Vk::drawFrame(const VisualState& vs, float timeSec, float dt, float scale) {
    if (std::fabs(scale - lastScale) > 0.01f && !applyScale(scale)) return false;
    RenderCore::Slot& f = core.slot(frameIndex);
    vkWaitForFences(ctx.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (qpool && slotUsed[frameIndex]) {  // timestamps written when this slot last ran
        uint64_t t[2];
        if (vkGetQueryPoolResults(ctx.dev, qpool, frameIndex * 2, 2, sizeof t, t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t mask = ctx.timestampValidBits >= 64 ? ~0ull : ((1ull << ctx.timestampValidBits) - 1);
            gpuMs = static_cast<float>(static_cast<double>((t[1] - t[0]) & mask) * ctx.props.limits.timestampPeriod / 1e6);
        }
    }

    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(ctx.dev, swapchain, UINT64_MAX, acquire[frameIndex], VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreateSwapchain(scale);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed: %d", r);
        return false;
    }
    vkResetFences(ctx.dev, 1, &f.fence);

    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    if (qpool) {
        vkCmdResetQueryPool(f.cmd, qpool, frameIndex * 2, 2);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, frameIndex * 2);
    }
    core.record(frameIndex, vs, timeSec, dt, framebuffers[img], extent, swapPass, std::clamp(scale, 0.25f, 1.f));
    if (qpool) {
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, frameIndex * 2 + 1);
        slotUsed[frameIndex] = true;
    }
    vkEndCommandBuffer(f.cmd);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquire[frameIndex];
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
    frameIndex = (frameIndex + 1) % RenderCore::kSlots;
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

void VulkanRenderer::setOverlayImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(layerMu_);
    overlay_ = {std::move(rgba), w, h, true};
    layersDirty_ = true;
}

void VulkanRenderer::setBackgroundImage(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(layerMu_);
    background_ = {std::move(rgba), w, h, true};
    layersDirty_ = true;
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

    // Dynamic quality: GPU time vs. the display period drives the internal render scale; the
    // thermal status caps it. ADPF lets the system schedule this thread for the frame budget.
    float scale = std::min(1.f, renderScale_.load());
    constexpr float kMinScale = 0.5f;
    double winGpu = 0.0, winDt = 0.0;
    int winN = 0, goodWindows = 0;
    APerformanceHintManager* hintMgr = nullptr;
    APerformanceHintSession* hint = nullptr;
    float hintFps = 0.f;
    bool appliedSaver = false;

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
                LOGI("frame interval %.2f ms (%.1f fps), GPU %.2f ms, render scale %.2f", statSum / statFrames * 1000.0,
                     statFrames / statSum, static_cast<double>(vk.gpuMs), static_cast<double>(scale));
                statFrames = 0;
                statSum = 0.0;
            }
            const float fpsTarget = powerSaver_.load() ? std::min(30.f, targetFps_.load()) : targetFps_.load();
            const int thermal = thermal_.load();
            const float thermalCap = thermal >= 5 ? 0.5f : thermal == 4 ? 0.55f : thermal == 3 ? 0.7f : thermal == 2 ? 0.85f : 1.f;
            const float userMax = std::min(1.f, renderScale_.load());
            const bool saver = powerSaver_.load();
            const float cap = std::min({userMax, thermalCap, saver ? 0.7f : 1.f});
            if (scale > cap) scale = std::max(kMinScale, cap);
            if (__builtin_available(android 33, *)) {
                if (!hint && !hintMgr) hintMgr = APerformanceHint_getManager();
                if (hintMgr && (!hint || std::fabs(hintFps - fpsTarget) > 1.f)) {
                    if (hint) APerformanceHint_closeSession(hint);
                    const int32_t tid = gettid();
                    hint = APerformanceHint_createSession(hintMgr, &tid, 1, static_cast<int64_t>(0.9e9 / fpsTarget));
                    hintFps = fpsTarget;
                }
            }
            if (layersDirty_.exchange(false)) {
                std::lock_guard<std::mutex> lk(layerMu_);
                if (overlay_.dirty) {
                    vk.core.setOverlay(overlay_.rgba.empty() ? nullptr : overlay_.rgba.data(), overlay_.w, overlay_.h);
                    overlay_.dirty = false;
                }
                if (background_.dirty) {
                    vk.core.setBackground(background_.rgba.empty() ? nullptr : background_.rgba.data(), background_.w, background_.h);
                    background_.dirty = false;
                }
            }
            vk.core.setBrandingOpacity(overlayOpacity_.load(), backgroundOpacity_.load());
            if (saver != appliedSaver && current) {
                if (__builtin_available(android 30, *)) {  // let the display drop its refresh rate too
                    ANativeWindow_setFrameRate(current, saver ? 30.f : 0.f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
                }
                appliedSaver = saver;
            }
            const auto work0 = std::chrono::steady_clock::now();
            const bool drawn = vk.drawFrame(state_.read(), t, std::min(dt, 0.1f), scale);
            if (__builtin_available(android 33, *)) {
                if (hint) {
                    const double cpuNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - work0).count();
                    APerformanceHint_reportActualWorkDuration(hint, static_cast<int64_t>(std::max(cpuNs, static_cast<double>(vk.gpuMs) * 1e6)));
                }
            }
            if (saver) {  // hold ~30 fps even if the compositor would present sooner
                const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
                if (spent < 1.0 / 30.0) std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / 30.0 - spent));
            }
            gpuMs_ = vk.gpuMs;
            scaleNow_ = scale;
            winGpu += vk.gpuMs;
            winDt += dt * 1000.0;
            if (++winN >= 45) {
                const double period = 1000.0 / fpsTarget;
                const double avgGpu = winGpu / winN, avgDt = winDt / winN;
                if (autoScale_.load()) {
                    if (avgGpu > 0.92 * period || avgDt > 1.3 * period) {
                        scale = std::max(kMinScale, scale - 0.1f);
                        goodWindows = 0;
                    } else if (avgGpu > 0.0 && avgGpu < 0.6 * period && avgDt < 1.15 * period) {
                        if (++goodWindows >= 4 && scale < cap) {
                            scale = std::min(cap, scale + 0.05f);
                            goodWindows = 0;
                        }
                    } else {
                        goodWindows = 0;
                    }
                } else {
                    scale = cap;
                }
                winGpu = winDt = 0.0;
                winN = 0;
            }
            if (!drawn) {
                LOGE("frame failed; waiting for a new surface");
                active = false;
                vk.destroySurface();
            }
        }
    }

    if (__builtin_available(android 33, *)) {
        if (hint) APerformanceHint_closeSession(hint);
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
