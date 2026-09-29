// examples/nga/nga_adapter.h — a kiln adapter over NoGraphicsAPI (docs/design/integration-examples.md).
// Mesh payloads are written by kiln straight into CPU-visible GPU memory and read by shaders through
// GPU pointers; textures are copied from a staging ring and named by descriptor heap indices.
#pragma once

#include <kiln/adapter.h>

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

namespace kiln::nga {

struct NgaAdapter;

struct NgaAdapterDesc {
    gpu::Device* device = nullptr;    ///< required; outlives the adapter
    u64 stagingBytes    = 64u << 20;  ///< CPU-visible ring for texture uploads
    u64 meshBytes       = 64u << 20;  ///< CPU-visible heap the mesh payloads live in
    u64 textureBytes    = 512u << 20; ///< GPU-only texture heap
    u32 maxSlots        = 4096;       ///< Adapter::bindlessSlots
    u32 maxDescriptors  = 4096;       ///< texture descriptor heap capacity
};

/// Fills `out`: bindless slots through bind(), the GPU work in Adapter::flush (so pump() and create()
/// run on the thread that submits to queue 0). Call on that thread. The host reports its frames
/// (PumpOptions): descriptors are freed only after the frames that read them.
[[nodiscard]] Result<NgaAdapter*> nga_adapter_create(NgaAdapterDesc const& desc, Adapter* out) noexcept;
/// After destroy(ctx). Waits for the adapter's own submissions.
void nga_adapter_destroy(NgaAdapter* a) noexcept;

/// Set these before drawing: textures by descriptor index, samplers 0 (repeat) and 1 (clamp).
[[nodiscard]] gpu::TextureDescriptorHeap* nga_texture_heap(NgaAdapter* a) noexcept;
[[nodiscard]] gpu::SamplerDescriptorHeap* nga_sampler_heap(NgaAdapter* a) noexcept;

/// The descriptor index a slot shows now: the placeholder until the texture arrives, then the texture.
/// A descriptor is written once and never changed while a frame may use it; bind() moves the
/// slot to another descriptor instead. kInvalid for a slot never bound.
[[nodiscard]] u32 nga_descriptor(NgaAdapter* a, u32 slot) noexcept;

struct NgaMesh {
    u64 gpu  = 0; ///< GPU address of the payload; stream and index offsets are relative to it
    u64 size = 0;
};
/// The mesh payload behind a GpuObject that gpu() returned (zero until Ready).
[[nodiscard]] NgaMesh nga_mesh(NgaAdapter* a, GpuObject obj) noexcept;

} // namespace kiln::nga
