// BC6H (unsigned) block encoder: a scalar C++ port of the BC6H part of the ISPC Texture
// Compressor (kernel.ispc). Output is byte-identical to the ISPC original (sse4 target).
#pragma once

#include <cstdint>

namespace ispc_bc6h {

struct Settings {
    bool slowMode          = false;
    bool fastMode          = false;
    int fastSkipThreshold  = 0;
    int refineIterations1p = 0;
    int refineIterations2p = 0;
};

// The original's profiles (GetProfile_bc6h_*).
inline constexpr Settings kVeryFast{false, true, 0, 0, 0};
inline constexpr Settings kFast{false, true, 2, 0, 1};
inline constexpr Settings kBasic{false, false, 4, 2, 2};
inline constexpr Settings kSlow{true, false, 10, 2, 2};
inline constexpr Settings kVerySlow{true, false, 32, 2, 2};

/// Encodes one 4x4 block. `rgb` holds 16 texels, row by row, as R, G, B half-float bit patterns;
/// each must be a finite, non-negative half (0 to 0x7BFF). Writes 16 bytes to `out`.
void encode_block(uint8_t out[16], uint16_t const rgb[48], Settings const& s) noexcept;

} // namespace ispc_bc6h
