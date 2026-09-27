// kiln/cook/image.h — decoded CPU images and the deterministic image pipeline
// used by texture cooking (mips, color space, channel selection). Cook side only.
//
// Determinism: every operation here is integer-exact (sRGB <-> linear through a
// fixed table, box filters in u32, IEEE sqrt for renormalization), so identical
// input produces byte-identical output on every compiler and platform.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln::cook {

/// Uncompressed, tightly packed, top-left origin, row-major.
struct Image {
    u32 width          = 0;
    u32 height         = 0;
    u32 channels       = 0; ///< 1 (R), 2 (RG), 3 (RGB), 4 (RGBA)
    u32 bitsPerChannel = 0; ///< 8 or 16 (16-bit values are native-endian u16)
    Vec<u8> pixels;         ///< width * height * channels * (bits / 8) bytes

    [[nodiscard]] u32 bytes_per_pixel() const noexcept { return channels * (bitsPerChannel / 8); }
    [[nodiscard]] u64 row_bytes() const noexcept { return u64(width) * bytes_per_pixel(); }
    [[nodiscard]] u64 byte_size() const noexcept { return row_bytes() * height; }
};

// ---------------------------------------------------------------------------
// Diagnostics (K2000-K2999: image import / encode). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum ImageDiagCode : u32 {
    kDiagImageDecodeFailed   = 2001, ///< PNG stream malformed or truncated (ParseError)
    kDiagImageUnsupported    = 2002, ///< PNG feature or bit depth kiln does not decode (Unsupported)
    kDiagImageUnknownFormat  = 2003, ///< bytes are neither PNG nor KTX2 (Unsupported)
    kDiagImagePassthroughBad = 2004, ///< KTX2 pass-through rejected (supercompressed, unsupported format,
                                     ///< invalid) (Unsupported/Corrupt)
    kDiagImageDownscaled = 2005,     ///< image larger than maxSize / target cap; top levels dropped (Info)
    kDiagImageNpotMips   = 2006,     ///< non-power-of-two size with mips: floor halving (Info)
    kDiagImageChannelMismatch =
        2007,                  ///< channel count unusual for the usage (e.g. RGBA for Height) (Warning)
    kDiagImageTooLarge = 2008, ///< dimension exceeds 16384 or byte size exceeds 2^32 (Unsupported)
};

/// True if `bytes` start with the PNG signature / KTX2 identifier.
[[nodiscard]] KILN_API bool is_png(Span<u8 const> bytes) noexcept;
[[nodiscard]] KILN_API bool is_ktx2(Span<u8 const> bytes) noexcept;

/// Decode a PNG (8- or 16-bit; gray, gray+alpha, RGB, RGBA, palette expanded to
/// RGB/RGBA) into an Image allocated from `alloc` (Tag::Cook). Interlaced PNGs are
/// accepted. Never throws; malformed input returns ParseError with a K2xxx diagnostic.
KILN_API Result<Image> decode_png(Span<u8 const> bytes, Allocator const* alloc,
                                  DiagSink const* diag = nullptr, StrView asset = {}) noexcept;

// ---------------------------------------------------------------------------
// sRGB transfer, integer-exact
// ---------------------------------------------------------------------------

/// sRGB-encoded 8-bit value -> linear 16-bit (0..65535), from a fixed table.
[[nodiscard]] KILN_API u16 srgb8_to_linear16(u8 v) noexcept;
/// Linear 16-bit -> nearest sRGB-encoded 8-bit value (binary search over the same table).
[[nodiscard]] KILN_API u8 linear16_to_srgb8(u16 v) noexcept;

// ---------------------------------------------------------------------------
// Operations (all deterministic; results allocated from `alloc`, Tag::Cook)
// ---------------------------------------------------------------------------

/// Convert to a different channel count / bit depth. Rules: dropping channels keeps
/// the first N (R, RG, RGB); adding channels fills G/B by replicating R for 1->3/4,
/// fills B with 0 for 2->3/4 (2 channels are RG, not gray+alpha; cook_texture handles
/// gray+alpha PNGs itself), and sets A = max; 16->8 rounds to nearest
/// (v * 255 + 32767) / 65535; 8->16 is
/// v * 257. Returns the input unchanged (a copy) when nothing changes.
KILN_API Result<Image> convert_image(Image const& src, u32 channels, u32 bitsPerChannel,
                                     Allocator const* alloc) noexcept;

/// Next mip level with a 2x2 box filter (floor halving, min 1: an odd trailing column
/// or row is dropped; a dimension of 1 samples the same texel twice). `srgb` selects
/// sRGB-correct averaging for the first three
/// channels (alpha is always linear). `renormalize` treats RGB as a tangent-space
/// normal (0..1 -> -1..1), renormalizes after averaging and writes back rounded.
struct MipOptions {
    bool srgb        = false;
    bool renormalize = false;
};
KILN_API Result<Image> downsample_2x(Image const& src, MipOptions const& opt,
                                     Allocator const* alloc) noexcept;

/// Flip the green channel (v -> max - v) in place; DirectX -> OpenGL normal convention.
KILN_API void flip_green(Image& img) noexcept;

/// Renormalize RGB as unit vectors in place (8- or 16-bit). No-op for < 3 channels.
KILN_API void renormalize(Image& img) noexcept;

/// Full mip chain: level 0 is a copy of `src` (or `src` moved in when `takeSource`),
/// then downsample_2x until 1x1. `maxLevels` 0 = full chain.
KILN_API Result<Vec<Image>> build_mip_chain(Image&& src, MipOptions const& opt, u32 maxLevels,
                                            Allocator const* alloc) noexcept;

} // namespace kiln::cook
