// examples/sokol/readback.h — the pixels of a color attachment, for --dump.
#pragma once

#include <kiln/containers.h>

#include "sokol_gfx.h"

namespace kiln::sk {

/// RGBA8, top row first. False on a backend without readback (Metal) or on failure.
bool read_rgba(sg_image image, u32 width, u32 height, Vec<u8>& out) noexcept;

} // namespace kiln::sk
