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
// | renormalize_rows  | bits, channels                   | rows  | SSE2 (x64), scalar elsewhere         |
// | prepare_rows      | the three kernels above          | rows  | as its parts; fused per row          |
// | downsample_rows   | bits, channels, Linear or Srgb   | rows  | auto (linear); scalar (sRGB)         |
// | downsample_rows   | bits, channels, Renorm           | rows  | SSE2 (x64), scalar elsewhere         |
#pragma once

#include "kiln/core.h"

#include <cmath>
#include <cstring>

#if defined(KILN_ARCH_X64)
#include <emmintrin.h> // SSE2, the x64 baseline: no ISA check needed
#endif

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

struct UnitFrom8Table {
    double v[256];
};

/// First renormalize step for 8-bit values: v[i] = double(i) / 127.5 - 1.0, evaluated
/// at compile time in IEEE double with the same two correctly rounded operations the
/// runtime expression performs, so a lookup is bit-identical to computing it. Saves
/// three of the seven dependent divides per texel; 16-bit keeps the divides (a
/// 65536-entry table would not stay in cache).
constexpr UnitFrom8Table make_unit_from8() noexcept {
    UnitFrom8Table t{};
    for (u32 i = 0; i < 256; ++i)
        t.v[i] = double(i) / 127.5 - 1.0;
    return t;
}

inline constexpr UnitFrom8Table kUnitFrom8 = make_unit_from8();

/// A channel value mapped to [-1, 1]: double(v) / half - 1.0, from kUnitFrom8 for 8-bit.
template <u32 Bits> KILN_FORCEINLINE double to_unit(u32 v) noexcept {
    if constexpr (Bits == 8)
        return kUnitFrom8.v[v];
    else
        return double(v) / (double(kMaxValue<Bits>) * 0.5) - 1.0;
}

