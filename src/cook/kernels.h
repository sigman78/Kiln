// src/cook/kernels.h — cook kernels: the per-texel hot paths of the image pipeline.
// Internal to kiln_cook, never installed. Design: docs/design/cook-kernels.md.
//
// Every kernel works on a row range [rowBegin, rowEnd) of its context, so a caller can
// split an image into row bands. Format parameters are template parameters; the
// *_kernel() helpers switch on the runtime values once and return a function pointer.
//
// | Kernel            | Parameters                       | Split | SIMD                                 |
// |-------------------|----------------------------------|-------|--------------------------------------|
// | convert_rows      | src/dst bits, channels, grayAlpha | rows  | auto                                 |
// | flip_green_rows   | bits, channels                   | rows  | auto                                 |
// | renormalize_rows  | bits, channels                   | rows  | scalar (double sqrt per texel)       |
// | prepare_rows      | the three kernels above          | rows  | as its parts; fused per row          |
// | downsample_rows   | bits, channels, mode             | rows  | auto (linear); scalar (sRGB, renorm) |
#pragma once

#include "kiln/core.h"

#include <cmath>
#include <cstring>

// No FMA contraction (see image.cpp); repeated here so the kernels never depend on
// include order.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace kiln::cook::kernels {

// ---------------------------------------------------------------------------
// sRGB tables
// ---------------------------------------------------------------------------

/// Generated offline in double with the exact sRGB EOTF (IEC 61966-2-1):
///   table[v] = round(65535 * f(v / 255)),  f(c) = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055)^2.4
/// No entry is within 1e-6 of a .5 tie, so the rounding mode does not matter.
/// Strictly increasing, so kLinear16ToSrgb8 inverts it exactly.
inline constexpr u16 kSrgbToLinear16[256] = {
    0,     20,    40,    60,    80,    99,    119,   139,   159,   179,   199,   219,   241,   264,   288,
    313,   340,   367,   396,   427,   458,   491,   526,   562,   599,   637,   677,   718,   761,   805,
    851,   898,   947,   997,   1048,  1101,  1156,  1212,  1270,  1330,  1391,  1453,  1517,  1583,  1651,
    1720,  1790,  1863,  1937,  2013,  2090,  2170,  2250,  2333,  2418,  2504,  2592,  2681,  2773,  2866,
    2961,  3058,  3157,  3258,  3360,  3464,  3570,  3678,  3788,  3900,  4014,  4129,  4247,  4366,  4488,
    4611,  4736,  4864,  4993,  5124,  5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,  6395,  6547,
    6700,  6856,  7014,  7174,  7335,  7500,  7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,  9072,
    9258,  9445,  9635,  9828,  10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874, 15122, 15371, 15623,
    15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001, 18277, 18556, 18837, 19121, 19407, 19696,
    19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329,
    24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542,
    29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143, 34544, 34948, 35355,
    35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891, 41337, 41785,
    42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359, 48850,
    49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952,
    65535,
};

struct Linear16ToSrgb8Table {
    u8 v[65536];
};

/// Nearest sRGB code for every linear 16-bit value: the smallest index whose entry is
/// >= v, then the nearer of it and its predecessor (ties go to the lower index). The
/// search index only moves forward as v grows, so this is one sweep over both tables.
constexpr Linear16ToSrgb8Table make_linear16_to_srgb8() noexcept {
    Linear16ToSrgb8Table t{};
    u32 lo = 0;
    for (u32 v = 0; v < 65536; ++v) {
        while (kSrgbToLinear16[lo] < v) // entry 255 is 65535, so this stops
            ++lo;
        u32 i = lo;
        if (i > 0 && v - kSrgbToLinear16[i - 1] <= u32(kSrgbToLinear16[i]) - v) --i;
        t.v[v] = u8(i);
    }
    return t;
}

inline constexpr Linear16ToSrgb8Table kLinear16ToSrgb8 = make_linear16_to_srgb8();

// ---------------------------------------------------------------------------
// Texel access
// ---------------------------------------------------------------------------

template <u32 Bits> inline constexpr u32 kMaxValue = Bits == 8 ? 255u : 65535u;

template <u32 Bits> KILN_FORCEINLINE u32 load(u8 const* p) noexcept {
    if constexpr (Bits == 8) {
        return *p;
    } else {
        u16 v;
        std::memcpy(&v, p, 2);
        return v;
    }
}

template <u32 Bits> KILN_FORCEINLINE void store(u8* p, u32 v) noexcept {
    if constexpr (Bits == 8) {
        *p = u8(v);
    } else {
        u16 const s = u16(v);
        std::memcpy(p, &s, 2);
    }
}

