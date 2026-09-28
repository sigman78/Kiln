// src/cook/image_decode.cpp — PNG, JPEG and WebP decoding via wuffs (v0.4); all wuffs memory
// comes from the caller's allocator. Decodes to RGBA_NONPREMUL (8-bit) or BGRA_NONPREMUL_4X16LE
// (16-bit PNG), then narrows to the channel count the source declares.
#include "kiln/cook/image.h"
#include "kiln/ktx2.h"

#include <cstring>

// wuffs is compiled once in third_party/wuffs/wuffs_impl.c; here it is only a header.
#include "wuffs_modules.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wduplicated-cond"
#pragma GCC diagnostic ignored "-Wlogical-op"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "wuffs-v0.4.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace kiln::cook {

namespace {

constexpr u8 kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr u32 kMaxDimension   = 16384;
constexpr u64 kMaxImageBytes  = u64(1) << 32;

u32 be32(u8 const* p) noexcept {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 le32(u8 const* p) noexcept {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

struct PngHeader {
    u32 width    = 0;
    u32 height   = 0;
    u8 depth     = 0;
    u8 colorType = 0;
    u8 interlace = 0;
    bool hasTrns = false;
};

/// Validate the IHDR combination (PNG spec table 11.1).
bool valid_depth(u8 colorType, u8 depth) noexcept {
    switch (colorType) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

/// Scan chunk headers up to the first IDAT for a tRNS chunk. Stops quietly on a
/// malformed chunk list; wuffs reports those errors during the real decode.
bool scan_trns(Span<u8 const> bytes) noexcept {
    usize pos = 8;
    while (pos + 8 <= bytes.size) {
        u32 const len  = be32(bytes.data + pos);
        u8 const* type = bytes.data + pos + 4;
        if (std::memcmp(type, "tRNS", 4) == 0) return true;
        if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) return false;
        u64 const next = u64(pos) + 12 + len;
        if (next > bytes.size) return false;
        pos = usize(next);
    }
    return false;
}

Status fail(DiagSink const* diag, StrView asset, char const* kind, Code code, u32 k, char const* fmt,
            unsigned long long a = 0, unsigned long long b = 0) noexcept {
    return diagf(diag, make_status(code), k, Severity::Error, asset, kind, fmt, a, b);
}

/// Map a wuffs error/suspension to kiln's Status and a K2xxx diagnostic.
Status wuffs_fail(DiagSink const* diag, StrView asset, char const* kind, wuffs_base__status st,
                  char const* stage) noexcept {
    char const* msg = st.repr ? st.repr : "unknown";
    if (std::strstr(msg, "unsupported") != nullptr)
        return diagf(diag, make_status(Code::Unsupported), kDiagImageUnsupported, Severity::Error, asset,
                     kind, "%s: %s", stage, msg);
    // Suspensions ("$base: short read") on a closed stream mean truncated input.
    return diagf(diag, make_status(Code::ParseError), kDiagImageDecodeFailed, Severity::Error, asset, kind,
                 "%s: %s", stage, msg);
}

/// Allocation owned for the duration of one decode.
struct Scratch {
    Allocator const* allocator = nullptr;
    void* ptr                  = nullptr;
    usize size                 = 0;
    usize align                = 0;

    Scratch(Allocator const* a, usize n, usize al) noexcept : allocator(a), size(n), align(al) {
        if (n) ptr = kiln::alloc(a, n, al, Tag::Cook);
    }
    ~Scratch() noexcept { kiln::free(allocator, ptr, size, align, Tag::Cook); }
    Scratch(Scratch const&)            = delete;
    Scratch& operator=(Scratch const&) = delete;
    [[nodiscard]] u8* bytes() const noexcept { return static_cast<u8*>(ptr); }
};

/// What the caller already knows about the image. `channels == 0` takes the channel
/// count from the decoder's source pixel format.
struct Expect {
    u32 width    = 0; ///< 0: not checked
    u32 height   = 0;
    u32 channels = 0;
    u32 bits     = 8;
};

u32 channels_from(wuffs_base__pixel_format pf) noexcept {
    if (pf.repr == WUFFS_BASE__PIXEL_FORMAT__Y) return 1;
    return wuffs_base__pixel_format__transparency(&pf) == WUFFS_BASE__PIXEL_ALPHA_TRANSPARENCY__OPAQUE ? 3u
                                                                                                       : 4u;
}

/// Shared tail of every decoder: image config, size checks, decode to RGBA, narrow.
Result<Image> decode_with(wuffs_base__image_decoder* dec, Span<u8 const> bytes, Expect expect,
                          Allocator const* alloc, DiagSink const* diag, StrView asset,
                          char const* kind) noexcept {
    // wuffs only reads through the io_buffer; the const_cast never leads to a write.
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(const_cast<u8*>(bytes.data), bytes.size, true);

    wuffs_base__image_config ic = wuffs_base__null_image_config();
    wuffs_base__status st       = wuffs_base__image_decoder__decode_image_config(dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, kind, st, "image config");
    u32 const w   = wuffs_base__pixel_config__width(&ic.pixcfg);
    u32 const hgt = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (expect.width && (w != expect.width || hgt != expect.height))
        return fail(diag, asset, kind, Code::ParseError, kDiagImageDecodeFailed,
                    "header / decoder extent mismatch");
    if (w == 0 || hgt == 0)
        return fail(diag, asset, kind, Code::ParseError, kDiagImageDecodeFailed, "invalid extent %llux%llu",
                    w, hgt);
    if (w > kMaxDimension || hgt > kMaxDimension)
        return fail(diag, asset, kind, Code::Unsupported, kDiagImageTooLarge,
                    "extent %llux%llu exceeds 16384", w, hgt);

    u32 const channels =
        expect.channels ? expect.channels : channels_from(wuffs_base__pixel_config__pixel_format(&ic.pixcfg));
    u32 const bits      = expect.bits;
    u64 const outBytes  = u64(w) * hgt * channels * (bits / 8);
    u64 const workBytes = u64(w) * hgt * 4 * (bits / 8); // 4-channel decode buffer
    if (outBytes > kMaxImageBytes)
        return fail(diag, asset, kind, Code::Unsupported, kDiagImageTooLarge,
                    "decoded size %llu bytes exceeds 2^32", outBytes);

    u32 const dstFormat = bits == 16 ? WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE
                                     : WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL;
    wuffs_base__pixel_config__set(&ic.pixcfg, dstFormat, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, hgt);

    u64 const workLen = wuffs_base__image_decoder__workbuf_len(dec).max_incl;
    if (workLen > kMaxImageBytes * 2)
        return fail(diag, asset, kind, Code::Unsupported, kDiagImageTooLarge,
                    "work buffer %llu bytes too large", workLen);
    Scratch work(alloc, usize(workLen), 16);
    Scratch pixels(alloc, usize(workBytes), 16);

    wuffs_base__pixel_buffer pb;
    st = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg,
                                                  wuffs_base__make_slice_u8(pixels.bytes(), pixels.size));
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, kind, st, "pixel buffer");

    // WUFFS_BASE__PIXEL_BLEND__SRC is a C-style cast of 0; spelled out to keep -Wold-style-cast quiet.
    constexpr auto kBlendSrc = static_cast<wuffs_base__pixel_blend>(0);
    st                       = wuffs_base__image_decoder__decode_frame(dec, &pb, &src, kBlendSrc,
                                                                       wuffs_base__make_slice_u8(work.bytes(), work.size), nullptr);
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, kind, st, "decode");

