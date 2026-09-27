// formats_internal.h — helpers shared by the src/formats/*.cpp readers and writers.
#pragma once

#include "kiln/core.h"

namespace kiln::fmt {

constexpr u32 gcd(u32 a, u32 b) noexcept {
    while (b != 0) {
        u32 t = a % b;
        a     = b;
        b     = t;
    }
    return a;
}
constexpr u32 lcm(u32 a, u32 b) noexcept { return a / gcd(a, b) * b; }

/// KTX 2.0 level alignment without supercompression: lcm(texel block size, 4).
constexpr u32 ktx2_level_align(u32 bytesPerBlock) noexcept { return lcm(bytesPerBlock, 4u); }

} // namespace kiln::fmt
