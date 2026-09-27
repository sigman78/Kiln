// examples/viewer/viewer_render.cpp — swapchain or offscreen target, frames in flight and
// per-layout pipelines for the viewer (docs/design/viewer.md, "Viewer design").
#include "viewer_render.h"

#include <kiln/formats.h>
#include <kiln/log.h>

#include "shaders/mesh_frag_spv.h"
#include "shaders/mesh_vert_spv.h"

#include <cstring>

namespace kiln::vkx {

namespace {

constexpr u32 kMaxSwapImages    = 8;
constexpr u32 kMaxFormats       = 64;
constexpr u32 kLocations        = 6; ///< position, normal, tangent, uv0, uv1, color
constexpr u64 kZeroBytes        = 64;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Image {
    VkImage image         = VK_NULL_HANDLE;
    VkImageView view      = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Frame {
    VkCommandPool pool       = VK_NULL_HANDLE;
    VkCommandBuffer cmd      = VK_NULL_HANDLE;
    VkFence fence            = VK_NULL_HANDLE;
    VkSemaphore acquired     = VK_NULL_HANDLE;
    VkBuffer ubo             = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    FrameUniforms* uniforms  = nullptr;
    VkDescriptorSet set      = VK_NULL_HANDLE;
    u64 number               = 0; ///< the frame that used this slot last
};

} // namespace

struct Renderer {
    RendererDesc desc{};
    Allocator const* alloc = nullptr;
    Device const* dev      = nullptr;
    VkDevice device        = VK_NULL_HANDLE;
    VkAdapter* adapter     = nullptr;

    VkShaderModule vert                  = VK_NULL_HANDLE;
    VkShaderModule frag                  = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool framePool           = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout      = VK_NULL_HANDLE;
    VkBuffer zero                        = VK_NULL_HANDLE;
    VkDeviceMemory zeroMemory            = VK_NULL_HANDLE;
    Vec<LayoutPipeline> pipelines; ///< a null pipeline records a layout that failed

    Frame frames[kFramesInFlight];
    u64 frameNumber = 0;
    u32 slot        = 0;

    bool offscreen       = false;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    Image depth;

    // Window mode.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    u32 imageCount           = 0;
    VkImage swapImages[kMaxSwapImages]{};
    VkImageView swapViews[kMaxSwapImages]{};
    VkSemaphore renderDone[kMaxSwapImages]{}; ///< per image: a present may still wait on it
    u32 imageIndex    = 0;
    bool needRecreate = false;

