// Minimal PNG encoder for tests: stored (uncompressed) deflate blocks inside a zlib
// stream, filter type 0 on every row, correct CRC-32 and Adler-32. Supports bit
// depths 8 and 16 for every color type, palettes with tRNS, and Adam7 interlacing.
// Used to feed kiln::cook::decode_png / cook_texture with known pixels.
#pragma once

#include "kiln/containers.h"

namespace kiln::test::png {

struct PngDesc {
    u32 width              = 0;
    u32 height             = 0;
    u8 colorType           = 6; ///< 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA
    u8 depth               = 8; ///< 8 or 16
    bool interlace         = false;
    Span<u8 const> pixels  = {}; ///< rows top to bottom, samples in PNG order (16-bit: big-endian)
    Span<u8 const> palette = {}; ///< PLTE: RGB triples (color type 3)
    Span<u8 const> trns    = {}; ///< tRNS chunk payload, written if non-empty
};

inline u32 samples_per_pixel(u8 colorType) noexcept {
    switch (colorType) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

inline u32 crc32(u8 const* p, usize n, u32 crc = 0) noexcept {
    crc = ~crc;
    for (usize i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void put_be32(Vec<u8>& out, u32 v) {
    u8 const b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)};
    out.append(Span<u8 const>(b, 4));
}

inline void put_chunk(Vec<u8>& out, char const* type, Span<u8 const> data) {
    put_be32(out, u32(data.size));
    usize const start = out.size();
    out.append(Span<u8 const>(reinterpret_cast<u8 const*>(type), 4));
    out.append(data);
    put_be32(out, crc32(out.data() + start, out.size() - start));
}

/// Encode a PNG. Returns an empty Vec for unsupported input.
inline Vec<u8> encode(PngDesc const& d) {
    Vec<u8> out(default_allocator(), Tag::Test);
    u32 const spp = samples_per_pixel(d.colorType);
    u32 const bpp = spp * (d.depth / 8);
    if (spp == 0 || (d.depth != 8 && d.depth != 16) || d.width == 0 || d.height == 0) return out;
    if (d.pixels.size != usize(d.width) * d.height * bpp) return out;

    // Raw scanlines (filter byte 0 + row), one pass or seven Adam7 passes.
    Vec<u8> raw(default_allocator(), Tag::Test);
    auto emit_pass = [&](u32 x0, u32 y0, u32 dx, u32 dy) {
        if (x0 >= d.width || y0 >= d.height) return;
        for (u32 y = y0; y < d.height; y += dy) {
            raw.push_back(0);
            for (u32 x = x0; x < d.width; x += dx)
                raw.append(Span<u8 const>(d.pixels.data + (usize(y) * d.width + x) * bpp, bpp));
        }
    };
    if (d.interlace) {
        emit_pass(0, 0, 8, 8);
        emit_pass(4, 0, 8, 8);
        emit_pass(0, 4, 4, 8);
        emit_pass(2, 0, 4, 4);
        emit_pass(0, 2, 2, 4);
        emit_pass(1, 0, 2, 2);
        emit_pass(0, 1, 1, 2);
    } else {
        emit_pass(0, 0, 1, 1);
    }

    // zlib: header, stored blocks of <= 65535 bytes, Adler-32.
    Vec<u8> z(default_allocator(), Tag::Test);
    z.push_back(0x78);
    z.push_back(0x01);
    usize pos = 0;
    do {
        usize const n   = min(raw.size() - pos, usize(65535));
        bool const last = pos + n == raw.size();
        z.push_back(u8(last ? 1 : 0));
        z.push_back(u8(n));
        z.push_back(u8(n >> 8));
        z.push_back(u8(~n));
        z.push_back(u8(~n >> 8));
        z.append(Span<u8 const>(raw.data() + pos, n));
        pos += n;
    } while (pos < raw.size());
    u32 a = 1, b = 0;
    for (u8 v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z, (b << 16) | a);

    static constexpr u8 kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.append(Span<u8 const>(kSig, 8));
    u8 const ihdr[13] = {u8(d.width >> 24),
                         u8(d.width >> 16),
                         u8(d.width >> 8),
                         u8(d.width),
                         u8(d.height >> 24),
                         u8(d.height >> 16),
                         u8(d.height >> 8),
                         u8(d.height),
                         d.depth,
                         d.colorType,
                         0,
                         0,
                         u8(d.interlace ? 1 : 0)};
    put_chunk(out, "IHDR", Span<u8 const>(ihdr, 13));
    if (!d.palette.empty()) put_chunk(out, "PLTE", d.palette);
    if (!d.trns.empty()) put_chunk(out, "tRNS", d.trns);
    put_chunk(out, "IDAT", z.span());
    put_chunk(out, "IEND", Span<u8 const>());
    return out;
}

/// Big-endian sample bytes from 16-bit values (PNG order).
inline Vec<u8> be16(Span<u16 const> values) {
    Vec<u8> out(default_allocator(), Tag::Test);
    for (u16 v : values) {
        out.push_back(u8(v >> 8));
        out.push_back(u8(v));
    }
    return out;
}

} // namespace kiln::test::png
