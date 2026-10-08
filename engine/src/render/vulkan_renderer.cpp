#include "av/render/vulkan_renderer.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "bars_frag.h"
#include "fullscreen_vert.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "av.vk", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "av.vk", __VA_ARGS__)

#define VK_TRY(expr)                                                       \
    do {                                                                   \
        const VkResult r_ = (expr);                                        \
        if (r_ != VK_SUCCESS) {                                            \
            LOGE("%s failed: %d (%s:%d)", #expr, r_, __FILE__, __LINE__);  \
            return false;                                                  \
        }                                                                  \
    } while (0)

namespace av {

namespace {
constexpr uint32_t kFramesInFlight = 2;

// Mirrors the std140 block in shaders/bars.frag.
struct FrameUbo {
    float params[4];  // aspect, time, rms, bar count
    float bars[VisualState::kMaxBars];
    float beat[4];    // beat pulse, beat phase, bar beat, section energy
    float misc[4];    // kick, bpm, hasTimeline, unused
    float bands[8];   // 7 normalised band energies
};
}  // namespace

struct VulkanRenderer::Vk {
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer ubo = VK_NULL_HANDLE;
        VkDeviceMemory uboMem = VK_NULL_HANDLE;
        FrameUbo* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    Frame frames[kFramesInFlight];

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFormat pipelineFormat = VK_FORMAT_UNDEFINED;

    ANativeWindow* window = nullptr;
    uint32_t frameIndex = 0;

    bool createInstance();
    bool createDevice();  // needs surface for present support
    bool createFrameResources();
    bool createSurface(ANativeWindow* w);
    bool createSwapchain();
    bool createPipeline();
    void destroySwapchain();
    void destroySurface();
    void destroyAll();
    bool recreateSwapchain();
    bool drawFrame(const VisualState& vs, float timeSec);
    VkShaderModule makeShader(const uint32_t* code, size_t bytes);
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
};

bool VulkanRenderer::Vk::createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AudioVisualizer";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VK_TRY(vkCreateInstance(&ci, nullptr, &instance));
    return true;
}

bool VulkanRenderer::Vk::createSurface(ANativeWindow* w) {
    window = w;
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = w;
    VK_TRY(vkCreateAndroidSurfaceKHR(instance, &ci, nullptr, &surface));
    return true;
}

bool VulkanRenderer::Vk::createDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) {
        LOGE("no Vulkan devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qp.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                phys = d;
                family = i;
                break;
            }
        }
        if (phys) break;
    }
    if (!phys) {
        LOGE("no graphics+present queue");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    LOGI("GPU: %s (Vulkan %u.%u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VK_TRY(vkCreateDevice(phys, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, family, 0, &queue);
    return createFrameResources();
}

uint32_t VulkanRenderer::Vk::findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool VulkanRenderer::Vk::createFrameResources() {
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = family;
    VK_TRY(vkCreateCommandPool(dev, &cp, nullptr, &cmdPool));

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl));

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl;
    VK_TRY(vkCreatePipelineLayout(dev, &pl, nullptr, &pipeLayout));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kFramesInFlight;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(dev, &dp, nullptr, &dpool));

    for (Frame& f : frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(dev, &ai, &f.cmd));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(dev, &si, nullptr, &f.acquire));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_TRY(vkCreateFence(dev, &fi, nullptr, &f.fence));

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(FrameUbo);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VK_TRY(vkCreateBuffer(dev, &bi, nullptr, &f.ubo));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, f.ubo, &mr);
        const uint32_t mt = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) {
            LOGE("no host-visible memory");
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        VK_TRY(vkAllocateMemory(dev, &mai, nullptr, &f.uboMem));
        VK_TRY(vkBindBufferMemory(dev, f.ubo, f.uboMem, 0));
        void* p = nullptr;
        VK_TRY(vkMapMemory(dev, f.uboMem, 0, VK_WHOLE_SIZE, 0, &p));
        f.mapped = static_cast<FrameUbo*>(p);
        std::memset(f.mapped, 0, sizeof(FrameUbo));

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        VK_TRY(vkAllocateDescriptorSets(dev, &dai, &f.set));
        VkDescriptorBufferInfo dbi{f.ubo, 0, sizeof(FrameUbo)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    return true;
}

VkShaderModule VulkanRenderer::Vk::makeShader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

bool VulkanRenderer::Vk::createPipeline() {
    if (pipeline && pipelineFormat == format) return true;
    if (pipeline) {
        vkDestroyPipeline(dev, pipeline, nullptr);
        vkDestroyRenderPass(dev, renderPass, nullptr);
        pipeline = VK_NULL_HANDLE;
        renderPass = VK_NULL_HANDLE;
    }
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is overwritten
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
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK_TRY(vkCreateRenderPass(dev, &rpi, nullptr, &renderPass));

    VkShaderModule vs = makeShader(shaders::fullscreen_vert, shaders::fullscreen_vertSize);
    VkShaderModule fs = makeShader(shaders::bars_frag, shaders::bars_fragSize);
    if (!vs || !fs) {
        LOGE("shader module creation failed");
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = pipeLayout;
    gp.renderPass = renderPass;
    const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %d", r);
        return false;
    }
    pipelineFormat = format;
    return true;
}

bool VulkanRenderer::Vk::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
    for (const auto& f : fmts) {
        // UNORM: shaders output display-referred colour directly (no implicit sRGB encode).
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
    // (Pre-rotation would save a compositor pass; tracked for the M6 performance work.)
    sc.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    sc.compositeAlpha = alpha;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync; the only mode guaranteed everywhere
    sc.clipped = VK_TRUE;
    VK_TRY(vkCreateSwapchainKHR(dev, &sc, nullptr, &swapchain));
    LOGI("swapchain %ux%u fmt=%d images=%u transform=0x%x", extent.width, extent.height, format,
         imageCount, caps.currentTransform);

    if (!createPipeline()) return false;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(dev, swapchain, &n, images.data());
    views.resize(n);
    framebuffers.resize(n);
    renderDone.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_TRY(vkCreateImageView(dev, &vi, nullptr, &views[i]));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &views[i];
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        VK_TRY(vkCreateFramebuffer(dev, &fb, nullptr, &framebuffers[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(dev, &si, nullptr, &renderDone[i]));
    }
    return true;
}

void VulkanRenderer::Vk::destroySwapchain() {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (VkSemaphore s : renderDone) vkDestroySemaphore(dev, s, nullptr);
    for (VkFramebuffer f : framebuffers) vkDestroyFramebuffer(dev, f, nullptr);
    for (VkImageView v : views) vkDestroyImageView(dev, v, nullptr);
    renderDone.clear();
    framebuffers.clear();
    views.clear();
    images.clear();
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void VulkanRenderer::Vk::destroySurface() {
    destroySwapchain();
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    window = nullptr;
}

bool VulkanRenderer::Vk::recreateSwapchain() {
    destroySwapchain();
    return createSwapchain();
}

void VulkanRenderer::Vk::destroyAll() {
    destroySurface();
    if (dev) {
        vkDeviceWaitIdle(dev);
        if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
        if (renderPass) vkDestroyRenderPass(dev, renderPass, nullptr);
        for (Frame& f : frames) {
            if (f.mapped) vkUnmapMemory(dev, f.uboMem);
            if (f.ubo) vkDestroyBuffer(dev, f.ubo, nullptr);
            if (f.uboMem) vkFreeMemory(dev, f.uboMem, nullptr);
            if (f.acquire) vkDestroySemaphore(dev, f.acquire, nullptr);
            if (f.fence) vkDestroyFence(dev, f.fence, nullptr);
        }
        if (dpool) vkDestroyDescriptorPool(dev, dpool, nullptr);
        if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
        vkDestroyDevice(dev, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    *this = Vk{};
}

bool VulkanRenderer::Vk::drawFrame(const VisualState& vs, float timeSec) {
    Frame& f = frames[frameIndex];
    vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);

    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f.acquire, VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreateSwapchain();
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed: %d", r);
        return false;
    }
    vkResetFences(dev, 1, &f.fence);

    f.mapped->params[0] = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    f.mapped->params[1] = timeSec;
    f.mapped->params[2] = vs.rms;
    f.mapped->params[3] = static_cast<float>(std::max<uint32_t>(vs.numBars, 1));
    std::memcpy(f.mapped->bars, vs.bars, sizeof(f.mapped->bars));
    f.mapped->beat[0] = vs.beatPulse;
    f.mapped->beat[1] = vs.beatPhase;
    f.mapped->beat[2] = static_cast<float>(vs.barBeat);
    f.mapped->beat[3] = vs.sectionEnergy;
    f.mapped->misc[0] = vs.kick;
    f.mapped->misc[1] = vs.bpm;
    f.mapped->misc[2] = vs.hasTimeline ? 1.f : 0.f;
    f.mapped->misc[3] = 0.f;
    std::memcpy(f.mapped->bands, vs.bands, sizeof(f.mapped->bands));

    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass;
    rp.framebuffer = framebuffers[img];
    rp.renderArea = {{0, 0}, extent};
    vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport vp{0.f, 0.f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                  0.f, 1.f};
    VkRect2D sc{{0, 0}, extent};
    vkCmdSetViewport(f.cmd, 0, 1, &vp);
    vkCmdSetScissor(f.cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &f.set, 0,
                            nullptr);
    vkCmdDraw(f.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(f.cmd);
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
    if (vkQueueSubmit(queue, 1, &si, f.fence) != VK_SUCCESS) return false;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &img;
    r = vkQueuePresentKHR(queue, &pi);
    frameIndex = (frameIndex + 1) % kFramesInFlight;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return recreateSwapchain();
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
    // Wait until the render thread has consumed the request (and released the old surface).
    cv_.wait(lk, [this] { return !surfaceDirty_ || stop_.load(); });
}

void VulkanRenderer::run() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
    Vk vk;
    ANativeWindow* current = nullptr;
    bool instanceOk = vk.createInstance();
    bool deviceOk = false;
    bool active = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastFrame = t0;

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
                if (!deviceOk) deviceOk = vk.createDevice();
                active = deviceOk && vk.createSwapchain();
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
            lastFrameMs_ = std::chrono::duration<float, std::milli>(now - lastFrame).count();
            lastFrame = now;
            if (!vk.drawFrame(state_.read(), t)) {
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