    // Offscreen mode.
    Image color;
    VkBuffer readback          = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    u8 const* readbackMapped   = nullptr;
    bool readbackRecorded      = false;
};

namespace {

// --- Small object helpers -----------------------------------------------------------------------

/// Names for the color formats a swapchain or the offscreen target is likely to use.
char const* vk_format_name(VkFormat f) noexcept {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "other";
    }
}

Status create_image(Renderer* r, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    Image* out) noexcept {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = format;
    info.extent        = {r->extent.width, r->extent.height, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = usage;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult res       = vkCreateImage(r->device, &info, nullptr, &out->image);
    if (res != VK_SUCCESS) {
        KILN_ERROR("viewer", "vkCreateImage(%ux%u) failed: %s", r->extent.width, r->extent.height,
                   result_name(res));
        return make_status(Code::Internal);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, out->image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = find_memory_type(*r->dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == kInvalid) return make_status(Code::Unsupported);
    VKX_CHECK(vkAllocateMemory(r->device, &mai, nullptr, &out->memory));
    VKX_CHECK(vkBindImageMemory(r->device, out->image, out->memory, 0));
    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = out->image;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    VKX_CHECK(vkCreateImageView(r->device, &view, nullptr, &out->view));
    return kOk;
}

void destroy_image(Renderer* r, Image& img) noexcept {
    if (img.view) vkDestroyImageView(r->device, img.view, nullptr);
    if (img.image) vkDestroyImage(r->device, img.image, nullptr);
    if (img.memory) vkFreeMemory(r->device, img.memory, nullptr);
    img = Image{};
}

/// A buffer in memory with every `required` flag, mapped when host-visible.
Status create_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                     VkMemoryPropertyFlags preferred, VkBuffer* buffer, VkDeviceMemory* memory,
                     void** mapped) noexcept {
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKX_CHECK(vkCreateBuffer(r->device, &info, nullptr, buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, *buffer, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = find_memory_type(*r->dev, req.memoryTypeBits, required, preferred);
    if (mai.memoryTypeIndex == kInvalid) return make_status(Code::Unsupported);
    VKX_CHECK(vkAllocateMemory(r->device, &mai, nullptr, memory));
    VKX_CHECK(vkBindBufferMemory(r->device, *buffer, *memory, 0));
    if (mapped) VKX_CHECK(vkMapMemory(r->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped));
    return kOk;
}

void destroy_buffer(Renderer* r, VkBuffer& buffer, VkDeviceMemory& memory) noexcept {
    if (buffer) vkDestroyBuffer(r->device, buffer, nullptr);
    if (memory) vkFreeMemory(r->device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                                    VkImageLayout to) noexcept {
    VkImageMemoryBarrier2 b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {aspect, 0, 1, 0, 1};
    return b;
}

void barrier(VkCommandBuffer cmd, VkImageMemoryBarrier2 const* images, u32 count) noexcept {
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers    = images;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// --- Targets ------------------------------------------------------------------------------------

Status create_depth(Renderer* r) noexcept {
    destroy_image(r, r->depth);
    return create_image(r, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, &r->depth);
}

void destroy_pipelines(Renderer* r) noexcept {
    for (LayoutPipeline& p : r->pipelines)
        if (p.pipeline) vkDestroyPipeline(r->device, p.pipeline, nullptr);
    r->pipelines.clear();
}

void destroy_swapchain_views(Renderer* r) noexcept {
    for (u32 i = 0; i < r->imageCount; ++i) {
        if (r->swapViews[i]) vkDestroyImageView(r->device, r->swapViews[i], nullptr);
        r->swapViews[i]  = VK_NULL_HANDLE;
        r->swapImages[i] = VK_NULL_HANDLE;
    }
    r->imageCount = 0;
}

/// (Re)creates the swapchain for the current framebuffer size. False while the window has
/// no area (minimized); the caller skips the frame and tries again later.
bool create_swapchain(Renderer* r) noexcept {
    u32 w = 0, h = 0;
    r->desc.framebufferSize(r->desc.user, &w, &h);
    if (w == 0 || h == 0) return false;
    VkSurfaceCapabilitiesKHR caps;
    VKX_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->dev->physical, r->desc.surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width  = clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;

    u32 formatCount = 0;
    VKX_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r->dev->physical, r->desc.surface, &formatCount, nullptr));
    VkSurfaceFormatKHR formats[kMaxFormats];
    formatCount = min(formatCount, kMaxFormats);
    VkResult const fr =
        vkGetPhysicalDeviceSurfaceFormatsKHR(r->dev->physical, r->desc.surface, &formatCount, formats);
    if (fr < 0 || formatCount == 0) KILN_PANIC("vkGetPhysicalDeviceSurfaceFormatsKHR: %s", result_name(fr));
    VkSurfaceFormatKHR chosen = formats[0];
    for (u32 i = 0; i < formatCount; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }

    u32 images = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) images = caps.maxImageCount;
    images = min(images, kMaxSwapImages);

    vkDeviceWaitIdle(r->device);
    VkSwapchainCreateInfoKHR info{};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = r->desc.surface;
    info.minImageCount    = images;
    info.imageFormat      = chosen.format;
    info.imageColorSpace  = chosen.colorSpace;
    info.imageExtent      = extent;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform     = caps.currentTransform;
    info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode      = VK_PRESENT_MODE_FIFO_KHR; // always available
    info.clipped          = VK_TRUE;
    info.oldSwapchain     = r->swapchain;
    VkSwapchainKHR next   = VK_NULL_HANDLE;
    VKX_CHECK(vkCreateSwapchainKHR(r->device, &info, nullptr, &next));
    destroy_swapchain_views(r);
    if (r->swapchain) vkDestroySwapchainKHR(r->device, r->swapchain, nullptr);
    r->swapchain = next;

    u32 count = 0;
    VKX_CHECK(vkGetSwapchainImagesKHR(r->device, r->swapchain, &count, nullptr));
    count             = min(count, kMaxSwapImages);
    VkResult const ir = vkGetSwapchainImagesKHR(r->device, r->swapchain, &count, r->swapImages);
    if (ir < 0) KILN_PANIC("vkGetSwapchainImagesKHR: %s", result_name(ir));
    r->imageCount = count;
    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{};
        view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image            = r->swapImages[i];
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = chosen.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKX_CHECK(vkCreateImageView(r->device, &view, nullptr, &r->swapViews[i]));
        if (!r->renderDone[i]) {
            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VKX_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &r->renderDone[i]));
        }
    }
    if (chosen.format != r->colorFormat) destroy_pipelines(r); // they are rebuilt on use
    bool const first = r->colorFormat == VK_FORMAT_UNDEFINED;
    r->colorFormat   = chosen.format;
    r->extent        = extent;
    r->needRecreate  = false;
    if (create_depth(r).failed()) KILN_PANIC("could not create the depth buffer");
    if (first)
        KILN_INFO("viewer", "swapchain %ux%u, %s, %u images, FIFO", extent.width, extent.height,
                  vk_format_name(chosen.format), count);
    return true;
}

Status create_offscreen(Renderer* r) noexcept {
    // sRGB like the window's swapchain, so the PNG looks the same; UNORM if it cannot be rendered to.
    VkFormatFeatureFlags const need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->dev->physical, VK_FORMAT_B8G8R8A8_SRGB, &props);
    r->colorFormat =
        (props.optimalTilingFeatures & need) == need ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    r->extent = {r->desc.width, r->desc.height};
    KILN_TRY(create_image(r, r->colorFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, &r->color));
    KILN_TRY(create_depth(r));
    void* mapped = nullptr;
    KILN_TRY(create_buffer(r, VkDeviceSize(r->extent.width) * r->extent.height * 4,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &r->readback, &r->readbackMem, &mapped));
    r->readbackMapped = static_cast<u8 const*>(mapped);
    KILN_INFO("viewer", "offscreen %ux%u, %s", r->extent.width, r->extent.height,
              vk_format_name(r->colorFormat));
    return kOk;
}