/// 16 <-> 8 bit conversion rules from image.h.
template <u32 From, u32 To> KILN_FORCEINLINE u32 convert_depth(u32 v) noexcept {
    if constexpr (From == 16 && To == 8)
        return (v * 255u + 32767u) / 65535u;
    else if constexpr (From == 8 && To == 16)
        return v * 257u;
    else
        return v;
}

/// Renormalize one RGB triple (values in 0..max) as a unit vector. The operation
/// order is part of the output format: do not reorder or simplify.
template <u32 Bits> KILN_FORCEINLINE void renormalize_px(u32* rgb) noexcept {
    constexpr u32 maxv = kMaxValue<Bits>;
    double const half  = double(maxv) * 0.5; // 127.5 or 32767.5
    double const x     = double(rgb[0]) / half - 1.0;
    double const y     = double(rgb[1]) / half - 1.0;
    double const z     = double(rgb[2]) / half - 1.0;
    double const len2  = x * x + y * y + z * z;
    double n[3]        = {0.0, 0.0, 1.0}; // a zero vector becomes the flat normal
    if (len2 > 0.0) {
        double const len = std::sqrt(len2);
        n[0]             = x / len;
        n[1]             = y / len;
        n[2]             = z / len;
    }
    for (u32 c = 0; c < 3; ++c) {
        double const e = std::floor((n[c] + 1.0) * half + 0.5); // round half up
        rgb[c]         = e <= 0.0 ? 0u : (e >= double(maxv) ? maxv : u32(e));
    }
}

// ---------------------------------------------------------------------------
// Row kernels over one image: convert, flip green, renormalize, prepare
// ---------------------------------------------------------------------------

/// Tightly packed rows. In-place kernels use only `dst`.
struct RowCtx {
    u8 const* src     = nullptr;
    u8* dst           = nullptr;
    usize srcRowBytes = 0;
    usize dstRowBytes = 0;
    u32 width         = 0;
};

using RowFn = void (*)(RowCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept;

/// Channel count / bit depth conversion (rules in image.h). `GrayAlpha`: a 2-channel
/// source is gray+alpha, so Y fills R, G and B and A stays alpha.
template <u32 SrcBits, u32 SrcChannels, u32 DstBits, u32 DstChannels, bool GrayAlpha>
KILN_HOT void convert_rows(RowCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    constexpr u32 sbpc = SrcBits / 8, sbpp = SrcChannels * sbpc;
    constexpr u32 dbpc = DstBits / 8, dbpp = DstChannels * dbpc;
    u32 const width = ctx.width;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8 const* KILN_RESTRICT s = ctx.src + usize(y) * ctx.srcRowBytes;
        u8* KILN_RESTRICT d       = ctx.dst + usize(y) * ctx.dstRowBytes;
        if constexpr (SrcBits == DstBits && SrcChannels == DstChannels && !GrayAlpha) {
            std::memcpy(d, s, usize(width) * sbpp);
        } else {
            for (u32 x = 0; x < width; ++x, s += sbpp, d += dbpp) {
                for (u32 c = 0; c < DstChannels; ++c) {
                    u32 v;
                    if constexpr (GrayAlpha)
                        v = c == 3 ? load<SrcBits>(s + sbpc) : load<SrcBits>(s); // (Y, Y, Y, A)
                    else if (c < SrcChannels)
                        v = load<SrcBits>(s + c * sbpc);
                    else if (c == 3)
                        v = kMaxValue<SrcBits>; // added alpha is opaque
                    else if constexpr (SrcChannels == 1)
                        v = load<SrcBits>(s); // gray -> G, B
                    else
                        v = 0; // RG -> RGB(A): B = 0
                    store<DstBits>(d + c * dbpc, convert_depth<SrcBits, DstBits>(v));
                }
            }
        }
    }
}

/// Green channel v -> max - v, in place.
template <u32 Bits, u32 Channels>
KILN_HOT void flip_green_rows(RowCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    static_assert(Channels >= 2);
    constexpr u32 bpc = Bits / 8, bpp = Channels * bpc;
    u32 const width = ctx.width;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8* KILN_RESTRICT g = ctx.dst + usize(y) * ctx.dstRowBytes + bpc;
        for (u32 x = 0; x < width; ++x, g += bpp)
            store<Bits>(g, kMaxValue<Bits> - load<Bits>(g));
    }
}

