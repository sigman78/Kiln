// examples/sokol/sokol_adapter.h — a kiln adapter over sokol_gfx (docs/design/integration-examples.md).
// sokol_gfx is single-threaded: kiln workers write into CPU memory and queue; Adapter::flush makes
// the images and buffers on the thread that runs the sokol_app frame callback.
#pragma once

#include <kiln/adapter.h>

#include "sokol_gfx.h"

namespace kiln::sk {

struct SokolAdapter;

struct SokolAdapterDesc {
    u32 maxObjects = 4096; ///< images and buffers alive at once
    u32 maxUploads = 256;  ///< uploads between begin_upload and completion
};

/// After sg_setup(). Fills `out`.
[[nodiscard]] Result<SokolAdapter*> sokol_adapter_create(SokolAdapterDesc const& desc, Adapter* out) noexcept;
/// After destroy(ctx), before sg_shutdown().
void sokol_adapter_destroy(SokolAdapter* a) noexcept;

/// The texture view behind a GpuObject that gpu_object() returned; invalid until flush made it.
[[nodiscard]] sg_view sokol_texture(SokolAdapter const* a, GpuObject obj) noexcept;
/// The buffer behind a mesh's GpuObject: vertices and indices, payload at offset 0.
[[nodiscard]] sg_buffer sokol_buffer(SokolAdapter const* a, GpuObject obj) noexcept;
/// SG_VERTEXFORMAT_INVALID for a format sokol cannot fetch.
[[nodiscard]] sg_vertex_format sokol_vertex_format(Format f) noexcept;

} // namespace kiln::sk