// --- Pipelines ----------------------------------------------------------------------------------

/// Fixed shader location for an attribute, kInvalid for semantics the shaders do not read.
u32 location_of(mesh::VertexAttrib const& a) noexcept {
    switch (mesh::Semantic(a.semantic)) {
    case mesh::Semantic::Position: return a.semanticIndex == 0 ? 0u : kInvalid;
    case mesh::Semantic::Normal: return a.semanticIndex == 0 ? 1u : kInvalid;
    case mesh::Semantic::Tangent: return a.semanticIndex == 0 ? 2u : kInvalid;
    case mesh::Semantic::TexCoord: return a.semanticIndex == 0 ? 3u : a.semanticIndex == 1 ? 4u : kInvalid;
    case mesh::Semantic::Color: return a.semanticIndex == 0 ? 5u : kInvalid;
    default: return kInvalid;
    }
}

VkPipeline create_pipeline(Renderer* r, mesh::VertexLayout const& layout, u32* zeroBindings) noexcept {
    VkVertexInputBindingDescription bindings[mesh::kMaxStreams + kLocations];
    VkVertexInputAttributeDescription attribs[kLocations];
    u32 bindingCount = 0;
    u32 attribCount  = 0;
    bool fed[kLocations]{};
    Format normalFormat = Format::Undefined;

    u32 const streams = min<u32>(layout.streamCount, mesh::kMaxStreams);
    for (u32 s = 0; s < streams; ++s) {
        VkVertexInputBindingDescription& b = bindings[bindingCount++];
        b                                  = VkVertexInputBindingDescription{};
        b.binding                          = s;
        b.stride                           = layout.strides[s];
        b.inputRate                        = VK_VERTEX_INPUT_RATE_VERTEX;
    }
    u32 const count = min<u32>(layout.attribCount, mesh::kMaxAttribs);
    for (u32 i = 0; i < count; ++i) {
        mesh::VertexAttrib const& a = layout.attribs[i];
        u32 const loc               = location_of(a);
        if (loc == kInvalid || fed[loc] || a.stream >= streams) continue;
        VkVertexInputAttributeDescription& d = attribs[attribCount++];
        d                                    = VkVertexInputAttributeDescription{};
        d.location                           = loc;
        d.binding                            = a.stream;
        d.format                             = static_cast<VkFormat>(a.format);
        d.offset                             = a.offset;
        fed[loc]                             = true;
        if (loc == 1) normalFormat = Format(a.format);
    }
    // Every shader input is fed: a missing one reads zeros from an instance-rate binding.
    *zeroBindings = 0;
    for (u32 loc = 0; loc < kLocations; ++loc) {
        if (fed[loc]) continue;
        u32 const binding                    = streams + (*zeroBindings)++;
        VkVertexInputBindingDescription& b   = bindings[bindingCount++];
        b                                    = VkVertexInputBindingDescription{};
        b.binding                            = binding;
        b.stride                             = 16;
        b.inputRate                          = VK_VERTEX_INPUT_RATE_INSTANCE;
        VkVertexInputAttributeDescription& d = attribs[attribCount++];
        d                                    = VkVertexInputAttributeDescription{};
        d.location                           = loc;
        d.binding                            = binding;
        d.format = loc == 3 || loc == 4 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        d.offset = 0;
    }

    // Specialization: a 2-component normal is octahedral (spec §5.3). A missing normal also
    // takes the octahedral path, where the zero input decodes to +Z instead of normalize(0).
    FormatInfo const* nfi  = format_info(normalFormat);
    VkBool32 const spec[4] = {
        !fed[1] || (nfi && nfi->channels == 2) ? VK_TRUE : VK_FALSE,
        fed[2] ? VK_TRUE : VK_FALSE,
        fed[3] ? VK_TRUE : VK_FALSE,
        fed[5] ? VK_TRUE : VK_FALSE,
    };
    VkSpecializationMapEntry entries[4];
    for (u32 i = 0; i < 4; ++i) {
        entries[i]            = VkSpecializationMapEntry{};
        entries[i].constantID = i;
        entries[i].offset     = u32(i * sizeof(VkBool32));
        entries[i].size       = sizeof(VkBool32);
    }
    VkSpecializationInfo specInfo{};
    specInfo.mapEntryCount = 4;
    specInfo.pMapEntries   = entries;
    specInfo.dataSize      = sizeof spec;
    specInfo.pData         = spec;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage               = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module              = r->vert;
    stages[0].pName               = "main";
    stages[0].pSpecializationInfo = &specInfo;
    stages[1].sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module              = r->frag;
    stages[1].pName               = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = bindingCount;
    vertexInput.pVertexBindingDescriptions      = bindings;
    vertexInput.vertexAttributeDescriptionCount = attribCount;
    vertexInput.pVertexAttributeDescriptions    = attribs;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType                           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount                 = 1;
    blend.pAttachments                    = &blendAttachment;
    VkDynamicState const dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &r->colorFormat;
    rendering.depthAttachmentFormat   = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = r->pipelineLayout;
    VkPipeline pipeline      = VK_NULL_HANDLE;
    VkResult const res = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        KILN_ERROR("viewer", "vkCreateGraphicsPipelines failed: %s", result_name(res));
        return VK_NULL_HANDLE;
    }
    KILN_INFO("viewer", "pipeline for layout: %u stream(s), %u attribute(s), %u zero binding(s)%s%s%s%s",
              streams, count, *zeroBindings, spec[0] ? ", oct normal" : "", spec[1] ? ", tangent" : "",
              spec[2] ? ", uv0" : "", spec[3] ? ", color" : "");
    return pipeline;
}

