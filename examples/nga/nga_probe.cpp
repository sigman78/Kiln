// examples/nga/nga_probe.cpp — says why NoGraphicsAPI found no device: it reports only "unsupported".
#include "nga_probe.h"

#include <kiln/log.h>

#include <vulkan/vulkan.h>

#include <cstring>

namespace kiln::nga {
namespace {

constexpr u32 kMaxNames = 512;

bool has(VkExtensionProperties const* list, u32 count, char const* name) {
    for (u32 i = 0; i < count; ++i)
        if (std::strcmp(list[i].extensionName, name) == 0) return true;
    return false;
}

/// Appends " <name>" for each name `list` lacks.
void append_missing(char* out, usize cap, VkExtensionProperties const* list, u32 count,
                    Span<char const* const> names) {
    for (char const* name : names)
        if (!has(list, count, name)) {
            usize const n = std::strlen(out);
            format(out + n, cap - n, " %s", name);
        }
}

} // namespace

void log_device_support(bool windowed) {
    u32 loader = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loader);
    KILN_INFO("nga", "Vulkan loader %u.%u.%u", VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader),
              VK_API_VERSION_PATCH(loader));
    static VkExtensionProperties names[kMaxNames];
    u32 count = kMaxNames;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, names);
    if (windowed && !has(names, count, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
        !has(names, count, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
        KILN_ERROR("nga", "the instance lacks VK_KHR_surface_maintenance1 and VK_EXT_surface_maintenance1");

    VkApplicationInfo const app{.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .apiVersion = VK_API_VERSION_1_4};
    VkInstanceCreateInfo const ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS) {
        KILN_ERROR("nga", "vkCreateInstance for Vulkan 1.4 failed (%d)", int(r));
        return;
    }
    VkPhysicalDevice devices[8];
    u32 deviceCount = 8;
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices);
    if (deviceCount == 0) KILN_ERROR("nga", "the loader found no Vulkan device");
    for (u32 d = 0; d < deviceCount; ++d) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devices[d], &p);
        count = kMaxNames;
        vkEnumerateDeviceExtensionProperties(devices[d], nullptr, &count, names);
        char missing[512]            = "";
        char const* const required[] = {
            VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
            VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME};
        append_missing(missing, sizeof missing, names, count, required);
        if (windowed) {
            char const* const swapchain[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            append_missing(missing, sizeof missing, names, count, swapchain);
            if (!has(names, count, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) &&
                !has(names, count, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
                usize const n = std::strlen(missing);
                format(missing + n, sizeof missing - n, " VK_KHR_swapchain_maintenance1");
            }
        }
        // driverVersion is vendor-packed: NVIDIA uses 10.8.8.6 bits, the others Vulkan's layout.
        bool const nvidia = p.vendorID == 0x10DE;
        u32 const major   = nvidia ? p.driverVersion >> 22 : VK_API_VERSION_MAJOR(p.driverVersion);
        u32 const minor   = nvidia ? (p.driverVersion >> 14) & 0xFF : VK_API_VERSION_MINOR(p.driverVersion);
        KILN_INFO("nga", "device %u: %s, Vulkan %u.%u.%u, driver %u.%u", d, p.deviceName,
                  VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                  VK_API_VERSION_PATCH(p.apiVersion), major, minor);
        if (p.apiVersion < VK_API_VERSION_1_4)
            KILN_ERROR("nga", "device %u: needs Vulkan 1.4; update the driver", d);
        if (missing[0])
            KILN_ERROR("nga", "device %u: the driver lacks%s", d, missing);
        else if (p.apiVersion >= VK_API_VERSION_1_4)
            KILN_ERROR("nga",
                       "device %u: version and extensions are there; a feature NoGraphicsAPI requires is off "
                       "(inspect_candidate in NoGraphicsAPI.cpp)",
                       d);
    }
    vkDestroyInstance(instance, nullptr);
}

} // namespace kiln::nga