/// RGB renormalized as unit vectors, in place.
template <u32 Bits, u32 Channels>
KILN_HOT void renormalize_rows(RowCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    static_assert(Channels >= 3);
    constexpr u32 bpc = Bits / 8, bpp = Channels * bpc;
    u32 const width = ctx.width;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8* KILN_RESTRICT p = ctx.dst + usize(y) * ctx.dstRowBytes;
        for (u32 x = 0; x < width; ++x, p += bpp) {
            u32 rgb[3] = {load<Bits>(p), load<Bits>(p + bpc), load<Bits>(p + 2 * bpc)};
            renormalize_px<Bits>(rgb);
            for (u32 c = 0; c < 3; ++c)
                store<Bits>(p + c * bpc, rgb[c]);
        }
    }
}

/// Level 0 preparation fused per row: each destination row is converted, then flipped
/// and renormalized while it is still in cache. One pass over the source.
struct PrepareCtx {
    RowCtx rows;
    RowFn convert     = nullptr;
    RowFn flipGreen   = nullptr; ///< null: skip
    RowFn renormalize = nullptr; ///< null: skip
};

KILN_HOT inline void prepare_rows(PrepareCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        ctx.convert(ctx.rows, y, y + 1);
        if (ctx.flipGreen) ctx.flipGreen(ctx.rows, y, y + 1);
        if (ctx.renormalize) ctx.renormalize(ctx.rows, y, y + 1);
    }
}

// ---------------------------------------------------------------------------
// 2x2 box downsample
// ---------------------------------------------------------------------------

enum class DownsampleMode : u8 {
    Linear, ///< every channel averaged as stored
    Srgb,   ///< 8-bit only: channels 0..2 averaged in linear light, alpha linear
    Renorm, ///< 3+ channels: average, then renormalize RGB as a unit vector
};

/// Output row y reads source rows 2y and 2y + 1, output column x reads columns 2x and
/// 2x + 1. With floor halving those never pass the last row or column, so the loop has
/// no clamps; a source dimension of 1 sets the step to the second sample to 0.
struct DownsampleCtx {
    u8 const* src     = nullptr;
    u8* dst           = nullptr;
    usize srcRowBytes = 0;
    usize dstRowBytes = 0;
    u32 dstWidth      = 0;
    usize colStep     = 0; ///< bytes from the first to the second column sample (0 or bpp)
    usize rowStep     = 0; ///< bytes from the first to the second row sample (0 or srcRowBytes)
};

using DownsampleFn = void (*)(DownsampleCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept;

template <u32 Bits, u32 Channels, DownsampleMode Mode>
KILN_HOT void downsample_rows(DownsampleCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    static_assert(Mode != DownsampleMode::Srgb || Bits == 8, "sRGB averaging is 8-bit only");
    static_assert(Mode != DownsampleMode::Renorm || Channels >= 3, "renormalize needs RGB");
    constexpr u32 bpc = Bits / 8, bpp = Channels * bpc;
    constexpr u32 srgbCh = Mode == DownsampleMode::Srgb ? (Channels < 3 ? Channels : 3) : 0;
    u32 const width      = ctx.dstWidth;
    usize const colStep  = ctx.colStep;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8 const* KILN_RESTRICT r0 = ctx.src + usize(2 * y) * ctx.srcRowBytes;
        u8 const* KILN_RESTRICT r1 = r0 + ctx.rowStep;
        u8* KILN_RESTRICT d        = ctx.dst + usize(y) * ctx.dstRowBytes;
        for (u32 x = 0; x < width; ++x, r0 += 2 * bpp, r1 += 2 * bpp, d += bpp) {
            u32 out[Channels] = {};
            for (u32 c = 0; c < srgbCh; ++c) {
                u32 const sum = 2 + kSrgbToLinear16[r0[c]] + kSrgbToLinear16[r0[colStep + c]] +
                                kSrgbToLinear16[r1[c]] + kSrgbToLinear16[r1[colStep + c]];
                out[c] = kLinear16ToSrgb8.v[sum / 4];
            }
            for (u32 c = srgbCh; c < Channels; ++c) {
                u32 const off = c * bpc;
                u32 const sum = 2 + load<Bits>(r0 + off) + load<Bits>(r0 + colStep + off) +
                                load<Bits>(r1 + off) + load<Bits>(r1 + colStep + off);
                out[c] = sum / 4; // the +2 rounds to nearest
            }
            if constexpr (Mode == DownsampleMode::Renorm) renormalize_px<Bits>(out);
            for (u32 c = 0; c < Channels; ++c)
                store<Bits>(d + c * bpc, out[c]);
        }
    }
}

// ---------------------------------------------------------------------------
// Dispatch: one switch on the runtime format, outside every loop
// ---------------------------------------------------------------------------