Status create_shared_objects(Renderer* r) noexcept {
    VkShaderModuleCreateInfo smi{};
    smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof k_mesh_vert_spv;
    smi.pCode    = k_mesh_vert_spv;
    VKX_CHECK(vkCreateShaderModule(r->device, &smi, nullptr, &r->vert));
    smi.codeSize = sizeof k_mesh_frag_spv;
    smi.pCode    = k_mesh_frag_spv;
    VKX_CHECK(vkCreateShaderModule(r->device, &smi, nullptr, &r->frag));

    // Set 1: the frame uniform buffer, one descriptor set per frame in flight.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings    = &binding;
    VKX_CHECK(vkCreateDescriptorSetLayout(r->device, &lci, nullptr, &r->frameSetLayout));
    VkDescriptorPoolSize const poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = kFramesInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &poolSize;
    VKX_CHECK(vkCreateDescriptorPool(r->device, &dpci, nullptr, &r->framePool));

    VkDescriptorSetLayout const setLayouts[2] = {adapter_set_layout(r->adapter), r->frameSetLayout};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0;
    push.size       = sizeof(DrawPush);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 2;
    plci.pSetLayouts            = setLayouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push;
    VKX_CHECK(vkCreatePipelineLayout(r->device, &plci, nullptr, &r->pipelineLayout));

    // The zero buffer: device-local is enough, but host-visible lets it be cleared with a memset.
    void* zeroMapped = nullptr;
    KILN_TRY(create_buffer(r, kZeroBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                           &r->zero, &r->zeroMemory, &zeroMapped));
    std::memset(zeroMapped, 0, kZeroBytes);
    vkUnmapMemory(r->device, r->zeroMemory);

    for (Frame& f : r->frames) {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = r->dev->graphicsFamily;
        VKX_CHECK(vkCreateCommandPool(r->device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = f.pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VKX_CHECK(vkAllocateCommandBuffers(r->device, &cai, &f.cmd));
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKX_CHECK(vkCreateFence(r->device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VKX_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &f.acquired));

        void* mapped = nullptr;
        KILN_TRY(create_buffer(r, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                               &f.ubo, &f.uboMemory, &mapped));
        f.uniforms = static_cast<FrameUniforms*>(mapped);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = r->framePool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &r->frameSetLayout;
        VKX_CHECK(vkAllocateDescriptorSets(r->device, &dsai, &f.set));
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = f.ubo;
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(FrameUniforms);
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = f.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &bufferInfo;
        vkUpdateDescriptorSets(r->device, 1, &write, 0, nullptr);
    }
    return kOk;
}

} // namespace

// --- Public entry points ------------------------------------------------------------------------

Result<Renderer*> renderer_create(RendererDesc const& desc) noexcept {
    if (!desc.device || !desc.device->device || !desc.adapter) return make_status(Code::InvalidArgument);
    if (desc.surface && !desc.framebufferSize) return make_status(Code::InvalidArgument);
    if (!desc.surface && (desc.width == 0 || desc.height == 0)) return make_status(Code::InvalidArgument);
    Allocator const* alloc = desc.alloc ? desc.alloc : default_allocator();
    Renderer* r            = new_object<Renderer>(alloc, Tag::General);
    r->desc                = desc;
    r->alloc               = alloc;
    r->dev                 = desc.device;
    r->device              = desc.device->device;
    r->adapter             = desc.adapter;
    r->offscreen           = desc.surface == VK_NULL_HANDLE;
    r->pipelines.init(alloc, Tag::General);
    r->pipelines.reserve(16);

    if (!r->offscreen) {
        VkBool32 present = VK_FALSE;
        VKX_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(r->dev->physical, r->dev->graphicsFamily, desc.surface,
                                                       &present));
        if (!present) {
            KILN_ERROR("viewer", "the graphics queue family cannot present to this window");
            renderer_destroy(r);
            return make_status(Code::Unsupported);
        }
    }
    Status st = create_shared_objects(r);
    if (st.ok()) {
        if (r->offscreen)
            st = create_offscreen(r);
        else
            r->needRecreate = !create_swapchain(r); // a minimized window builds it later
    }
    if (st.failed()) {
        renderer_destroy(r);
        return st;
    }
    return r;
}

