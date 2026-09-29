// examples/viewer/vk_adapter.h — the example kiln::Adapter on raw Vulkan 1.4: self-submitting
// uploads on a transfer queue with a timeline semaphore, bindless texture slots, deferred
// destroy by frames in flight. Design: docs/design/viewer.md.
#pragma once

#include <kiln/adapter.h>
#include <kiln/alloc.h>

#include "vk_device.h"

namespace kiln::vkx {

struct AdapterDesc {
    Device const* device   = nullptr; ///< required; outlives the adapter
    Allocator const* alloc = nullptr; ///< nullptr = default allocator (Tag::Payload for tables)
    u64 stagingBytes       = 64u << 20;
    u32 maxSlots           = 4096; ///< bindless sampled-image slots
    u32 maxObjects         = 8192; ///< images + buffers alive at once
    u32 framesInFlight     = 2;    ///< destroy_deferred delay, in frames
    /// true: acquire() hands out bindless slots and publish() writes them. false: no acquire(); the
    /// host binds each texture's image view itself (adapter_texture), as kiln-vk-basic does.
    bool bindless = true;
};

struct VkAdapter;

/// Fills `out` (kSelfSubmitting, kCubeTextures, kArrayTextures, kMeshes; bindless unless
/// AdapterDesc::bindless is false). `out` must outlive its users.
[[nodiscard]] Result<VkAdapter*> adapter_create(AdapterDesc const& desc, Adapter* out) noexcept;
/// Waits for the transfer queue to go idle, then frees everything, including deferred objects.
void adapter_destroy(VkAdapter* a) noexcept;

// --- What the renderer needs from the adapter (render thread) ---------------------------

/// The bindless set: bindings 0, 1, 2 = sampler2D, samplerCube and sampler2DArray arrays of
/// `maxSlots` (combined image samplers, partially bound, update after bind), one per
/// TextureShape. A slot index is shared: a texture's slot is valid in the binding of its shape.
/// The viewer binds the set once per frame.
[[nodiscard]] VkDescriptorSetLayout adapter_set_layout(VkAdapter* a) noexcept;
[[nodiscard]] VkDescriptorSet adapter_descriptor_set(VkAdapter* a) noexcept;

/// Timeline semaphore and the value that covers every upload published so far. A frame
/// submit that waits on (semaphore, value) may sample any slot kiln has published.
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

/// Frees objects that destroy_deferred received at least framesInFlight frames before
/// `completedFrame`. Call once per frame after the frame's fence has been waited.
void adapter_retire(VkAdapter* a, u64 completedFrame) noexcept;

struct AdapterStats {
    u32 uploadsInFlight = 0;
    u32 busyReturned    = 0; ///< begin_upload calls that returned Busy (staging full)
    u64 bytesUploaded   = 0;
    u32 liveObjects     = 0;
    u32 slotsInUse      = 0;
    u64 stagingUsed     = 0; ///< bytes of the ring currently reserved
};
[[nodiscard]] AdapterStats adapter_stats(VkAdapter* a) noexcept;

} // namespace kiln::vkx