    Image img;
    img.width          = w;
    img.height         = hgt;
    img.channels       = channels;
    img.bitsPerChannel = bits;
    img.pixels.init(alloc, Tag::Cook);
    img.pixels.resize(usize(outBytes));

    // Source channel order in the decode buffer: RGBA for 8-bit, BGRA for 16-bit.
    // 1 channel = R (gray replicated), 2 = R + A (gray + alpha), 3 = RGB, 4 = RGBA.
    u32 const rgbaIndex[4] = {bits == 16 ? 2u : 0u, 1u, bits == 16 ? 0u : 2u, 3u};
    u32 pick[4]            = {rgbaIndex[0], rgbaIndex[1], rgbaIndex[2], rgbaIndex[3]};
    if (channels == 2) pick[1] = rgbaIndex[3];

    u64 const count = u64(w) * hgt;
    u8 const* s     = pixels.bytes();
    u8* d           = img.pixels.data();
    if (bits == 8) {
        for (u64 i = 0; i < count; ++i, s += 4, d += channels)
            for (u32 c = 0; c < channels; ++c)
                d[c] = s[pick[c]];
    } else {
        for (u64 i = 0; i < count; ++i, s += 8, d += channels * 2) {
            for (u32 c = 0; c < channels; ++c) {
                u8 const* q = s + pick[c] * 2;
                u16 const v = u16(q[0] | (q[1] << 8)); // little-endian -> native
                std::memcpy(d + c * 2, &v, 2);
            }
        }
    }
    return img;
}

} // namespace