void renderer_destroy(Renderer* r) noexcept {
    if (!r) return;
    vkDeviceWaitIdle(r->device);
    destroy_pipelines(r);
    destroy_swapchain_views(r);
    if (r->swapchain) vkDestroySwapchainKHR(r->device, r->swapchain, nullptr);
    for (VkSemaphore& s : r->renderDone)
        if (s) vkDestroySemaphore(r->device, s, nullptr);
    destroy_image(r, r->color);
    destroy_image(r, r->depth);
    destroy_buffer(r, r->readback, r->readbackMem);
    for (Frame& f : r->frames) {
        if (f.pool) vkDestroyCommandPool(r->device, f.pool, nullptr);
        if (f.fence) vkDestroyFence(r->device, f.fence, nullptr);
        if (f.acquired) vkDestroySemaphore(r->device, f.acquired, nullptr);
        destroy_buffer(r, f.ubo, f.uboMemory);
    }
    destroy_buffer(r, r->zero, r->zeroMemory);
    if (r->pipelineLayout) vkDestroyPipelineLayout(r->device, r->pipelineLayout, nullptr);
    if (r->framePool) vkDestroyDescriptorPool(r->device, r->framePool, nullptr);
    if (r->frameSetLayout) vkDestroyDescriptorSetLayout(r->device, r->frameSetLayout, nullptr);
    if (r->vert) vkDestroyShaderModule(r->device, r->vert, nullptr);
    if (r->frag) vkDestroyShaderModule(r->device, r->frag, nullptr);
    delete_object(r->alloc, r, Tag::General);
}