namespace detail {

template <u32 SB, u32 SC, u32 DB, u32 DC> RowFn convert_pick(bool grayAlpha) noexcept {
    if constexpr (SC == 2 && DC >= 3) {
        if (grayAlpha) return &convert_rows<SB, SC, DB, DC, true>;
    }
    return &convert_rows<SB, SC, DB, DC, false>;
}

template <u32 SB, u32 SC, u32 DB> RowFn convert_dst_channels(u32 dc, bool grayAlpha) noexcept {
    switch (dc) {
    case 1: return convert_pick<SB, SC, DB, 1>(grayAlpha);
    case 2: return convert_pick<SB, SC, DB, 2>(grayAlpha);
    case 3: return convert_pick<SB, SC, DB, 3>(grayAlpha);
    case 4: return convert_pick<SB, SC, DB, 4>(grayAlpha);
    default: return nullptr;
    }
}

template <u32 SB, u32 SC> RowFn convert_dst(u32 db, u32 dc, bool grayAlpha) noexcept {
    switch (db) {
    case 8: return convert_dst_channels<SB, SC, 8>(dc, grayAlpha);
    case 16: return convert_dst_channels<SB, SC, 16>(dc, grayAlpha);
    default: return nullptr;
    }
}

template <u32 SB> RowFn convert_src_channels(u32 sc, u32 db, u32 dc, bool grayAlpha) noexcept {
    switch (sc) {
    case 1: return convert_dst<SB, 1>(db, dc, grayAlpha);
    case 2: return convert_dst<SB, 2>(db, dc, grayAlpha);
    case 3: return convert_dst<SB, 3>(db, dc, grayAlpha);
    case 4: return convert_dst<SB, 4>(db, dc, grayAlpha);
    default: return nullptr;
    }
}

template <u32 B> RowFn flip_green_for(u32 channels) noexcept {
    switch (channels) {
    case 2: return &flip_green_rows<B, 2>;
    case 3: return &flip_green_rows<B, 3>;
    case 4: return &flip_green_rows<B, 4>;
    default: return nullptr;
    }
}

template <u32 B> RowFn renormalize_for(u32 channels) noexcept {
    switch (channels) {
    case 3: return &renormalize_rows<B, 3>;
    case 4: return &renormalize_rows<B, 4>;
    default: return nullptr;
    }
}

template <u32 B, u32 C> DownsampleFn downsample_mode(DownsampleMode mode) noexcept {
    if constexpr (C >= 3) {
        if (mode == DownsampleMode::Renorm) return &downsample_rows<B, C, DownsampleMode::Renorm>;
    }
    if constexpr (B == 8) {
        if (mode == DownsampleMode::Srgb) return &downsample_rows<B, C, DownsampleMode::Srgb>;
    }
    return &downsample_rows<B, C, DownsampleMode::Linear>;
}

template <u32 B> DownsampleFn downsample_for(u32 channels, DownsampleMode mode) noexcept {
    switch (channels) {
    case 1: return downsample_mode<B, 1>(mode);
    case 2: return downsample_mode<B, 2>(mode);
    case 3: return downsample_mode<B, 3>(mode);
    case 4: return downsample_mode<B, 4>(mode);
    default: return nullptr;
    }
}

} // namespace detail

/// Converter between two formats; null for an unsupported one. `grayAlpha` only
/// matters for a 2-channel source going to 3 or 4 channels.
inline RowFn convert_kernel(u32 srcBits, u32 srcChannels, u32 dstBits, u32 dstChannels,
                            bool grayAlpha) noexcept {
    switch (srcBits) {
    case 8: return detail::convert_src_channels<8>(srcChannels, dstBits, dstChannels, grayAlpha);
    case 16: return detail::convert_src_channels<16>(srcChannels, dstBits, dstChannels, grayAlpha);
    default: return nullptr;
    }
}

/// Null when there is nothing to flip (fewer than 2 channels) or the format is unsupported.
inline RowFn flip_green_kernel(u32 bits, u32 channels) noexcept {
    return bits == 8 ? detail::flip_green_for<8>(channels)
                     : (bits == 16 ? detail::flip_green_for<16>(channels) : nullptr);
}

/// Null when there is nothing to renormalize (fewer than 3 channels) or the format is
/// unsupported.
inline RowFn renormalize_kernel(u32 bits, u32 channels) noexcept {
    return bits == 8 ? detail::renormalize_for<8>(channels)
                     : (bits == 16 ? detail::renormalize_for<16>(channels) : nullptr);
}

/// `mode` falls back to Linear where it does not apply (Srgb on 16-bit, Renorm on
/// fewer than 3 channels). Null for an unsupported format.
inline DownsampleFn downsample_kernel(u32 bits, u32 channels, DownsampleMode mode) noexcept {
    return bits == 8 ? detail::downsample_for<8>(channels, mode)
                     : (bits == 16 ? detail::downsample_for<16>(channels, mode) : nullptr);
}

} // namespace kiln::cook::kernels
