// examples/gl/gl_adapter.h — a kiln adapter over OpenGL 4.6 core: textures bound per draw, or
// bindless (ARB_bindless_texture) with a slot per texture (docs/design/integration-examples.md). GL calls run
// only on the context's thread: kiln workers write into a persistently mapped staging buffer and queue;
// Adapter::flush does the GL work.
#pragma once

#include <kiln/adapter.h>

namespace kiln::glx {

struct GlAdapter;

struct GlAdapterDesc {
    u64 stagingBytes = 96u << 20; ///< persistently mapped upload ring; a larger upload fails
    u32 maxObjects   = 4096;      ///< textures and buffers alive at once
    u32 maxUploads   = 256;       ///< uploads between begin_upload and completion
    /// bind() writes each texture's resident handle into kiln's slot of a table (gl_handle_table).
    /// Needs ARB_bindless_texture, and a host that reports frames (PumpOptions).
    bool bindless = false;
    u32 maxSlots  = 4096; ///< Adapter::bindlessSlots
};

/// Creates the adapter and fills `out`. Call on the thread that owns the GL context; call create()
/// and pump() on that thread too, since kiln runs Adapter::flush there.
[[nodiscard]] Result<GlAdapter*> gl_adapter_create(GlAdapterDesc const& desc, Adapter* out) noexcept;
/// After destroy(ctx), on the GL thread.
void gl_adapter_destroy(GlAdapter* a) noexcept;

struct GlTexture {
    unsigned name   = 0; ///< 0 = not created yet
    unsigned target = 0; ///< GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP or GL_TEXTURE_2D_ARRAY
};
/// The GL texture behind a GpuObject that gpu() returned.
[[nodiscard]] GlTexture gl_texture(GlAdapter const* a, GpuObject obj) noexcept;
/// Bindless: the buffer of u64 handles, indexed by GpuObject::slot; bind it as a storage buffer.
[[nodiscard]] unsigned gl_handle_table(GlAdapter const* a) noexcept;
/// The GL buffer behind a mesh's GpuObject; the payload starts at offset 0.
[[nodiscard]] unsigned gl_buffer(GlAdapter const* a, GpuObject obj) noexcept;

struct GlVertexFormat {
    int size        = 0; ///< components; 0 = not a vertex format this example reads
    unsigned type   = 0;
    bool normalized = false;
};
[[nodiscard]] GlVertexFormat gl_vertex_format(Format f) noexcept;

} // namespace kiln::glx
