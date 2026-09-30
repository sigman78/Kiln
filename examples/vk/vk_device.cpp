// examples/vk/vk_device.cpp — Vulkan 1.4 instance and device bring-up (docs/design/viewer.md).
#include "vk_device.h"

#include <kiln/alloc.h>
#include <kiln/log.h>

#include <cstring>

namespace kiln::vkx {

namespace {

constexpr char const* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kMaxNames                = 32;
constexpr u32 kMaxDevices              = 16;
constexpr u32 kMaxFamilies             = 32;
constexpr u32 kMaxLayers               = 64;

Status vk_fail(DiagSink const* diag, char const* what, VkResult r) noexcept {
    return diagf(diag, make_status(Code::Internal), 0, Severity::Error, {}, "vulkan", "%s failed: %s", what,
                 result_name(r));
}

bool layer_present(char const* name) noexcept {
    u32 count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    VkLayerProperties layers[kMaxLayers];
    count = count < kMaxLayers ? count : kMaxLayers;
    if (vkEnumerateInstanceLayerProperties(&count, layers) < 0) return false;
    for (u32 i = 0; i < count; ++i)
        if (std::strcmp(layers[i].layerName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              VkDebugUtilsMessengerCallbackDataEXT const* data,
                                              void* /*user*/) {
    char const* msg = data && data->pMessage ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        KILN_ERROR("vulkan", "%s", msg);
    else
        KILN_WARN("vulkan", "%s", msg);
    return VK_FALSE;
}

/// The feature chain the viewer and the adapter need; link() before querying; `missing` names
/// the first absent feature (null when all are present).
struct FeatureChain {
    VkPhysicalDeviceVulkan14Features f14{};
    VkPhysicalDeviceVulkan13Features f13{};
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan11Features f11{};
    VkPhysicalDeviceFeatures2 f2{};

    void link() noexcept {
        f2.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        f2.pNext  = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &f14;
        f14.pNext = nullptr;
    }

    [[nodiscard]] char const* missing() const noexcept {
        if (!f12.timelineSemaphore) return "timelineSemaphore";
        if (!f12.descriptorIndexing) return "descriptorIndexing";
        if (!f12.shaderSampledImageArrayNonUniformIndexing)
            return "shaderSampledImageArrayNonUniformIndexing";
        if (!f12.descriptorBindingSampledImageUpdateAfterBind)
            return "descriptorBindingSampledImageUpdateAfterBind";
        if (!f12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
        if (!f12.descriptorBindingUpdateUnusedWhilePending)
            return "descriptorBindingUpdateUnusedWhilePending";
        if (!f12.runtimeDescriptorArray) return "runtimeDescriptorArray";
        if (!f12.bufferDeviceAddress) return "bufferDeviceAddress";
        if (!f13.synchronization2) return "synchronization2";
        if (!f13.dynamicRendering) return "dynamicRendering";
        if (!f14.maintenance5) return "maintenance5";
        return nullptr;
    }

    /// Only the features this example uses, so the device does not pay for the rest.
    void enable_required() noexcept {
        f12.timelineSemaphore                            = VK_TRUE;
        f12.descriptorIndexing                           = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingPartiallyBound              = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE;
        f12.runtimeDescriptorArray                       = VK_TRUE;
        f12.bufferDeviceAddress                          = VK_TRUE;
        f13.synchronization2                             = VK_TRUE;
        f13.dynamicRendering                             = VK_TRUE;
        f14.maintenance5                                 = VK_TRUE;
        link();
    }
};

bool has_extension(VkPhysicalDevice pd, char const* name) noexcept {
    u32 count = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    u32 const cap              = count;
    VkExtensionProperties* ext = alloc_array<VkExtensionProperties>(default_allocator(), cap, Tag::General);
    bool found                 = false;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, ext) >= 0)
        for (u32 i = 0; i < count && !found; ++i)
            found = std::strcmp(ext[i].extensionName, name) == 0;
    free_array(default_allocator(), ext, cap, Tag::General);
    return found;
}

/// Returns the graphics family, or kInvalid when the device has none.
u32 graphics_family(VkPhysicalDevice pd) noexcept {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    VkQueueFamilyProperties fam[kMaxFamilies];
    count = count < kMaxFamilies ? count : kMaxFamilies;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fam);
    for (u32 i = 0; i < count; ++i)
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    return kInvalid;
}

} // namespace

char const* result_name(VkResult r) noexcept {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return "VkResult(unknown)";
    }
}

