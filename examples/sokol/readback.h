// examples/sokol/readback.h — the pixels of a color attachment (--dump) and the blocks of a texture
// (--verify).
#pragma once

#include <kiln/adapter.h>
#include <kiln/containers.h>

#include "sokol_gfx.h"

namespace kiln::sk {

/// RGBA8, top row first. False on a backend without readback (Metal) or on failure.
bool read_rgba(sg_image image, u32 width, u32 height, Vec<u8>& out) noexcept;

/// Every level of `image`, each level's layers in order, rows tightly packed (whole blocks): what
/// ex::ReadTextureFn returns. D3D11 any format; GL block formats only; false elsewhere.
bool read_texture(sg_image image, TextureDesc const& desc, Vec<u8>& out) noexcept;

} // namespace kiln::sk
