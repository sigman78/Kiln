// kiln/formats.h — engine-neutral pixel/vertex format enum and its constexpr property table.
// Values equal VkFormat; other backends map through format_info(). See docs/design/adapter.md.
#pragma once

#include "kiln/core.h"

namespace kiln {

/// Formats kiln knows about. The v0.5 cooker emits only the uncompressed ones;
/// block-compressed entries exist so KTX2 pass-through and tools can describe them.
enum class Format : u32 {
    Undefined = 0,

    R8_UNORM       = 9,
    R8_SNORM       = 10,
    R8G8_UNORM     = 16,
    R8G8_SNORM     = 17,
    R8G8B8_UNORM   = 23,
    R8G8B8_SRGB    = 29,
    R8G8B8A8_UNORM = 37,
    R8G8B8A8_SNORM = 38,
    R8G8B8A8_SRGB  = 43,

    R16_UNORM           = 70,
    R16_SNORM           = 71,
    R16_SFLOAT          = 76,
    R16G16_UNORM        = 77,
    R16G16_SNORM        = 78,
    R16G16_SFLOAT       = 83,
    R16G16B16A16_UNORM  = 91,
    R16G16B16A16_SNORM  = 92,
    R16G16B16A16_SFLOAT = 97,

    R32_SFLOAT          = 100,
    R32G32_SFLOAT       = 103,
    R32G32B32_SFLOAT    = 106,
    R32G32B32A32_SFLOAT = 109,

    // Block compressed (BC)
    BC1_RGB_UNORM  = 131,
    BC1_RGB_SRGB   = 132,
    BC1_RGBA_UNORM = 133,
    BC1_RGBA_SRGB  = 134,
    BC2_UNORM      = 135,
    BC2_SRGB       = 136,
    BC3_UNORM      = 137,
    BC3_SRGB       = 138,
    BC4_UNORM      = 139,
    BC4_SNORM      = 140,
    BC5_UNORM      = 141,
    BC5_SNORM      = 142,
    BC6H_UFLOAT    = 143,
    BC6H_SFLOAT    = 144,
    BC7_UNORM      = 145,
    BC7_SRGB       = 146,

    // ETC2 / EAC (mobile)
    ETC2_R8G8B8_UNORM   = 147,
    ETC2_R8G8B8_SRGB    = 148,
    ETC2_R8G8B8A1_UNORM = 149,
    ETC2_R8G8B8A1_SRGB  = 150,
    ETC2_R8G8B8A8_UNORM = 151,
    ETC2_R8G8B8A8_SRGB  = 152,
    EAC_R11_UNORM       = 153,
    EAC_R11_SNORM       = 154,
    EAC_R11G11_UNORM    = 155,
    EAC_R11G11_SNORM    = 156,