LayoutPipeline const* renderer_pipeline(Renderer* r, mesh::VertexLayout const& layout) noexcept {
    for (LayoutPipeline const& p : r->pipelines)
        if (std::memcmp(&p.layout, &layout, sizeof layout) == 0) return p.pipeline ? &p : nullptr;
    if (r->colorFormat == VK_FORMAT_UNDEFINED) return nullptr; // no target yet (window not shown)
    LayoutPipeline p;
    p.layout      = layout;
    p.streamCount = min<u32>(layout.streamCount, mesh::kMaxStreams);
    p.pipeline    = create_pipeline(r, layout, &p.zeroBindings);
    r->pipelines.push_back(p);
    return p.pipeline ? &r->pipelines.back() : nullptr;
}

VkPipelineLayout renderer_pipeline_layout(Renderer* r) noexcept { return r->pipelineLayout; }
VkBuffer renderer_zero_buffer(Renderer* r) noexcept { return r->zero; }
VkExtent2D renderer_extent(Renderer* r) noexcept { return r->extent; }
FrameUniforms* renderer_uniforms(Renderer* r) noexcept { return r->frames[r->slot].uniforms; }
void renderer_resize(Renderer* r) noexcept { r->needRecreate = !r->offscreen; }
void renderer_wait_idle(Renderer* r) noexcept { vkDeviceWaitIdle(r->device); }
char const* renderer_color_format_name(Renderer* r) noexcept { return vk_format_name(r->colorFormat); }

