// tests/hdr_writer.h — builds Radiance .hdr files for the tests: a header and flat RGBE texels.
#pragma once

#include "kiln/containers.h"
#include "kiln/log.h"

#include <cstring>

namespace kiln::test::hdr {

/// A flat (not run-length encoded) .hdr file of w x h RGBE texels, top row first. `resolution`
/// replaces the standard "-Y h +X w" line when set.
inline Vec<u8> encode_flat(u32 w, u32 h, Span<u8 const> rgbe, char const* formatName = "32-bit_rle_rgbe",
                           char const* resolution = nullptr) {
    char header[256];
    char res[64];
    if (!resolution) {
        kiln::format(res, sizeof res, "-Y %u +X %u", h, w);
        resolution = res;
    }
    usize const n = kiln::format(header, sizeof header, "#?RADIANCE\nFORMAT=%s\nEXPOSURE=1.0\n\n%s\n",
                                 formatName, resolution);
    Vec<u8> out(default_allocator(), Tag::Test);
    out.append(Span<u8 const>(reinterpret_cast<u8 const*>(header), n));
    out.append(rgbe);
    return out;
}

/// RGBE texels with mantissas from `seed` and exponents 120..136 (values from about 2^-16 to 2).
inline void pattern(u8* rgbe, u32 texels, u32 seed) {
    u32 s = seed;
    for (u32 i = 0; i < texels; ++i) {
        for (u32 c = 0; c < 3; ++c) {
            s               = s * 1664525u + 1013904223u;
            rgbe[i * 4 + c] = u8(128 + (s >> 25)); // a normalized mantissa: 128..255
        }
        rgbe[i * 4 + 3] = u8(120 + (i * 7 + seed) % 17);
    }
}

} // namespace kiln::test::hdr
