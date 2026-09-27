// src/cook/image.cpp — image pipeline. Integer-exact except normal renormalization,
// which uses IEEE double and std::sqrt (correctly rounded, so identical everywhere).
#include "kiln/cook/image.h"

#include <cmath>

// Keep a*b+c from being fused into an FMA: fused and unfused results can differ in
// the last bit, which would break cross-platform byte-identical output. gcc in
// ISO mode (-std=c++NN) already defaults to -ffp-contract=off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace kiln::cook {

namespace {

/// Generated offline in double with the exact sRGB EOTF (IEC 61966-2-1):
///   table[v] = round(65535 * f(v / 255)),  f(c) = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055)^2.4
/// No entry is within 1e-6 of a .5 tie, so the rounding mode does not matter.
/// Strictly increasing, so linear16_to_srgb8 inverts it exactly.
constexpr u16 kSrgbToLinear16[256] = {
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

bool valid_image(Image const& img) noexcept {
    return img.width > 0 && img.height > 0 && img.channels >= 1 && img.channels <= 4 &&
           (img.bitsPerChannel == 8 || img.bitsPerChannel == 16) && img.pixels.size() == img.byte_size();
}

u32 load(u8 const* p, u32 bits) noexcept {
    if (bits == 8) return *p;
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

void store(u8* p, u32 bits, u32 v) noexcept {
    if (bits == 8) {
        *p = u8(v);
        return;
    }
    u16 const s = u16(v);
    std::memcpy(p, &s, 2);
}

/// 16 <-> 8 bit conversion rules from image.h.
u32 convert_depth(u32 v, u32 fromBits, u32 toBits) noexcept {
    if (fromBits == 16 && toBits == 8) return (v * 255u + 32767u) / 65535u;
    if (fromBits == 8 && toBits == 16) return v * 257u;
    return v;
}

Image make_image(u32 w, u32 h, u32 channels, u32 bits, Allocator const* alloc) noexcept {
    Image img;
    img.width          = w;
    img.height         = h;
    img.channels       = channels;
    img.bitsPerChannel = bits;
    img.pixels.init(alloc ? alloc : default_allocator(), Tag::Cook);
    img.pixels.resize(usize(img.byte_size()));
    return img;
}

Image copy_image(Image const& src, Allocator const* alloc) noexcept {
    Image img = make_image(src.width, src.height, src.channels, src.bitsPerChannel, alloc);
    if (!src.pixels.empty()) std::memcpy(img.pixels.data(), src.pixels.data(), src.pixels.size());
    return img;
}

/// Renormalize one RGB triple (values in 0..maxv) as a unit vector.
void renormalize_px(u32 rgb[3], u32 maxv) noexcept {
    double const half = double(maxv) * 0.5; // 127.5 or 32767.5
    double const x    = double(rgb[0]) / half - 1.0;
    double const y    = double(rgb[1]) / half - 1.0;
    double const z    = double(rgb[2]) / half - 1.0;
    double const len2 = x * x + y * y + z * z;
    double n[3]       = {0.0, 0.0, 1.0}; // a zero vector becomes the flat normal
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

} // namespace

u16 srgb8_to_linear16(u8 v) noexcept { return kSrgbToLinear16[v]; }

u8 linear16_to_srgb8(u16 v) noexcept {
    // Smallest index whose entry is >= v, then pick the nearer of it and its
    // predecessor (ties go to the lower index).
    u32 lo = 0, hi = 255;
    while (lo < hi) {
        u32 const mid = (lo + hi) / 2;
        if (kSrgbToLinear16[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0 && u32(v) - kSrgbToLinear16[lo - 1] <= u32(kSrgbToLinear16[lo]) - v) --lo;
    return u8(lo);
}

Result<Image> convert_image(Image const& src, u32 channels, u32 bitsPerChannel,
                            Allocator const* alloc) noexcept {
    if (!valid_image(src) || channels < 1 || channels > 4 || (bitsPerChannel != 8 && bitsPerChannel != 16))
        return make_status(Code::InvalidArgument);
    if (channels == src.channels && bitsPerChannel == src.bitsPerChannel) return copy_image(src, alloc);

    Image dst        = make_image(src.width, src.height, channels, bitsPerChannel, alloc);
    u32 const sbits  = src.bitsPerChannel;
    u32 const sbytes = sbits / 8;
    u32 const dbytes = bitsPerChannel / 8;
    u32 const dmax   = bitsPerChannel == 8 ? 255u : 65535u;
    u32 const sbpp   = src.bytes_per_pixel();
    u32 const dbpp   = dst.bytes_per_pixel();
    u64 const count  = u64(src.width) * src.height;
    u8 const* s      = src.pixels.data();
    u8* d            = dst.pixels.data();
    for (u64 i = 0; i < count; ++i, s += sbpp, d += dbpp) {
        for (u32 c = 0; c < channels; ++c) {
            u32 v;
            if (c < src.channels)
                v = convert_depth(load(s + c * sbytes, sbits), sbits, bitsPerChannel);
            else if (c == 3)
                v = dmax; // added alpha is opaque
            else if (src.channels == 1)
                v = convert_depth(load(s, sbits), sbits, bitsPerChannel); // gray -> G, B
            else
                v = 0; // RG -> RGB(A): B = 0
            store(d + c * dbytes, bitsPerChannel, v);
        }
    }
    return dst;
}

Result<Image> downsample_2x(Image const& src, MipOptions const& opt, Allocator const* alloc) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const w    = max(src.width / 2, 1u);
    u32 const h    = max(src.height / 2, 1u);
    Image dst      = make_image(w, h, src.channels, src.bitsPerChannel, alloc);
    u32 const bits = src.bitsPerChannel;
    u32 const bpc  = bits / 8;
    u32 const bpp  = src.bytes_per_pixel();
    u64 const row  = src.row_bytes();
    u32 const maxv = bits == 8 ? 255u : 65535u;
    // sRGB averaging exists only for 8-bit data (there is no 16-bit sRGB format) and
    // covers the color channels, never alpha (channel 3). Normals are linear.
    bool const renorm       = opt.renormalize && src.channels >= 3;
    bool const srgb         = opt.srgb && bits == 8 && !renorm;
    u32 const colorChannels = min(src.channels, 3u);

    u8 const* sp = src.pixels.data();
    u8* dp       = dst.pixels.data();
    for (u32 y = 0; y < h; ++y) {
        u64 const r0 = u64(min(2 * y, src.height - 1)) * row;
        u64 const r1 = u64(min(2 * y + 1, src.height - 1)) * row;
        for (u32 x = 0; x < w; ++x) {
            u64 const c0   = u64(min(2 * x, src.width - 1)) * bpp;
            u64 const c1   = u64(min(2 * x + 1, src.width - 1)) * bpp;
            u8 const* p[4] = {sp + r0 + c0, sp + r0 + c1, sp + r1 + c0, sp + r1 + c1};
            u32 out[4]     = {};
            for (u32 c = 0; c < src.channels; ++c) {
                u32 const off = c * bpc;
                u32 sum       = 2; // round to nearest
                if (srgb && c < colorChannels) {
                    for (u8 const* q : p)
                        sum += srgb8_to_linear16(q[off]);
                    out[c] = linear16_to_srgb8(u16(sum / 4));
                } else {
                    for (u8 const* q : p)
                        sum += load(q + off, bits);
                    out[c] = sum / 4;
                }
            }
            if (renorm) renormalize_px(out, maxv);
            u8* o = dp + (u64(y) * w + x) * bpp;
            for (u32 c = 0; c < src.channels; ++c)
                store(o + c * bpc, bits, out[c]);
        }
    }
    return dst;
}

void flip_green(Image& img) noexcept {
    if (!valid_image(img) || img.channels < 2) return;
    u32 const bits = img.bitsPerChannel;
    u32 const maxv = bits == 8 ? 255u : 65535u;
    u32 const bpp  = img.bytes_per_pixel();
    u8* g          = img.pixels.data() + bits / 8;
    u64 const n    = u64(img.width) * img.height;
    for (u64 i = 0; i < n; ++i, g += bpp)
        store(g, bits, maxv - load(g, bits));
}

void renormalize(Image& img) noexcept {
    if (!valid_image(img) || img.channels < 3) return;
    u32 const bits = img.bitsPerChannel;
    u32 const bpc  = bits / 8;
    u32 const maxv = bits == 8 ? 255u : 65535u;
    u32 const bpp  = img.bytes_per_pixel();
    u8* p          = img.pixels.data();
    u64 const n    = u64(img.width) * img.height;
    for (u64 i = 0; i < n; ++i, p += bpp) {
        u32 rgb[3] = {load(p, bits), load(p + bpc, bits), load(p + 2 * bpc, bits)};
        renormalize_px(rgb, maxv);
        for (u32 c = 0; c < 3; ++c)
            store(p + c * bpc, bits, rgb[c]);
    }
}

Result<Vec<Image>> build_mip_chain(Image&& src, MipOptions const& opt, u32 maxLevels,
                                   Allocator const* alloc) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const full  = u32(std::bit_width(max(src.width, src.height)));
    u32 const count = maxLevels == 0 ? full : min(maxLevels, full);
    Vec<Image> chain(alloc ? alloc : default_allocator(), Tag::Cook);
    chain.reserve(count);
    chain.push_back(std::move(src));
    for (u32 i = 1; i < count; ++i) {
        Result<Image> next = downsample_2x(chain[i - 1], opt, alloc);
        if (next.failed()) return next.status();
        chain.push_back(std::move(next).value());
    }
    return chain;
}

} // namespace kiln::cook