/// Renormalize one RGB triple (values in 0..max) as a unit vector. The operation
/// order is part of the output format: do not reorder or simplify.
template <u32 Bits> KILN_FORCEINLINE void renormalize_px(u32* rgb) noexcept {
    constexpr u32 maxv = kMaxValue<Bits>;
    double const half  = double(maxv) * 0.5; // 127.5 or 32767.5
    double const x     = to_unit<Bits>(rgb[0]);
    double const y     = to_unit<Bits>(rgb[1]);
    double const z     = to_unit<Bits>(rgb[2]);
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

#if defined(KILN_ARCH_X64)
/// renormalize_px<Bits> for two texels at once, in place on the first three values of
/// `a` and `b`: lane 0 of every vector is texel `a`, lane 1 is texel `b`. Shared by
/// renormalize_rows_sse2 and the renormalizing downsample. Bit-identical to the scalar
/// path: each step is the same correctly rounded IEEE double operation in the same
/// order (divpd and sqrtpd round like divsd and sqrtsd), 8-bit values come from the
/// same kUnitFrom8 table, and fp contract is off, so no FMA forms.
template <u32 Bits> KILN_FORCEINLINE void renormalize_px2_sse2(u32* a, u32* b) noexcept {
    constexpr u32 maxv    = kMaxValue<Bits>;
    __m128d const half    = _mm_set1_pd(double(maxv) * 0.5);
    __m128d const one     = _mm_set1_pd(1.0);
    __m128d const zero    = _mm_setzero_pd();
    __m128d const roundUp = _mm_set1_pd(0.5);
    __m128d v[3];
    for (u32 c = 0; c < 3; ++c) {
        if constexpr (Bits == 8) {
            v[c] = _mm_setr_pd(kUnitFrom8.v[a[c]], kUnitFrom8.v[b[c]]);
        } else {
            __m128i const i = _mm_setr_epi32(int(a[c]), int(b[c]), 0, 0); // values <= 65535
            v[c]            = _mm_sub_pd(_mm_div_pd(_mm_cvtepi32_pd(i), half), one);
        }
    }
    __m128d const len2 =
        _mm_add_pd(_mm_add_pd(_mm_mul_pd(v[0], v[0]), _mm_mul_pd(v[1], v[1])), _mm_mul_pd(v[2], v[2]));
    // Per lane: a zero vector becomes the flat normal (0, 0, 1). Its divisor is replaced
    // by 1.0 so no lane computes 0/0; the blend then discards that lane's quotient.
    // Integer input never gives a zero vector (half is not an integer), but the scalar
    // path handles one, so this one does too.
    __m128d const nonzero = _mm_cmpgt_pd(len2, zero);
    __m128d const len     = _mm_or_pd(_mm_and_pd(nonzero, _mm_sqrt_pd(len2)), _mm_andnot_pd(nonzero, one));
    __m128d const flat[3] = {zero, zero, one};
    for (u32 c = 0; c < 3; ++c) {
        __m128d const q = _mm_div_pd(v[c], len);
        __m128d const n = _mm_or_pd(_mm_and_pd(nonzero, q), _mm_andnot_pd(nonzero, flat[c]));
        __m128d const e = _mm_add_pd(_mm_mul_pd(_mm_add_pd(n, one), half), roundUp);
        // SSE2 has no floor. |n| <= 1 (a correctly rounded x / len with |x| <= len), so
        // e lies in [0.5, maxv + 0.5]: never negative and far below 2^31, where
        // truncation equals floor. The scalar clamp to [0, maxv] then reduces to an
        // integer min.
        __m128i const t = _mm_cvttpd_epi32(e);
        u32 const ea    = u32(_mm_cvtsi128_si32(t));
        u32 const eb    = u32(_mm_cvtsi128_si32(_mm_srli_si128(t, 4)));
        a[c]            = ea < maxv ? ea : maxv;
        b[c]            = eb < maxv ? eb : maxv;
    }
}

/// renormalize_rows with two texels per step; an odd last texel takes the scalar path.
template <u32 Bits, u32 Channels>
KILN_HOT void renormalize_rows_sse2(RowCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    static_assert(Channels >= 3);
    constexpr u32 bpc = Bits / 8, bpp = Channels * bpc;
    u32 const width = ctx.width;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8* KILN_RESTRICT p = ctx.dst + usize(y) * ctx.dstRowBytes;
        u32 x               = 0;
        for (; x + 2 <= width; x += 2, p += 2 * bpp) {
            u8* const q = p + bpp;
            u32 a[3]    = {load<Bits>(p), load<Bits>(p + bpc), load<Bits>(p + 2 * bpc)};
            u32 b[3]    = {load<Bits>(q), load<Bits>(q + bpc), load<Bits>(q + 2 * bpc)};
            renormalize_px2_sse2<Bits>(a, b);
            for (u32 c = 0; c < 3; ++c) {
                store<Bits>(p + c * bpc, a[c]);
                store<Bits>(q + c * bpc, b[c]);
            }
        }
        if (x < width) {
            u32 rgb[3] = {load<Bits>(p), load<Bits>(p + bpc), load<Bits>(p + 2 * bpc)};
            renormalize_px<Bits>(rgb);
            for (u32 c = 0; c < 3; ++c)
                store<Bits>(p + c * bpc, rgb[c]);
        }
    }
}
#endif

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

/// One output texel's 2x2 average; `r0` and `r1` point at the first column sample in
/// the two source rows. Renorm averages like Linear; the caller then renormalizes.
template <u32 Bits, u32 Channels, DownsampleMode Mode>
KILN_FORCEINLINE void box_px(u8 const* r0, u8 const* r1, usize colStep, u32* out) noexcept {
    constexpr u32 bpc    = Bits / 8;
    constexpr u32 srgbCh = Mode == DownsampleMode::Srgb ? (Channels < 3 ? Channels : 3) : 0;
    for (u32 c = 0; c < srgbCh; ++c) {
        u32 const sum = 2 + kSrgbToLinear16[r0[c]] + kSrgbToLinear16[r0[colStep + c]] +
                        kSrgbToLinear16[r1[c]] + kSrgbToLinear16[r1[colStep + c]];
        out[c] = kLinear16ToSrgb8.v[sum / 4];
    }
    for (u32 c = srgbCh; c < Channels; ++c) {
        u32 const off = c * bpc;
        u32 const sum = 2 + load<Bits>(r0 + off) + load<Bits>(r0 + colStep + off) + load<Bits>(r1 + off) +
                        load<Bits>(r1 + colStep + off);
        out[c] = sum / 4; // the +2 rounds to nearest
    }
}

