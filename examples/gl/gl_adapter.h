// examples/gl/gl_adapter.h — a kiln adapter over OpenGL 4.6 core: textures bound per draw, or
// bindless (ARB_bindless_texture) with a slot per texture (docs/design/integration-examples.md). GL calls run
// only on the context's thread: kiln workers write into a persistently mapped staging buffer and queue;
// Adapter::flush does the GL work.
#pragma once

#include <kiln/adapter.h>
#include <kiln/alloc.h>
#include <kiln/containers.h>

#include "adapter_stats.h"

namespace kiln::glx {

struct GlAdapter;

struct GlAdapterDesc {
    Allocator const* alloc = nullptr;   ///< the adapter's tables; nullptr = default allocator
    u64 stagingBytes       = 96u << 20; ///< persistently mapped upload ring; a larger upload fails
    u32 maxObjects         = 4096;      ///< textures and buffers alive at once
    u32 maxUploads         = 256;       ///< uploads between begin_upload and completion
    /// bind() records each texture's resident handle for kiln's slot; gl_handle_table() gives a
    /// frame its copy of the table. Needs ARB_bindless_texture, and a host that reports frames
    /// (PumpOptions).
    bool bindless   = false;
    u32 maxSlots    = 4096; ///< Adapter::bindlessSlots
    u32 tableFrames = 4;    ///< copies of the handle table; the host keeps fewer frames in flight
};

/// Creates the adapter and fills `out`. Call on the thread that owns the GL context; call create()
/// and pump() on that thread too, since kiln runs Adapter::flush there.
[[nodiscard]] Result<GlAdapter*> gl_adapter_create(GlAdapterDesc const& desc, Adapter* out) noexcept;
/// After destroy(ctx), on the GL thread.
void gl_adapter_destroy(GlAdapter* a) noexcept;
/// The counters shared by every example adapter; any thread.
[[nodiscard]] ex::AdapterStats gl_adapter_stats(GlAdapter* a) noexcept;

struct GlTexture {
    unsigned name   = 0; ///< 0 = not created yet
    unsigned target = 0; ///< GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP or GL_TEXTURE_2D_ARRAY
};
/// The GL texture behind a GpuObject that gpu_object() returned.
[[nodiscard]] GlTexture gl_texture(GlAdapter const* a, GpuObject obj) noexcept;
/// An ex::ReadTextureFn (`user` is the GlAdapter), for --verify; on the GL thread.
[[nodiscard]] bool gl_read_texture(void* user, GpuObject obj, TextureDesc const& desc, Vec<u8>* out) noexcept;
struct GlBufferRange {
    unsigned buffer = 0;
    u64 offset = 0, size = 0;
};
/// Bindless: the u64 handles, indexed by GpuObject::slot, for frame `frame`; bind the range as a
/// storage buffer. Copies what every slot shows now into copy `frame % tableFrames`, so bind() never
/// changes a table a frame in flight reads: call it once per frame, before the draws, with fewer
/// than tableFrames frames in flight.
[[nodiscard]] GlBufferRange gl_handle_table(GlAdapter* a, u64 frame) noexcept;
/// The GL buffer behind a mesh's GpuObject; the payload starts at offset 0.
[[nodiscard]] unsigned gl_buffer(GlAdapter const* a, GpuObject obj) noexcept;

struct GlVertexFormat {
    int size        = 0; ///< components; 0 = not a vertex format this example reads
    unsigned type   = 0;
    bool normalized = false;
};
[[nodiscard]] GlVertexFormat gl_vertex_format(Format f) noexcept;

} // namespace kiln::glx
