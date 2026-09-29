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
    u32 bitsPerChannel = 0; ///< 8, 16 (native-endian u16) or 32 (f32: HDR, linear, not clamped)
    Vec<u8> pixels;         ///< width * height * channels * (bits / 8) bytes

    [[nodiscard]] u32 bytes_per_pixel() const noexcept { return channels * (bitsPerChannel / 8); }
    [[nodiscard]] u64 row_bytes() const noexcept { return u64(width) * bytes_per_pixel(); }
    [[nodiscard]] u64 byte_size() const noexcept { return row_bytes() * height; }
};

// ---------------------------------------------------------------------------
// Diagnostics (K2000-K2999: image import / encode). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum ImageDiagCode : u32 {
    kDiagImageDecodeFailed = 2001, ///< image stream malformed or truncated (ParseError)
    kDiagImageUnsupported  = 2002, ///< image feature kiln does not decode, or WebP not built in (Unsupported)
    kDiagImageUnknownFormat  = 2003, ///< bytes are not PNG, JPEG, WebP or KTX2 (Unsupported)
    kDiagImagePassthroughBad = 2004, ///< KTX2 pass-through rejected (supercompressed, unsupported format,
                                     ///< invalid) (Unsupported/Corrupt)
    kDiagImageDownscaled = 2005,     ///< image larger than maxSize / target cap; top levels dropped (Info)
    kDiagImageNpotMips   = 2006,     ///< non-power-of-two size with mips: floor halving (Info)
    kDiagImageChannelMismatch =
        2007,                  ///< channel count unusual for the usage (e.g. RGBA for Height) (Warning)
    kDiagImageTooLarge = 2008, ///< dimension exceeds 16384 or byte size exceeds 2^32 (Unsupported)
    kDiagImageLossySource =
        2009, ///< lossy source (JPEG, lossy WebP) for a Normal or Height texture (Warning)
    kDiagImageSliceLayout = 2010, ///< a cube or array strip does not divide into its slices (InvalidArgument)
    kDiagImageNoHdrRange  = 2011, ///< usage Hdr with an 8/16-bit source: values stay in 0..1 (Warning)
};

/// True if `bytes` start with the signature of that format.
[[nodiscard]] KILN_API bool is_png(Span<u8 const> bytes) noexcept;
[[nodiscard]] KILN_API bool is_jpeg(Span<u8 const> bytes) noexcept;
[[nodiscard]] KILN_API bool is_webp(Span<u8 const> bytes) noexcept;
[[nodiscard]] KILN_API bool is_ktx2(Span<u8 const> bytes) noexcept;
/// Radiance .hdr: the header starts with `#?RADIANCE` or `#?RGBE`.
[[nodiscard]] KILN_API bool is_hdr(Span<u8 const> bytes) noexcept;
/// True for JPEG and lossy WebP.
[[nodiscard]] KILN_API bool is_lossy_image(Span<u8 const> bytes) noexcept;
/// True if this build decodes WebP (CMake option KILN_WEBP, off by default).
[[nodiscard]] KILN_API bool webp_decode_enabled() noexcept;

// Decoders return an Image allocated from `alloc` (Tag::Cook). Malformed input returns
// ParseError, an unsupported feature returns Unsupported; both emit a K2xxx diagnostic.

/// PNG: 8- or 16-bit; gray, gray+alpha, RGB, RGBA, palette expanded to RGB/RGBA. Interlaced
/// PNGs are accepted.
KILN_API Result<Image> decode_png(Span<u8 const> bytes, Allocator const* alloc,
                                  DiagSink const* diag = nullptr, StrView asset = {}) noexcept;
/// JPEG: baseline and progressive, to 1 channel (gray) or 3 (RGB), 8-bit. Arithmetic
/// coding, 12/16-bit precision, lossless and hierarchical JPEG return Unsupported.
KILN_API Result<Image> decode_jpeg(Span<u8 const> bytes, Allocator const* alloc,
                                   DiagSink const* diag = nullptr, StrView asset = {}) noexcept;
/// WebP: lossy and lossless, first frame only, to 3 channels (opaque) or 4, 8-bit.
/// Returns Unsupported when webp_decode_enabled() is false.
KILN_API Result<Image> decode_webp(Span<u8 const> bytes, Allocator const* alloc,
                                   DiagSink const* diag = nullptr, StrView asset = {}) noexcept;
/// Radiance .hdr (RGBE): flat or run-length encoded scanlines, standard orientation (`-Y H +X W`)
/// only, to 3 channels of f32 (bitsPerChannel 32). Texels decode as Radiance does:
/// (mantissa + 0.5) * 2^(exponent - 136), exponent 0 is black. The header's EXPOSURE is ignored.
KILN_API Result<Image> decode_hdr(Span<u8 const> bytes, Allocator const* alloc,
                                  DiagSink const* diag = nullptr, StrView asset = {}) noexcept;
/// Picks the decoder from the signature. Other bytes return Unsupported (K2003).
KILN_API Result<Image> decode_image(Span<u8 const> bytes, Allocator const* alloc,
                                    DiagSink const* diag = nullptr, StrView asset = {}) noexcept;

// ---------------------------------------------------------------------------
// Half floats, integer-exact
// ---------------------------------------------------------------------------

/// IEEE 754 binary16 from f32, round to nearest even. Values beyond the half range saturate to
/// +-65504 instead of becoming infinity, so one bright texel cannot poison filtering; NaN becomes 0.
[[nodiscard]] KILN_API u16 float_to_half(f32 v) noexcept;
[[nodiscard]] KILN_API f32 half_to_float(u16 h) noexcept;

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
/// v * 257. To 32 (f32): integers become v / max (no sRGB decode), A = 1.0. From 32 only to 32.
/// Returns a copy when nothing changes.
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
/// `src`. Byte-identical to calling them in that order. For f32 output the flags must be off.
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
/// or row is dropped; a dimension of 1 samples the same texel twice. f32 images average as
/// ((a + b) + (c + d)) * 0.25 with no fused multiply-add, identical on every compiler.
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
