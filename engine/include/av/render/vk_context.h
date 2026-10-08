#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <cstddef>
#include <cstdint>

namespace av::vk {

/// Instance/device/queue and the small helpers every render component needs.
struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};

    bool createInstance();
    /// Picks a graphics+present device for `surface` and creates the device and command pool.
    bool createDevice(VkSurfaceKHR surface);
    void destroy();

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    bool formatSupports(VkFormat f, VkFormatFeatureFlags needed) const;
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;  // non-null for host-visible buffers
    VkDeviceSize size = 0;
};
bool createBuffer(const Context&, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                  Buffer* out);
void destroy(const Context&, Buffer& b);

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
};
bool createImage2D(const Context&, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                   Image* out);
void destroy(const Context&, Image& i);

VkShaderModule createShader(const Context&, const uint32_t* code, size_t bytes);

}  // namespace av::vk
