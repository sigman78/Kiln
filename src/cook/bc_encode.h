// src/cook/bc_encode.h — BC1/3/4/5/7 block encoding of one image. Internal to kiln_cook.
// Design: docs/design/bcn-encoding.md.
#pragma once

#include "kiln/cook/image.h"
#include "kiln/cook/settings.h"
#include "kiln/formats.h"

namespace kiln::cook {

/// Encodes `img` as `format` (a BC1_RGB, BC3, BC4_UNORM, BC5_UNORM or BC7 format) and appends the
/// blocks, row by row, to `out`. `img` is 8-bit with 4 channels, or 1 (BC4) or 2 (BC4, BC5)
/// channels; BC4 reads channel 0, BC5 channels 0 and 1. Edge blocks repeat the last row and
/// column. The bytes do not depend on `budget`.
void bc_encode(Image const& img, Format format, EncodeQuality quality, Vec<u8>& out,
               JobBudget const& budget) noexcept;

} // namespace kiln::cook
