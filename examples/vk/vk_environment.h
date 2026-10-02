// examples/vk/vk_environment.h — one scene-level cube set per frame in flight.
#pragma once

#include "vk_render.h"
#include <kiln/assets.h>

namespace kiln::vkx {

struct Environment {
    TextureHandle texture;
    VkDescriptorSetLayout layout          = VK_NULL_HANDLE;
    VkDescriptorPool pool                 = VK_NULL_HANDLE;
    VkSampler sampler                     = VK_NULL_HANDLE;
    VkDescriptorSet sets[kFramesInFlight] = {};
    u64 stamp                             = 1;
    u64 written[kFramesInFlight]          = {};
    bool present[kFramesInFlight]         = {};

    void create(VkDevice device) {
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings    = &binding;
        VKX_CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout));
        VkDescriptorPoolSize const size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets       = kFramesInFlight;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &size;
        VKX_CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &pool));
        VkDescriptorSetLayout layouts[kFramesInFlight];
        for (auto& l : layouts)
            l = layout;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = pool;
        ai.descriptorSetCount = kFramesInFlight;
        ai.pSetLayouts        = layouts;
        VKX_CHECK(vkAllocateDescriptorSets(device, &ai, sets));
        VkSamplerCreateInfo sci{};
        sci.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode                = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod                                             = VK_LOD_CLAMP_NONE;
        VKX_CHECK(vkCreateSampler(device, &sci, nullptr, &sampler));
    }

    /// After renderer_wait_frame(): only the completed frame slot may be rewritten. True if it was.
    bool update(VkDevice device, VkAdapter* adapter, Context* ctx, u32 slot) {
        if (written[slot] == stamp) return false;
        TextureView view = texture ? adapter_texture(adapter, gpu_object(ctx, texture)) : TextureView{};
        present[slot]    = view.view && view.shape == TextureShape::Cube;
        if (!present[slot])
            view =
                adapter_texture(adapter, placeholder_object(ctx, TextureKind::Emissive, TextureShape::Cube));
        VkDescriptorImageInfo const image{sampler, view.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = sets[slot];
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        written[slot] = stamp;
        return true;
    }

    void bind(Renderer* renderer, VkCommandBuffer cmd, u32 slot) const {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer_pipeline_layout(renderer), 2,
                                1, &sets[slot], 0, nullptr);
    }

    void release(VkDevice device) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        vkDestroySampler(device, sampler, nullptr);
    }
};

} // namespace kiln::vkx