bool is_png(Span<u8 const> bytes) noexcept {
    return bytes.size >= sizeof(kPngSignature) &&
           std::memcmp(bytes.data, kPngSignature, sizeof(kPngSignature)) == 0;
}

bool is_jpeg(Span<u8 const> bytes) noexcept {
    return bytes.size >= 3 && bytes.data[0] == 0xFF && bytes.data[1] == 0xD8 && bytes.data[2] == 0xFF;
}

bool is_webp(Span<u8 const> bytes) noexcept {
    return bytes.size >= 12 && std::memcmp(bytes.data, "RIFF", 4) == 0 &&
           std::memcmp(bytes.data + 8, "WEBP", 4) == 0;
}

bool is_ktx2(Span<u8 const> bytes) noexcept {
    return bytes.size >= sizeof(ktx2::kIdentifier) &&
           std::memcmp(bytes.data, ktx2::kIdentifier, sizeof(ktx2::kIdentifier)) == 0;
}

bool is_lossy_image(Span<u8 const> bytes) noexcept {
    if (is_jpeg(bytes)) return true;
    if (!is_webp(bytes)) return false;
    // Lossy WebP carries a "VP8 " chunk: first in a simple file, after "VP8X" in an extended one.
    for (usize pos = 12; pos + 8 <= bytes.size;) {
        if (std::memcmp(bytes.data + pos, "VP8 ", 4) == 0) return true;
        u32 const len = le32(bytes.data + pos + 4);
        pos += 8 + usize(len) + (len & 1u);
    }
    return false;
}

bool webp_decode_enabled() noexcept {
#if defined(KILN_WEBP) && KILN_WEBP
    return true;
#else
    return false;
#endif
}

Result<Image> decode_png(Span<u8 const> bytes, Allocator const* alloc, DiagSink const* diag,
                         StrView asset) noexcept {
    if (!alloc) alloc = default_allocator();
    if (!is_png(bytes))
        return fail(diag, asset, "png", Code::ParseError, kDiagImageDecodeFailed, "not a PNG signature");

    // IHDR is always the first chunk: 4 length + 4 type + 13 data + 4 CRC.
    if (bytes.size < 8 + 8 + 13 + 4 || be32(bytes.data + 8) != 13 ||
        std::memcmp(bytes.data + 12, "IHDR", 4) != 0)
        return fail(diag, asset, "png", Code::ParseError, kDiagImageDecodeFailed,
                    "missing or truncated IHDR");
    u8 const* ihdr = bytes.data + 16;
    PngHeader h;
    h.width     = be32(ihdr + 0);
    h.height    = be32(ihdr + 4);
    h.depth     = ihdr[8];
    h.colorType = ihdr[9];
    h.interlace = ihdr[12];
    if (h.width == 0 || h.height == 0 || h.width > 0x7FFFFFFFu || h.height > 0x7FFFFFFFu)
        return fail(diag, asset, "png", Code::ParseError, kDiagImageDecodeFailed, "invalid extent %llux%llu",
                    h.width, h.height);
    if (!valid_depth(h.colorType, h.depth))
        return fail(diag, asset, "png", Code::ParseError, kDiagImageDecodeFailed,
                    "invalid color type %llu / bit depth %llu", h.colorType, h.depth);
    if (h.width > kMaxDimension || h.height > kMaxDimension)
        return fail(diag, asset, "png", Code::Unsupported, kDiagImageTooLarge,
                    "extent %llux%llu exceeds 16384", h.width, h.height);
    h.hasTrns = scan_trns(bytes);

    Expect e{.width = h.width, .height = h.height, .bits = h.depth == 16 ? 16u : 8u};
    switch (h.colorType) {
    case 0: e.channels = h.hasTrns ? 2 : 1; break; // gray (+ tRNS key -> alpha)
    case 2: e.channels = h.hasTrns ? 4 : 3; break; // RGB
    case 3: e.channels = h.hasTrns ? 4 : 3; break; // palette -> RGB / RGBA
    case 4: e.channels = 2; break;                 // gray + alpha
    case 6: e.channels = 4; break;                 // RGBA
    default: break;
    }

    Scratch decMem(alloc, sizeof__wuffs_png__decoder(), 16);
    auto* dec = static_cast<wuffs_png__decoder*>(decMem.ptr);
    wuffs_base__status const st =
        wuffs_png__decoder__initialize(dec, sizeof__wuffs_png__decoder(), WUFFS_VERSION, 0u);
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, "png", st, "initialize");
    return decode_with(wuffs_png__decoder__upcast_as__wuffs_base__image_decoder(dec), bytes, e, alloc, diag,
                       asset, "png");
}