    // ASTC LDR (mobile)
    ASTC_4x4_UNORM   = 157,
    ASTC_4x4_SRGB    = 158,
    ASTC_5x4_UNORM   = 159,
    ASTC_5x4_SRGB    = 160,
    ASTC_5x5_UNORM   = 161,
    ASTC_5x5_SRGB    = 162,
    ASTC_6x5_UNORM   = 163,
    ASTC_6x5_SRGB    = 164,
    ASTC_6x6_UNORM   = 165,
    ASTC_6x6_SRGB    = 166,
    ASTC_8x5_UNORM   = 167,
    ASTC_8x5_SRGB    = 168,
    ASTC_8x6_UNORM   = 169,
    ASTC_8x6_SRGB    = 170,
    ASTC_8x8_UNORM   = 171,
    ASTC_8x8_SRGB    = 172,
    ASTC_10x5_UNORM  = 173,
    ASTC_10x5_SRGB   = 174,
    ASTC_10x6_UNORM  = 175,
    ASTC_10x6_SRGB   = 176,
    ASTC_10x8_UNORM  = 177,
    ASTC_10x8_SRGB   = 178,
    ASTC_10x10_UNORM = 179,
    ASTC_10x10_SRGB  = 180,
    ASTC_12x10_UNORM = 181,
    ASTC_12x10_SRGB  = 182,
    ASTC_12x12_UNORM = 183,
    ASTC_12x12_SRGB  = 184,
};

/// Numeric representation of the channel data.
enum class FormatKind : u8 {
    UNorm = 0,
    SNorm,
    UFloat, ///< e.g. BC6H_UFLOAT
    SFloat,
};

/// What a format is used for; drives adapter capability queries.
enum class FormatUsage : u8 {
    VertexBuffer = 0,
    SampledImage,
};

/// A set of block-compressed formats, one bit per format from BC1_RGB_UNORM (131) to
/// ASTC_12x12_SRGB (184): bit (value - 131). The bit of any other format is 0.
[[nodiscard]] constexpr u64 block_format_bit(Format f) noexcept {
    u32 const v = u32(f);
    return v >= u32(Format::BC1_RGB_UNORM) && v <= u32(Format::ASTC_12x12_SRGB)
               ? u64(1) << (v - u32(Format::BC1_RGB_UNORM))
               : 0;
}

struct FormatInfo {
    Format format;
    char const* name;  ///< e.g. "R8G8B8A8_SRGB"
    u8 bytesPerBlock;  ///< texel block size in bytes (== texel size when uncompressed)
    u8 blockWidth;     ///< 1 for uncompressed
    u8 blockHeight;    ///< 1 for uncompressed
    u8 channels;       ///< 1..4 (for BC formats: channels the format encodes)
    u8 bitsPerChannel; ///< 0 for compressed formats
    FormatKind kind;
    bool srgb; ///< sRGB transfer function on the color channels
    bool compressed;
    Format srgbPair; ///< the sRGB twin of a UNORM format or the UNORM twin of an sRGB one; Undefined if none
};

namespace detail {
// clang-format off
inline constexpr FormatInfo kFormatTable[] = {
    // format                      name                     bpb bw bh ch bits kind                 srgb  comp  pair
    {Format::R8_UNORM,             "R8_UNORM",               1, 1, 1, 1,  8, FormatKind::UNorm,  false, false, Format::Undefined},
    {Format::R8_SNORM,             "R8_SNORM",               1, 1, 1, 1,  8, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R8G8_UNORM,           "R8G8_UNORM",             2, 1, 1, 2,  8, FormatKind::UNorm,  false, false, Format::Undefined},
    {Format::R8G8_SNORM,           "R8G8_SNORM",             2, 1, 1, 2,  8, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R8G8B8_UNORM,         "R8G8B8_UNORM",           3, 1, 1, 3,  8, FormatKind::UNorm,  false, false, Format::R8G8B8_SRGB},
    {Format::R8G8B8_SRGB,          "R8G8B8_SRGB",            3, 1, 1, 3,  8, FormatKind::UNorm,  true,  false, Format::R8G8B8_UNORM},
    {Format::R8G8B8A8_UNORM,       "R8G8B8A8_UNORM",         4, 1, 1, 4,  8, FormatKind::UNorm,  false, false, Format::R8G8B8A8_SRGB},
    {Format::R8G8B8A8_SNORM,       "R8G8B8A8_SNORM",         4, 1, 1, 4,  8, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R8G8B8A8_SRGB,        "R8G8B8A8_SRGB",          4, 1, 1, 4,  8, FormatKind::UNorm,  true,  false, Format::R8G8B8A8_UNORM},
    {Format::R16_UNORM,            "R16_UNORM",              2, 1, 1, 1, 16, FormatKind::UNorm,  false, false, Format::Undefined},
    {Format::R16_SNORM,            "R16_SNORM",              2, 1, 1, 1, 16, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R16_SFLOAT,           "R16_SFLOAT",             2, 1, 1, 1, 16, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R16G16_UNORM,         "R16G16_UNORM",           4, 1, 1, 2, 16, FormatKind::UNorm,  false, false, Format::Undefined},
    {Format::R16G16_SNORM,         "R16G16_SNORM",           4, 1, 1, 2, 16, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R16G16_SFLOAT,        "R16G16_SFLOAT",          4, 1, 1, 2, 16, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R16G16B16A16_UNORM,   "R16G16B16A16_UNORM",     8, 1, 1, 4, 16, FormatKind::UNorm,  false, false, Format::Undefined},
    {Format::R16G16B16A16_SNORM,   "R16G16B16A16_SNORM",     8, 1, 1, 4, 16, FormatKind::SNorm,  false, false, Format::Undefined},
    {Format::R16G16B16A16_SFLOAT,  "R16G16B16A16_SFLOAT",    8, 1, 1, 4, 16, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R32_SFLOAT,           "R32_SFLOAT",             4, 1, 1, 1, 32, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R32G32_SFLOAT,        "R32G32_SFLOAT",          8, 1, 1, 2, 32, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R32G32B32_SFLOAT,     "R32G32B32_SFLOAT",      12, 1, 1, 3, 32, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::R32G32B32A32_SFLOAT,  "R32G32B32A32_SFLOAT",   16, 1, 1, 4, 32, FormatKind::SFloat, false, false, Format::Undefined},
    {Format::BC1_RGB_UNORM,        "BC1_RGB_UNORM",          8, 4, 4, 3,  0, FormatKind::UNorm,  false, true,  Format::BC1_RGB_SRGB},
    {Format::BC1_RGB_SRGB,         "BC1_RGB_SRGB",           8, 4, 4, 3,  0, FormatKind::UNorm,  true,  true,  Format::BC1_RGB_UNORM},
    {Format::BC1_RGBA_UNORM,       "BC1_RGBA_UNORM",         8, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::BC1_RGBA_SRGB},
    {Format::BC1_RGBA_SRGB,        "BC1_RGBA_SRGB",          8, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::BC1_RGBA_UNORM},
    {Format::BC2_UNORM,            "BC2_UNORM",             16, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::BC2_SRGB},
    {Format::BC2_SRGB,             "BC2_SRGB",              16, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::BC2_UNORM},
    {Format::BC3_UNORM,            "BC3_UNORM",             16, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::BC3_SRGB},
    {Format::BC3_SRGB,             "BC3_SRGB",              16, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::BC3_UNORM},
    {Format::BC4_UNORM,            "BC4_UNORM",              8, 4, 4, 1,  0, FormatKind::UNorm,  false, true,  Format::Undefined},
    {Format::BC4_SNORM,            "BC4_SNORM",              8, 4, 4, 1,  0, FormatKind::SNorm,  false, true,  Format::Undefined},
    {Format::BC5_UNORM,            "BC5_UNORM",             16, 4, 4, 2,  0, FormatKind::UNorm,  false, true,  Format::Undefined},
    {Format::BC5_SNORM,            "BC5_SNORM",             16, 4, 4, 2,  0, FormatKind::SNorm,  false, true,  Format::Undefined},
    {Format::BC6H_UFLOAT,          "BC6H_UFLOAT",           16, 4, 4, 3,  0, FormatKind::UFloat, false, true,  Format::Undefined},
    {Format::BC6H_SFLOAT,          "BC6H_SFLOAT",           16, 4, 4, 3,  0, FormatKind::SFloat, false, true,  Format::Undefined},
    {Format::BC7_UNORM,            "BC7_UNORM",             16, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::BC7_SRGB},
    {Format::BC7_SRGB,             "BC7_SRGB",              16, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::BC7_UNORM},
    {Format::ETC2_R8G8B8_UNORM,       "ETC2_R8G8B8_UNORM",      8, 4, 4, 3,  0, FormatKind::UNorm,  false, true,  Format::ETC2_R8G8B8_SRGB},
    {Format::ETC2_R8G8B8_SRGB,        "ETC2_R8G8B8_SRGB",       8, 4, 4, 3,  0, FormatKind::UNorm,  true,  true,  Format::ETC2_R8G8B8_UNORM},
    {Format::ETC2_R8G8B8A1_UNORM,     "ETC2_R8G8B8A1_UNORM",    8, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::ETC2_R8G8B8A1_SRGB},
    {Format::ETC2_R8G8B8A1_SRGB,      "ETC2_R8G8B8A1_SRGB",     8, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::ETC2_R8G8B8A1_UNORM},
    {Format::ETC2_R8G8B8A8_UNORM,     "ETC2_R8G8B8A8_UNORM",   16, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::ETC2_R8G8B8A8_SRGB},
    {Format::ETC2_R8G8B8A8_SRGB,      "ETC2_R8G8B8A8_SRGB",    16, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::ETC2_R8G8B8A8_UNORM},
    {Format::EAC_R11_UNORM,           "EAC_R11_UNORM",          8, 4, 4, 1,  0, FormatKind::UNorm,  false, true,  Format::Undefined},
    {Format::EAC_R11_SNORM,           "EAC_R11_SNORM",          8, 4, 4, 1,  0, FormatKind::SNorm,  false, true,  Format::Undefined},
    {Format::EAC_R11G11_UNORM,        "EAC_R11G11_UNORM",      16, 4, 4, 2,  0, FormatKind::UNorm,  false, true,  Format::Undefined},
    {Format::EAC_R11G11_SNORM,        "EAC_R11G11_SNORM",      16, 4, 4, 2,  0, FormatKind::SNorm,  false, true,  Format::Undefined},
    {Format::ASTC_4x4_UNORM,          "ASTC_4x4_UNORM",        16, 4, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_4x4_SRGB},
    {Format::ASTC_4x4_SRGB,           "ASTC_4x4_SRGB",         16, 4, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_4x4_UNORM},
    {Format::ASTC_5x4_UNORM,          "ASTC_5x4_UNORM",        16, 5, 4, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_5x4_SRGB},
    {Format::ASTC_5x4_SRGB,           "ASTC_5x4_SRGB",         16, 5, 4, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_5x4_UNORM},
    {Format::ASTC_5x5_UNORM,          "ASTC_5x5_UNORM",        16, 5, 5, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_5x5_SRGB},
    {Format::ASTC_5x5_SRGB,           "ASTC_5x5_SRGB",         16, 5, 5, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_5x5_UNORM},
    {Format::ASTC_6x5_UNORM,          "ASTC_6x5_UNORM",        16, 6, 5, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_6x5_SRGB},
    {Format::ASTC_6x5_SRGB,           "ASTC_6x5_SRGB",         16, 6, 5, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_6x5_UNORM},
    {Format::ASTC_6x6_UNORM,          "ASTC_6x6_UNORM",        16, 6, 6, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_6x6_SRGB},
    {Format::ASTC_6x6_SRGB,           "ASTC_6x6_SRGB",         16, 6, 6, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_6x6_UNORM},
    {Format::ASTC_8x5_UNORM,          "ASTC_8x5_UNORM",        16, 8, 5, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_8x5_SRGB},
    {Format::ASTC_8x5_SRGB,           "ASTC_8x5_SRGB",         16, 8, 5, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_8x5_UNORM},
    {Format::ASTC_8x6_UNORM,          "ASTC_8x6_UNORM",        16, 8, 6, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_8x6_SRGB},
    {Format::ASTC_8x6_SRGB,           "ASTC_8x6_SRGB",         16, 8, 6, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_8x6_UNORM},
    {Format::ASTC_8x8_UNORM,          "ASTC_8x8_UNORM",        16, 8, 8, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_8x8_SRGB},
    {Format::ASTC_8x8_SRGB,           "ASTC_8x8_SRGB",         16, 8, 8, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_8x8_UNORM},
    {Format::ASTC_10x5_UNORM,         "ASTC_10x5_UNORM",       16, 10, 5, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_10x5_SRGB},
    {Format::ASTC_10x5_SRGB,          "ASTC_10x5_SRGB",        16, 10, 5, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_10x5_UNORM},
    {Format::ASTC_10x6_UNORM,         "ASTC_10x6_UNORM",       16, 10, 6, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_10x6_SRGB},
    {Format::ASTC_10x6_SRGB,          "ASTC_10x6_SRGB",        16, 10, 6, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_10x6_UNORM},
    {Format::ASTC_10x8_UNORM,         "ASTC_10x8_UNORM",       16, 10, 8, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_10x8_SRGB},
    {Format::ASTC_10x8_SRGB,          "ASTC_10x8_SRGB",        16, 10, 8, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_10x8_UNORM},
    {Format::ASTC_10x10_UNORM,        "ASTC_10x10_UNORM",      16, 10, 10, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_10x10_SRGB},
    {Format::ASTC_10x10_SRGB,         "ASTC_10x10_SRGB",       16, 10, 10, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_10x10_UNORM},
    {Format::ASTC_12x10_UNORM,        "ASTC_12x10_UNORM",      16, 12, 10, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_12x10_SRGB},
    {Format::ASTC_12x10_SRGB,         "ASTC_12x10_SRGB",       16, 12, 10, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_12x10_UNORM},
    {Format::ASTC_12x12_UNORM,        "ASTC_12x12_UNORM",      16, 12, 12, 4,  0, FormatKind::UNorm,  false, true,  Format::ASTC_12x12_SRGB},
    {Format::ASTC_12x12_SRGB,         "ASTC_12x12_SRGB",       16, 12, 12, 4,  0, FormatKind::UNorm,  true,  true,  Format::ASTC_12x12_UNORM},
};
// clang-format on
} // namespace detail

inline constexpr usize kFormatCount = countof(detail::kFormatTable);

/// Property row for a format, or nullptr if kiln does not know it.
[[nodiscard]] constexpr FormatInfo const* format_info(Format f) noexcept {
    for (FormatInfo const& e : detail::kFormatTable)
        if (e.format == f) return &e;
    return nullptr;
}
[[nodiscard]] constexpr bool is_known_format(Format f) noexcept { return format_info(f) != nullptr; }
[[nodiscard]] constexpr char const* format_name(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return i ? i->name : "UNDEFINED";
}
[[nodiscard]] constexpr u32 format_block_bytes(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return i ? i->bytesPerBlock : 0;
}
[[nodiscard]] constexpr bool is_srgb_format(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return i && i->srgb;
}
[[nodiscard]] constexpr bool is_compressed_format(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return i && i->compressed;
}
/// Linear twin of an sRGB format (or the format itself when already linear / no twin).
[[nodiscard]] constexpr Format linear_format(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return (i && i->srgb && i->srgbPair != Format::Undefined) ? i->srgbPair : f;
}
/// sRGB twin of a linear format (or the format itself when already sRGB / no twin).
[[nodiscard]] constexpr Format srgb_format(Format f) noexcept {
    FormatInfo const* i = format_info(f);
    return (i && !i->srgb && i->srgbPair != Format::Undefined) ? i->srgbPair : f;
}

/// Bytes of one tightly packed row of `width` texels (whole blocks for compressed formats).
[[nodiscard]] constexpr u64 format_row_bytes(Format f, u32 width) noexcept {
    FormatInfo const* i = format_info(f);
    if (!i) return 0;
    u64 blocks = (u64(width) + i->blockWidth - 1) / i->blockWidth;
    return blocks * i->bytesPerBlock;
}
/// Bytes of one tightly packed 2D image (whole blocks for compressed formats).
[[nodiscard]] constexpr u64 format_image_bytes(Format f, u32 width, u32 height) noexcept {
    FormatInfo const* i = format_info(f);
    if (!i) return 0;
    u64 rows = (u64(height) + i->blockHeight - 1) / i->blockHeight;
    return format_row_bytes(f, width) * rows;
}

// Table sanity: every row's enum value is unique and pairs point back at each other.
namespace detail {
constexpr bool format_table_ok() noexcept {
    for (usize a = 0; a < kFormatCount; ++a) {
        for (usize b = a + 1; b < kFormatCount; ++b)
            if (kFormatTable[a].format == kFormatTable[b].format) return false;
        FormatInfo const& e = kFormatTable[a];
        if (e.srgbPair != Format::Undefined) {
            FormatInfo const* p = format_info(e.srgbPair);
            if (!p || p->srgbPair != e.format || p->srgb == e.srgb) return false;
        }
        if (!e.compressed && (e.blockWidth != 1 || e.blockHeight != 1 || e.bitsPerChannel == 0)) return false;
        if (!e.compressed && e.channels * e.bitsPerChannel != e.bytesPerBlock * 8) return false;
    }
    return true;
}
static_assert(format_table_ok());
} // namespace detail

} // namespace kiln