Result<Device> device_create(DeviceDesc const& desc, DiagSink const* diag) noexcept {
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) return vk_fail(diag, "volkInitialize (no Vulkan loader)", r);
    if (volkGetInstanceVersion() < VK_API_VERSION_1_4)
        return diagf(diag, make_status(Code::Unsupported), 0, Severity::Error, {}, "vulkan",
                     "the Vulkan loader is older than 1.4");

    // --- Instance ---------------------------------------------------------------------------
    char const* extensions[kMaxNames];
    u32 extCount = 0;
    for (char const* e : desc.instanceExtensions)
        if (extCount < kMaxNames - 1) extensions[extCount++] = e;
    bool validation = desc.validation;
    if (validation && !layer_present(kValidationLayer)) {
        KILN_WARN("vulkan", "%s is not installed; continuing without validation", kValidationLayer);
        validation = false;
    }
    if (validation) extensions[extCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = desc.appName;
    app.applicationVersion = 1;
    app.pEngineName        = "kiln";
    app.engineVersion      = 1;
    app.apiVersion         = VK_API_VERSION_1_4;
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
    messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = &debug_callback;
    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pNext                   = validation ? &messengerInfo : nullptr;
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = validation ? 1u : 0u;
    ici.ppEnabledLayerNames     = &kValidationLayer;
    ici.enabledExtensionCount   = extCount;
    ici.ppEnabledExtensionNames = extensions;
    Device d;
    r = vkCreateInstance(&ici, nullptr, &d.instance);
    if (r != VK_SUCCESS) return vk_fail(diag, "vkCreateInstance", r);
    volkLoadInstanceOnly(d.instance);
    if (validation) {
        r = vkCreateDebugUtilsMessengerEXT(d.instance, &messengerInfo, nullptr, &d.messenger);
        if (r != VK_SUCCESS) {
            device_destroy(d);
            return vk_fail(diag, "vkCreateDebugUtilsMessengerEXT", r);
        }
    }

    // --- Physical device: first qualifying one, discrete preferred ---------------------------
    u32 count = 0;
    r         = vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
    VkPhysicalDevice devices[kMaxDevices];
    count = count < kMaxDevices ? count : kMaxDevices;
    if (r == VK_SUCCESS) r = vkEnumeratePhysicalDevices(d.instance, &count, devices);
    if (r < 0) {
        device_destroy(d);
        return vk_fail(diag, "vkEnumeratePhysicalDevices", r);
    }
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    bool chosenDiscrete     = false;
    for (u32 i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        char const* why = nullptr;
        FeatureChain fc;
        fc.link();
        if (props.apiVersion < VK_API_VERSION_1_4) {
            why = "driver is older than Vulkan 1.4";
        } else {
            vkGetPhysicalDeviceFeatures2(devices[i], &fc.f2);
            why = fc.missing();
            if (!why && graphics_family(devices[i]) == kInvalid) why = "no graphics queue";
            if (!why && desc.needSwapchain && !has_extension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                why = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        }
        if (why) {
            KILN_INFO("vulkan", "skipping %s: %s", props.deviceName, why);
            continue;
        }
        bool const discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (!chosen || (discrete && !chosenDiscrete)) {
            chosen         = devices[i];
            chosenDiscrete = discrete;
        }
    }
    if (!chosen) {
        device_destroy(d);
        return diagf(diag, make_status(Code::Unsupported), 0, Severity::Error, {}, "vulkan",
                     "no Vulkan 1.4 device with the required features (%u device(s) checked)", count);
    }
    d.physical = chosen;
    vkGetPhysicalDeviceProperties(d.physical, &d.props);
    vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memProps);

    // --- Queues ------------------------------------------------------------------------------
    u32 famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &famCount, nullptr);
    VkQueueFamilyProperties fam[kMaxFamilies];
    famCount = famCount < kMaxFamilies ? famCount : kMaxFamilies;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &famCount, fam);
    d.graphicsFamily = graphics_family(d.physical);
    d.transferFamily = d.graphicsFamily;
    for (u32 i = 0; i < famCount; ++i) {
        if ((fam[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            d.transferFamily    = i;
            d.dedicatedTransfer = true;
            break;
        }
    }
    // Same family: a second queue if the family has one, else share the graphics queue.
    u32 const transferIndex = !d.dedicatedTransfer && fam[d.graphicsFamily].queueCount > 1 ? 1u : 0u;

    float const priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queues[2]{};
    u32 queueInfos             = 1;
    queues[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = d.graphicsFamily;
    queues[0].queueCount       = d.dedicatedTransfer ? 1u : 1u + transferIndex;
    queues[0].pQueuePriorities = priorities;
    if (d.dedicatedTransfer) {
        queues[1].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[1].queueFamilyIndex = d.transferFamily;
        queues[1].queueCount       = 1;
        queues[1].pQueuePriorities = priorities;
        queueInfos                 = 2;
    }

    FeatureChain enable{};
    enable.enable_required();
    char const* deviceExt[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &enable.f2;
    dci.queueCreateInfoCount    = queueInfos;
    dci.pQueueCreateInfos       = queues;
    dci.enabledExtensionCount   = desc.needSwapchain ? 1u : 0u;
    dci.ppEnabledExtensionNames = deviceExt;
    r                           = vkCreateDevice(d.physical, &dci, nullptr, &d.device);
    if (r != VK_SUCCESS) {
        device_destroy(d);
        return vk_fail(diag, "vkCreateDevice", r);
    }
    volkLoadDevice(d.device);
    vkGetDeviceQueue(d.device, d.graphicsFamily, 0, &d.graphicsQueue);
    vkGetDeviceQueue(d.device, d.transferFamily, transferIndex, &d.transferQueue);

    KILN_INFO("vulkan", "%s (Vulkan %u.%u.%u), graphics family %u, transfer family %u queue %u%s%s",
              d.props.deviceName, VK_API_VERSION_MAJOR(d.props.apiVersion),
              VK_API_VERSION_MINOR(d.props.apiVersion), VK_API_VERSION_PATCH(d.props.apiVersion),
              d.graphicsFamily, d.transferFamily, transferIndex, d.dedicatedTransfer ? " (dedicated)" : "",
              validation ? ", validation on" : "");
    return d;
}

void device_destroy(Device& d) noexcept {
    if (d.device) {
        vkDeviceWaitIdle(d.device);
        vkDestroyDevice(d.device, nullptr);
    }
    if (d.messenger) vkDestroyDebugUtilsMessengerEXT(d.instance, d.messenger, nullptr);
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    d = Device{};
}

u32 find_memory_type(Device const& d, u32 typeBits, VkMemoryPropertyFlags required,
                     VkMemoryPropertyFlags preferred) noexcept {
    u32 fallback = kInvalid;
    for (u32 i = 0; i < d.memProps.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        VkMemoryPropertyFlags const f = d.memProps.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        if ((f & preferred) == preferred) return i;
        if (fallback == kInvalid) fallback = i;
    }
    return fallback;
}

} // namespace kiln::vkx