void renderer_wait_frame(Renderer* r) noexcept {
    r->slot  = u32(r->frameNumber % kFramesInFlight);
    Frame& f = r->frames[r->slot];
    VKX_CHECK(vkWaitForFences(r->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    // Everything the slot's previous frame submitted has finished; deferred objects that no
    // frame still in flight can reference are freed.
    adapter_retire(r->adapter, f.number);
    f.number = ++r->frameNumber;
}

VkCommandBuffer renderer_begin(Renderer* r) noexcept {
    Frame& f            = r->frames[r->slot];
    VkImage target      = r->color.image;
    VkImageView targetV = r->color.view;
    if (!r->offscreen) {
        if (r->needRecreate && !create_swapchain(r)) return VK_NULL_HANDLE;
        VkResult const res = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, f.acquired,
                                                   VK_NULL_HANDLE, &r->imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            r->needRecreate = true; // the semaphore was not signaled; try again next frame
            return VK_NULL_HANDLE;
        }
        if (res == VK_SUBOPTIMAL_KHR)
            r->needRecreate = true; // still usable; rebuilt after this frame
        else if (res != VK_SUCCESS)
            KILN_PANIC("vkAcquireNextImageKHR failed: %s", result_name(res));
        target  = r->swapImages[r->imageIndex];
        targetV = r->swapViews[r->imageIndex];
    }

    VKX_CHECK(vkResetCommandPool(r->device, f.pool, 0));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKX_CHECK(vkBeginCommandBuffer(f.cmd, &begin));

    // Both attachments are cleared, so their old contents are discarded (UNDEFINED). The source
    // scopes order this frame's writes after the previous frame's use of the same images.
    VkImageMemoryBarrier2 toAttachment[2];
    toAttachment[0] = image_barrier(target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    toAttachment[0].srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    toAttachment[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment[1] = image_barrier(r->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    toAttachment[1].srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toAttachment[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toAttachment[1].dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toAttachment[1].dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier(f.cmd, toAttachment, 2);

    VkRenderingAttachmentInfo color{};
    color.sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView                   = targetV;
    color.imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp                      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp                     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = 0.02f;
    color.clearValue.color.float32[1] = 0.025f;
    color.clearValue.color.float32[2] = 0.035f;
    color.clearValue.color.float32[3] = 1.0f;
    VkRenderingAttachmentInfo depth{};
    depth.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView                     = r->depth.view;
    depth.imageLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp                        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp                       = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent    = r->extent;
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color;
    rendering.pDepthAttachment     = &depth;
    vkCmdBeginRendering(f.cmd, &rendering);

    VkViewport viewport{};
    viewport.width    = f32(r->extent.width);
    viewport.height   = f32(r->extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(f.cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = r->extent;
    vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    VkDescriptorSet const sets[2] = {adapter_descriptor_set(r->adapter), f.set};
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipelineLayout, 0, 2, sets, 0,
                            nullptr);
    return f.cmd;
}

void renderer_end(Renderer* r, bool readback) noexcept {
    Frame& f = r->frames[r->slot];
    vkCmdEndRendering(f.cmd);

    if (r->offscreen) {
        if (readback) {
            VkImageMemoryBarrier2 toSrc =
                image_barrier(r->color.image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            toSrc.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toSrc.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toSrc.dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier(f.cmd, &toSrc, 1);
            VkBufferImageCopy2 region{};
            region.sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent      = {r->extent.width, r->extent.height, 1};
            VkCopyImageToBufferInfo2 copy{};
            copy.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
            copy.srcImage       = r->color.image;
            copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy.dstBuffer      = r->readback;
            copy.regionCount    = 1;
            copy.pRegions       = &region;
            vkCmdCopyImageToBuffer2(f.cmd, &copy);
            VkBufferMemoryBarrier2 toHost{};
            toHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            toHost.srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
            toHost.srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask        = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask       = VK_ACCESS_2_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer              = r->readback;
            toHost.size                = VK_WHOLE_SIZE;
            VkDependencyInfo dep{};
            dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers    = &toHost;
            vkCmdPipelineBarrier2(f.cmd, &dep);
            r->readbackRecorded = true;
        }
    } else {
        VkImageMemoryBarrier2 toPresent =
            image_barrier(r->swapImages[r->imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        toPresent.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
        toPresent.dstAccessMask = VK_ACCESS_2_NONE;
        barrier(f.cmd, &toPresent, 1);
    }
    VKX_CHECK(vkEndCommandBuffer(f.cmd));

    // Wait for every upload kiln has published so far before reading vertices or sampling.
    VkSemaphoreSubmitInfo waits[2];
    u32 waitCount              = 0;
    waits[waitCount]           = VkSemaphoreSubmitInfo{};
    waits[waitCount].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[waitCount].semaphore = adapter_timeline(r->adapter);
    waits[waitCount].value     = adapter_upload_watermark(r->adapter);
    waits[waitCount].stageMask =
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ++waitCount;
    VkSemaphoreSubmitInfo signal{};
    if (!r->offscreen) {
        waits[waitCount]           = VkSemaphoreSubmitInfo{};
        waits[waitCount].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[waitCount].semaphore = f.acquired;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ++waitCount;
        signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = r->renderDone[r->imageIndex];
        signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = f.cmd;
    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount   = waitCount;
    submit.pWaitSemaphoreInfos      = waits;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &cmdInfo;
    submit.signalSemaphoreInfoCount = r->offscreen ? 0u : 1u;
    submit.pSignalSemaphoreInfos    = &signal;
    VKX_CHECK(vkResetFences(r->device, 1, &f.fence));
    VKX_CHECK(vkQueueSubmit2(r->dev->graphicsQueue, 1, &submit, f.fence));

    if (!r->offscreen) {
        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &r->renderDone[r->imageIndex];
        present.swapchainCount     = 1;
        present.pSwapchains        = &r->swapchain;
        present.pImageIndices      = &r->imageIndex;
        VkResult const res         = vkQueuePresentKHR(r->dev->graphicsQueue, &present);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
            r->needRecreate = true;
        else if (res != VK_SUCCESS)
            KILN_PANIC("vkQueuePresentKHR failed: %s", result_name(res));
    }
}

Status renderer_read_back(Renderer* r, Vec<u8>* rgba, u32* width, u32* height) noexcept {
    if (!r->offscreen || !r->readbackRecorded) return make_status(Code::InvalidArgument);
    vkDeviceWaitIdle(r->device);
    u32 const w = r->extent.width, h = r->extent.height;
    rgba->resize(usize(w) * h * 4);
    u8 const* src = r->readbackMapped;
    u8* dst       = rgba->data();
    for (usize i = 0; i < usize(w) * h; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = 255; // opaque: the clear color and every material write alpha 1
    }
    *width  = w;
    *height = h;
    return kOk;
}

} // namespace kiln::vkx
