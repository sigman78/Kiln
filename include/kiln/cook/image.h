// kiln/cook/image.h — decoded CPU images and the image pipeline for texture cooking.
// Every operation is deterministic: identical input gives byte-identical output on
// every compiler and platform.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln {
struct JobSystem; // kiln/io.h
} // namespace kiln

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
/// accepted. Malformed input returns ParseError with a K2xxx diagnostic.
KILN_API Result<Image> decode_png(Span<u8 const> bytes, Allocator const* alloc,
                                  DiagSink const* diag = nullptr, StrView asset = {}) noexcept;

// ---------------------------------------------------------------------------
// sRGB transfer, integer-exact
// ---------------------------------------------------------------------------

/// sRGB-encoded 8-bit value -> linear 16-bit (0..65535), from a fixed table.
[[nodiscard]] KILN_API u16 srgb8_to_linear16(u8 v) noexcept;
/// Linear 16-bit -> nearest sRGB-encoded 8-bit value (ties to the lower code), from a
/// 64 KiB table derived from the same 256 entries.
[[nodiscard]] KILN_API u8 linear16_to_srgb8(u16 v) noexcept;

// ---------------------------------------------------------------------------
// Operations (all deterministic; results allocated from `alloc`, Tag::Cook)
//
// `jobs` (optional) splits the work into row bands run on its workers and the calling
// thread; rows are independent, so the output is byte-identical with or without it.
// ---------------------------------------------------------------------------

/// Optional workers for a split pass. `maxThreads` counts the calling thread: 1 runs
/// inline, 0 lifts the cap. The default keeps a cook from crowding out the host's threads.
inline constexpr u32 kDefaultCookThreads = 3;
struct JobBudget {
    JobSystem const* jobs = nullptr;
    u32 maxThreads        = kDefaultCookThreads;
};

/// Convert to another channel count / bit depth. Dropping channels keeps the first N.
/// Adding channels: 1 -> 3/4 replicates R into G/B; 2 -> 3/4 sets B = 0 (2 channels
/// are RG, not gray+alpha); A = max. 16 -> 8 is (v * 255 + 32767) / 65535; 8 -> 16 is
/// v * 257. Returns a copy when nothing changes.
KILN_API Result<Image> convert_image(Image const& src, u32 channels, u32 bitsPerChannel,
                                     Allocator const* alloc, JobBudget const& budget = {}) noexcept;

/// Level 0 preparation for prepare_image. Flip and renormalize apply to the converted
/// image and are no-ops where flip_green / renormalize would be.
struct PrepareOptions {
    bool grayAlpha   = false; ///< a 2-channel source is gray+alpha: to 3/4 channels as (Y, Y, Y, A)
    bool flipGreen   = false; ///< flip_green after converting
    bool renormalize = false; ///< renormalize after converting (and flipping)
};
/// convert_image, then flip_green and renormalize as `opt` asks, in one pass over
/// `src`. Byte-identical to calling them in that order.
KILN_API Result<Image> prepare_image(Image const& src, u32 channels, u32 bitsPerChannel,
                                     PrepareOptions const& opt, Allocator const* alloc,
                                     JobBudget const& budget = {}) noexcept;

/// `srgb`: sRGB-correct averaging of the first three channels (8-bit only; alpha is
/// always linear). `renormalize`: treat RGB as a tangent-space normal (0..1 -> -1..1)
/// and renormalize after averaging; takes precedence over `srgb`.
struct MipOptions {
    bool srgb        = false;
    bool renormalize = false;
};
/// Next mip level with a 2x2 box filter. Floor halving, min 1: an odd trailing column
/// or row is dropped; a dimension of 1 samples the same texel twice.
KILN_API Result<Image> downsample_2x(Image const& src, MipOptions const& opt, Allocator const* alloc,
                                     JobBudget const& budget = {}) noexcept;

/// Flip the green channel (v -> max - v) in place; DirectX -> OpenGL normal convention.
KILN_API void flip_green(Image& img, JobBudget const& budget = {}) noexcept;

/// Renormalize RGB as unit vectors in place (8- or 16-bit). No-op for < 3 channels.
KILN_API void renormalize(Image& img, JobBudget const& budget = {}) noexcept;

/// Full mip chain: level 0 is `src` (moved in), then downsample_2x until 1x1.
/// `maxLevels` 0 = full chain. `budget` splits only the level 0 to 1 downsample; the
/// smaller levels run on the calling thread.
KILN_API Result<Vec<Image>> build_mip_chain(Image&& src, MipOptions const& opt, u32 maxLevels,
                                            Allocator const* alloc, JobBudget const& budget = {}) noexcept;

} // namespace kiln::cook
