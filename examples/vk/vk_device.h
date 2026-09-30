// examples/vk/vk_device.h — Vulkan 1.4 instance and device bring-up shared by the
// Vulkan examples and the adapter smoke test. Example code only; the library never sees Vulkan.
#pragma once

#include <kiln/core.h>
#include <kiln/result.h>

#include <volk.h>

namespace kiln::vkx {

struct DeviceDesc {
    char const* appName = "kiln-viewer";
    bool validation     = false; ///< enable VK_LAYER_KHRONOS_validation and a debug messenger
    /// Instance extensions the window system needs (from glfwGetRequiredInstanceExtensions).
    /// Empty for offscreen use.
    Span<char const* const> instanceExtensions = {};
    bool needSwapchain                         = false; ///< enable VK_KHR_swapchain on the device
};

/// Everything the viewer and the adapter need to talk to one device.
struct Device {
    VkInstance instance                       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger        = VK_NULL_HANDLE;
    VkPhysicalDevice physical                 = VK_NULL_HANDLE;
    VkDevice device                           = VK_NULL_HANDLE;
    u32 graphicsFamily                        = 0;
    VkQueue graphicsQueue                     = VK_NULL_HANDLE;
    u32 transferFamily                        = 0;
    VkQueue transferQueue                     = VK_NULL_HANDLE;
    bool dedicatedTransfer                    = false; ///< transferFamily != graphicsFamily
    VkPhysicalDeviceProperties props          = {};
    VkPhysicalDeviceMemoryProperties memProps = {};
};

/// Loads volk, creates the instance, picks a Vulkan 1.4 device with the features listed in
/// docs/design/viewer.md, creates the queues. Fails with Code::Unsupported when no device
/// qualifies and Code::Internal for any Vulkan error (the VkResult name is in the diagnostic).
[[nodiscard]] Result<Device> device_create(DeviceDesc const& desc, DiagSink const* diag = nullptr) noexcept;
void device_destroy(Device& d) noexcept;

/// Memory type index with every `required` flag, preferring one that also has `preferred`.
[[nodiscard]] u32 find_memory_type(Device const& d, u32 typeBits, VkMemoryPropertyFlags required,
                                   VkMemoryPropertyFlags preferred = 0) noexcept;

[[nodiscard]] char const* result_name(VkResult r) noexcept;

} // namespace kiln::vkx

/// Panics with the VkResult name on failure. Example code treats any Vulkan error as a bug.
#define VKX_CHECK(expr)                                                                                      \
    do {                                                                                                     \
        VkResult const vkx_r_ = (expr);                                                                      \
        if (vkx_r_ != VK_SUCCESS) KILN_PANIC("%s failed: %s", #expr, ::kiln::vkx::result_name(vkx_r_));      \
    } while (0)