Result<Image> decode_jpeg(Span<u8 const> bytes, Allocator const* alloc, DiagSink const* diag,
                          StrView asset) noexcept {
    if (!alloc) alloc = default_allocator();
    if (!is_jpeg(bytes))
        return fail(diag, asset, "jpeg", Code::ParseError, kDiagImageDecodeFailed, "not a JPEG signature");

    Scratch decMem(alloc, sizeof__wuffs_jpeg__decoder(), 16);
    auto* dec = static_cast<wuffs_jpeg__decoder*>(decMem.ptr);
    wuffs_base__status const st =
        wuffs_jpeg__decoder__initialize(dec, sizeof__wuffs_jpeg__decoder(), WUFFS_VERSION, 0u);
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, "jpeg", st, "initialize");
    return decode_with(wuffs_jpeg__decoder__upcast_as__wuffs_base__image_decoder(dec), bytes, Expect{}, alloc,
                       diag, asset, "jpeg");
}

Result<Image> decode_webp(Span<u8 const> bytes, Allocator const* alloc, DiagSink const* diag,
                          StrView asset) noexcept {
    if (!alloc) alloc = default_allocator();
    if (!is_webp(bytes))
        return fail(diag, asset, "webp", Code::ParseError, kDiagImageDecodeFailed, "not a WebP signature");
#if defined(KILN_WEBP) && KILN_WEBP
    Scratch decMem(alloc, sizeof__wuffs_webp__decoder(), 16);
    auto* dec = static_cast<wuffs_webp__decoder*>(decMem.ptr);
    wuffs_base__status const st =
        wuffs_webp__decoder__initialize(dec, sizeof__wuffs_webp__decoder(), WUFFS_VERSION, 0u);
    if (!wuffs_base__status__is_ok(&st)) return wuffs_fail(diag, asset, "webp", st, "initialize");
    return decode_with(wuffs_webp__decoder__upcast_as__wuffs_base__image_decoder(dec), bytes, Expect{}, alloc,
                       diag, asset, "webp");
#else
    return fail(diag, asset, "webp", Code::Unsupported, kDiagImageUnsupported,
                "WebP decoding is not built in (configure with KILN_WEBP=ON)");
#endif
}

Result<Image> decode_image(Span<u8 const> bytes, Allocator const* alloc, DiagSink const* diag,
                           StrView asset) noexcept {
    if (is_png(bytes)) return decode_png(bytes, alloc, diag, asset);
    if (is_jpeg(bytes)) return decode_jpeg(bytes, alloc, diag, asset);
    if (is_webp(bytes)) return decode_webp(bytes, alloc, diag, asset);
    return fail(diag, asset, "image", Code::Unsupported, kDiagImageUnknownFormat,
                "not a PNG, JPEG or WebP file (%llu bytes)", bytes.size);
}

} // namespace kiln::cook