/// On x64 the Renorm mode averages two output texels, then renormalizes both with
/// renormalize_px2_sse2; an odd last texel, and every texel of the other modes, takes
/// the one-texel loop.
template <u32 Bits, u32 Channels, DownsampleMode Mode>
KILN_HOT void downsample_rows(DownsampleCtx const& ctx, u32 rowBegin, u32 rowEnd) noexcept {
    static_assert(Mode != DownsampleMode::Srgb || Bits == 8, "sRGB averaging is 8-bit only");
    static_assert(Mode != DownsampleMode::Renorm || Channels >= 3, "renormalize needs RGB");
    constexpr u32 bpc = Bits / 8, bpp = Channels * bpc;
    u32 const width     = ctx.dstWidth;
    usize const colStep = ctx.colStep;
    for (u32 y = rowBegin; y < rowEnd; ++y) {
        u8 const* KILN_RESTRICT r0 = ctx.src + usize(2 * y) * ctx.srcRowBytes;
        u8 const* KILN_RESTRICT r1 = r0 + ctx.rowStep;
        u8* KILN_RESTRICT d        = ctx.dst + usize(y) * ctx.dstRowBytes;
        u32 x                      = 0;
#if defined(KILN_ARCH_X64)
        if constexpr (Mode == DownsampleMode::Renorm) {
            for (; x + 2 <= width; x += 2, r0 += 4 * bpp, r1 += 4 * bpp, d += 2 * bpp) {
                u32 a[Channels] = {}, b[Channels] = {};
                box_px<Bits, Channels, Mode>(r0, r1, colStep, a);
                box_px<Bits, Channels, Mode>(r0 + 2 * bpp, r1 + 2 * bpp, colStep, b);
                renormalize_px2_sse2<Bits>(a, b);
                for (u32 c = 0; c < Channels; ++c) {
                    store<Bits>(d + c * bpc, a[c]);
                    store<Bits>(d + bpp + c * bpc, b[c]);
                }
            }
        }
#endif
        for (; x < width; ++x, r0 += 2 * bpp, r1 += 2 * bpp, d += bpp) {
            u32 out[Channels] = {};
            box_px<Bits, Channels, Mode>(r0, r1, colStep, out);
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

template <u32 B> RowFn renormalize_scalar_for(u32 channels) noexcept {
    switch (channels) {
    case 3: return &renormalize_rows<B, 3>;
    case 4: return &renormalize_rows<B, 4>;
    default: return nullptr;
    }
}

template <u32 B> RowFn renormalize_for(u32 channels) noexcept {
#if defined(KILN_ARCH_X64)
    switch (channels) {
    case 3: return &renormalize_rows_sse2<B, 3>;
    case 4: return &renormalize_rows_sse2<B, 4>;
    default: return nullptr;
    }
#else
    return renormalize_scalar_for<B>(channels);
#endif
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
/// unsupported. The SSE2 path on x64, the scalar template elsewhere.
inline RowFn renormalize_kernel(u32 bits, u32 channels) noexcept {
    return bits == 8 ? detail::renormalize_for<8>(channels)
                     : (bits == 16 ? detail::renormalize_for<16>(channels) : nullptr);
}

/// Always the scalar reference template: tests compare it with renormalize_kernel.
inline RowFn renormalize_kernel_scalar(u32 bits, u32 channels) noexcept {
    return bits == 8 ? detail::renormalize_scalar_for<8>(channels)
                     : (bits == 16 ? detail::renormalize_scalar_for<16>(channels) : nullptr);
}

/// `mode` falls back to Linear where it does not apply (Srgb on 16-bit, Renorm on
/// fewer than 3 channels). Null for an unsupported format.
inline DownsampleFn downsample_kernel(u32 bits, u32 channels, DownsampleMode mode) noexcept {
    return bits == 8 ? detail::downsample_for<8>(channels, mode)
                     : (bits == 16 ? detail::downsample_for<16>(channels, mode) : nullptr);
}

} // namespace kiln::cook::kernels
