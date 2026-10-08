#include "av/render/vk_context.h"

#include <android/log.h>

#include <vector>

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

namespace av::vk {

bool Context::createInstance() {
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

bool Context::createDevice(VkSurfaceKHR surface) {
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
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
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

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = family;
    VK_TRY(vkCreateCommandPool(dev, &cp, nullptr, &cmdPool));
    return true;
}

void Context::destroy() {
    if (dev) {
        vkDeviceWaitIdle(dev);
        if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
        vkDestroyDevice(dev, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    *this = Context{};
}

uint32_t Context::findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool Context::formatSupports(VkFormat f, VkFormatFeatureFlags needed) const {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(phys, f, &p);
    return (p.optimalTilingFeatures & needed) == needed;
}

bool createBuffer(const Context& c, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                  Buffer* out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VK_TRY(vkCreateBuffer(c.dev, &bi, nullptr, &out->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c.dev, out->buf, &mr);
    VkMemoryPropertyFlags want = hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mt = c.findMemoryType(mr.memoryTypeBits, want);
    if (mt == UINT32_MAX) mt = c.findMemoryType(mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) {
        LOGE("no suitable memory type");
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    VK_TRY(vkAllocateMemory(c.dev, &mai, nullptr, &out->mem));
    VK_TRY(vkBindBufferMemory(c.dev, out->buf, out->mem, 0));
    out->size = size;
    if (hostVisible) VK_TRY(vkMapMemory(c.dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->mapped));
    return true;
}

void destroy(const Context& c, Buffer& b) {
    if (b.mapped) vkUnmapMemory(c.dev, b.mem);
    if (b.buf) vkDestroyBuffer(c.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(c.dev, b.mem, nullptr);
    b = Buffer{};
}

bool createImage2D(const Context& c, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                   Image* out) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY(vkCreateImage(c.dev, &ii, nullptr, &out->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c.dev, out->img, &mr);
    const uint32_t mt = c.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    VK_TRY(vkAllocateMemory(c.dev, &mai, nullptr, &out->mem));
    VK_TRY(vkBindImageMemory(c.dev, out->img, out->mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_TRY(vkCreateImageView(c.dev, &vi, nullptr, &out->view));
    out->format = fmt;
    out->w = w;
    out->h = h;
    return true;
}

void destroy(const Context& c, Image& i) {
    if (i.view) vkDestroyImageView(c.dev, i.view, nullptr);
    if (i.img) vkDestroyImage(c.dev, i.img, nullptr);
    if (i.mem) vkFreeMemory(c.dev, i.mem, nullptr);
    i = Image{};
}

VkShaderModule createShader(const Context& c, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c.dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

}  // namespace av::vk
