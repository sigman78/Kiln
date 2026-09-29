// examples/viewer/vk_adapter.h — the example kiln::Adapter on raw Vulkan 1.4: self-submitting
// uploads on a transfer queue with a timeline semaphore, bindless texture slots.
// Design: docs/design/viewer.md.
#pragma once

#include <kiln/adapter.h>
#include <kiln/alloc.h>

#include "adapter_stats.h"
#include "vk_device.h"

namespace kiln::vkx {

struct AdapterDesc {
    Device const* device   = nullptr; ///< required; outlives the adapter
    Allocator const* alloc = nullptr; ///< nullptr = default allocator (Tag::Payload for tables)
    u64 stagingBytes       = 64u << 20;
    u32 maxSlots           = 4096; ///< bindless sampled-image slots (Adapter::bindlessSlots)
    u32 maxObjects         = 8192; ///< images + buffers alive at once
    /// true: bind() writes kiln's slots into the bindless set. false: the host binds each
    /// texture's image view itself (adapter_texture), as kiln-vk-basic does.
    bool bindless = true;
};

struct VkAdapter;

/// Fills `out` (kSelfSubmitting, kCubeTextures, kArrayTextures, kMeshes; bindless unless
/// AdapterDesc::bindless is false). `out` must outlive its users.
[[nodiscard]] Result<VkAdapter*> adapter_create(AdapterDesc const& desc, Adapter* out) noexcept;
/// Waits for the transfer queue to go idle, then frees everything.
void adapter_destroy(VkAdapter* a) noexcept;

// --- What the renderer needs from the adapter (render thread) ---------------------------

/// The bindless set: bindings 0, 1, 2 = sampler2D, samplerCube and sampler2DArray arrays of
/// `maxSlots` (combined image samplers, partially bound, update after bind), one per
/// TextureShape. A slot index is shared: a texture's slot is valid in the binding of its shape.
/// The viewer binds the set once per frame.
[[nodiscard]] VkDescriptorSetLayout adapter_set_layout(VkAdapter* a) noexcept;
[[nodiscard]] VkDescriptorSet adapter_descriptor_set(VkAdapter* a) noexcept;

/// Timeline semaphore and the value that covers every upload kiln uses so far. A frame
/// submit that waits on (semaphore, value) may sample any slot kiln has bound.
[[nodiscard]] VkSemaphore adapter_timeline(VkAdapter* a) noexcept;
[[nodiscard]] u64 adapter_upload_watermark(VkAdapter* a) noexcept;

/// A mesh payload: the buffer and the offset of the decoded payload inside it. The .mesh
/// stream and index offsets are relative to `offset`. Null buffer if `obj` is not a mesh.
struct MeshPayload {
    VkBuffer buffer         = VK_NULL_HANDLE;
    VkDeviceSize offset     = 0;
    VkDeviceSize size       = 0;
    VkDeviceAddress address = 0; ///< device address of the payload start
};
[[nodiscard]] MeshPayload adapter_mesh(VkAdapter* a, GpuObject obj) noexcept;

/// The image view behind a texture's GpuObject (placeholder or real), for hosts that write their
/// own descriptor sets. Null if `obj` is not a texture.
struct TextureView {
    VkImageView view   = VK_NULL_HANDLE;
    TextureShape shape = TextureShape::Tex2D;
};
[[nodiscard]] TextureView adapter_texture(VkAdapter* a, GpuObject obj) noexcept;

/// The counters shared by every example adapter; any thread. A submitted copy never fails, and the
/// only Busy is a full staging ring (a full object table is OutOfMemory).
[[nodiscard]] ex::AdapterStats adapter_stats(VkAdapter* a) noexcept;

} // namespace kiln::vkx
