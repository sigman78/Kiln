// tests/test_hdr.cpp — the Radiance .hdr decoder, half floats and the HDR cook (cook-only).
// Design: docs/design/hdr-textures.md.
#include "hdr_writer.h"
#include "kiln_test.h"
#include "ktx2_corpus.h" // texels
#include "png_writer.h"

#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/ktx2.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::cook;
namespace hdr = kiln::test::hdr;

namespace {

struct DiagLast {
    u32 code = 0;
    Severity sev{};
    int count = 0;
    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLast*>(user);
        self->code = d.code;
        self->sev  = d.severity;
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

f32 pixel(Image const& img, u32 x, u32 y, u32 c) {
    f32 v;
    std::memcpy(&v, img.pixels.data() + (usize(y) * img.width + x) * img.channels * 4 + c * 4, 4);
    return v;
}

u16 half_at(Span<u8 const> level, usize texel, u32 c) {
    u16 h;
    std::memcpy(&h, level.data + texel * 8 + c * 2, 2);
    return h;
}

Vec<u8> bytes_of(char const* s) {
    Vec<u8> v(default_allocator(), Tag::Test);
    v.append(Span<u8 const>(reinterpret_cast<u8 const*>(s), std::strlen(s)));
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

KILN_TEST(Hdr, DecodesFlatTexels) {
    // (m + 0.5) * 2^(e - 136): e = 129 is 1/128, so 128 -> 1.00390625; e = 0 is black.
    u8 const rgbe[] = {128, 64, 32, 129, 255, 255, 255, 136, 10, 20, 30, 0, 200, 100, 50, 130};
    Vec<u8> const f = hdr::encode_flat(2, 2, Span<u8 const>(rgbe, sizeof rgbe));
    KILN_CHECK(is_hdr(f.span()));
    Result<Image> r = decode_hdr(f.span(), default_allocator());
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->width == 2 && r->height == 2 && r->channels == 3 && r->bitsPerChannel == 32);
    KILN_CHECK_EQ(pixel(*r, 0, 0, 0), 128.5f / 128.0f);
    KILN_CHECK_EQ(pixel(*r, 0, 0, 1), 64.5f / 128.0f);
    KILN_CHECK_EQ(pixel(*r, 0, 0, 2), 32.5f / 128.0f);
    KILN_CHECK_EQ(pixel(*r, 1, 0, 0), 255.5f); // e = 136: 2^0
    KILN_CHECK_EQ(pixel(*r, 0, 1, 1), 0.0f);   // e = 0
    KILN_CHECK_EQ(pixel(*r, 1, 1, 2), 50.5f / 64.0f);
    // decode_image picks it by signature.
    KILN_CHECK(decode_image(f.span(), default_allocator()).ok());
}

KILN_TEST(Hdr, DecodesRunLengthScanlines) {
    // One 8-wide scanline, run-length encoded per channel, against its flat equivalent.
    u8 flat[8 * 4];
    for (u32 x = 0; x < 8; ++x) {
        flat[x * 4 + 0] = 128;                       // R: one run of 8
        flat[x * 4 + 1] = u8(130 + x);               // G: one literal of 8
        flat[x * 4 + 2] = x < 4 ? 140 : u8(150 + x); // B: a run of 4, then a literal of 4
        flat[x * 4 + 3] = 131;                       // E: one run of 8
    }
    Vec<u8> const expected = hdr::encode_flat(8, 1, Span<u8 const>(flat, sizeof flat));
    Vec<u8> rle            = hdr::encode_flat(8, 1, {});
    u8 const line[]        = {2,   2,   0,   8,                            // RLE scanline, width 8
                              136, 128,                                    // R
                              8,   130, 131, 132, 133, 134, 135, 136, 137, // G
                              132, 140, 4,   154, 155, 156, 157,           // B
                              136, 131};                                   // E
    rle.append(Span<u8 const>(line, sizeof line));
    Result<Image> a = decode_hdr(expected.span(), default_allocator());
    Result<Image> b = decode_hdr(rle.span(), default_allocator());
    KILN_REQUIRE(a.ok() && b.ok());
    KILN_CHECK(a->pixels.size() == b->pixels.size() &&
               std::memcmp(a->pixels.data(), b->pixels.data(), a->pixels.size()) == 0);
}

KILN_TEST(Hdr, RejectsMalformedFiles) {
    u8 texels[8 * 4] = {};
    struct Case {
        Vec<u8> bytes;
        Code code;
        u32 diag;
    };
    auto rle = [](u8 const* extra, usize n) {
        Vec<u8> v = hdr::encode_flat(8, 1, {});
        v.append(Span<u8 const>(extra, n));
        return v;
    };
    u8 const wrongWidth[] = {2, 2, 0, 9};
    u8 const overflow[]   = {2, 2, 0, 8, 137, 1};     // a run of 9 in an 8-wide scanline
    u8 const zeroLit[]    = {2, 2, 0, 8, 0};          // a literal of 0
    u8 const cut[]        = {2, 2, 0, 8, 8, 1, 2, 3}; // a literal of 8 with 3 bytes
    Case const cases[]    = {
        {bytes_of("P6 not an hdr"), Code::ParseError, kDiagImageDecodeFailed},
        {bytes_of("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n"), Code::ParseError, kDiagImageDecodeFailed},
        {hdr::encode_flat(8, 1, Span<u8 const>(texels, 32), "32-bit_rle_xyze"), Code::Unsupported,
         kDiagImageUnsupported},
        {hdr::encode_flat(8, 1, Span<u8 const>(texels, 32), "32-bit_rle_rgbe", "+Y 1 +X 8"),
         Code::Unsupported, kDiagImageUnsupported},
        {hdr::encode_flat(8, 1, Span<u8 const>(texels, 32), "32-bit_rle_rgbe", "-Y one +X 8"),
         Code::ParseError, kDiagImageDecodeFailed},
        {hdr::encode_flat(8, 0, {}), Code::ParseError, kDiagImageDecodeFailed},
        {hdr::encode_flat(20000, 1, {}), Code::Unsupported, kDiagImageTooLarge},
        {hdr::encode_flat(8, 1, Span<u8 const>(texels, 31)), Code::ParseError, kDiagImageDecodeFailed},
        {rle(wrongWidth, sizeof wrongWidth), Code::ParseError, kDiagImageDecodeFailed},
        {rle(overflow, sizeof overflow), Code::ParseError, kDiagImageDecodeFailed},
        {rle(zeroLit, sizeof zeroLit), Code::ParseError, kDiagImageDecodeFailed},
        {rle(cut, sizeof cut), Code::ParseError, kDiagImageDecodeFailed},
    };
    for (usize i = 0; i < countof(cases); ++i) {
        DiagLast d;
        DiagSink const sink = d.sink();
        Result<Image> r     = decode_hdr(cases[i].bytes.span(), default_allocator(), &sink);
        KILN_CHECK_MSG(r.code() == cases[i].code && d.code == cases[i].diag, "case %zu: %s, K%u", i,
                       code_name(r.code()), d.code);
    }
}

// ---------------------------------------------------------------------------
// Half floats
// ---------------------------------------------------------------------------

KILN_TEST(Hdr, FloatToHalfRounding) {
    struct Case {
        f32 v;
        u16 h;
    };
    Case const cases[] = {
        {0.0f, 0x0000},
        {-0.0f, 0x8000},
        {1.0f, 0x3C00},
        {-2.0f, 0xC000},
        {0.5f, 0x3800},
        {65504.0f, 0x7BFF},
        {65519.0f, 0x7BFF},
        {65520.0f, 0x7BFF}, // would round to infinity: saturates
        {1e9f, 0x7BFF},
        {-INFINITY, 0xFBFF},
        {NAN, 0x0000},
        {std::ldexp(1.0f, -14), 0x0400}, // smallest normal
        {std::ldexp(1.0f, -24), 0x0001}, // smallest subnormal
        {std::ldexp(1.0f, -25), 0x0000}, // half a step: ties to even (zero)
        {std::ldexp(3.0f, -26), 0x0001}, // 0.75 of a step rounds up
        {1.0f + std::ldexp(1.0f, -11), 0x3C00}, // tie between 1 and 1 + 2^-10: to even
        {1.0f + std::ldexp(3.0f, -11), 0x3C02}, // tie: to even, upwards
    };
    for (Case const& c : cases)
        KILN_CHECK_MSG(float_to_half(c.v) == c.h, "%g -> %04x, expected %04x", double(c.v),
                       unsigned(float_to_half(c.v)), unsigned(c.h));
}

// Every finite half survives half -> float -> half unchanged.
KILN_TEST(Hdr, HalfRoundTripsExhaustively) {
    u32 bad = 0;
    for (u32 h = 0; h < 0x10000; ++h) {
        if ((h & 0x7C00) == 0x7C00) continue; // infinities and NaNs
        if (float_to_half(half_to_float(u16(h))) != h) ++bad;
    }
    KILN_CHECK_EQ(bad, 0u);
}

// ---------------------------------------------------------------------------
// Cook
// ---------------------------------------------------------------------------

namespace {

Result<CookedTexture> cook_hdr(Span<u8 const> bytes, TextureCookSettings s, DiagLast* d = nullptr) {
    DiagSink sink = d ? d->sink() : DiagSink{};
    return cook_texture({.bytes = bytes, .assetPath = "test/hdr.hdr", .sourcePath = "hdr.hdr"}, s,
                        TargetProfile{.blockFamily = BlockFamily::None}, {.diag = &sink});
}

void write_sample(char const* name, Span<u8 const> bytes) {
    char path[1024];
    format(path, sizeof path, "%s/cooked_%s.ktx2", kiln::test::sample_dir(), name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", path)) return;
    KILN_CHECK(std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size);
    std::fclose(f);
}

} // namespace

KILN_TEST(Hdr, CooksToRgba16f) {
    u8 rgbe[4 * 4 * 4];
    hdr::pattern(rgbe, 16, 3);
    Vec<u8> const f         = hdr::encode_flat(4, 4, Span<u8 const>(rgbe, sizeof rgbe));
    Result<CookedTexture> r = cook_hdr(f.span(), {.usage = TextureUsage::Hdr});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R16G16B16A16_SFLOAT);
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->desc().levels, 3u);
    Result<Image> src = decode_hdr(f.span(), default_allocator());
    KILN_REQUIRE(src.ok());
    Vec<u8> const t0        = kiln::test::corpus::texels(*v, 0);
    Span<u8 const> const l0 = t0.span();
    KILN_REQUIRE(l0.size >= 16 * 8);
    for (u32 i = 0; i < 16; ++i) {
        for (u32 c = 0; c < 3; ++c)
            KILN_CHECK_EQ(half_at(l0, i, c), float_to_half(pixel(*src, i % 4, i / 4, c)));
        KILN_CHECK_EQ(half_at(l0, i, 3), u16(0x3C00)); // alpha 1
    }
    // Level 1 texel 0 is the average of the top-left 2x2, rounded to half once.
    f32 const avg =
        ((pixel(*src, 0, 0, 0) + pixel(*src, 1, 0, 0)) + (pixel(*src, 0, 1, 0) + pixel(*src, 1, 1, 0))) *
        0.25f;
    KILN_CHECK_EQ(half_at(kiln::test::corpus::texels(*v, 1).span(), 0, 0), float_to_half(avg));
    write_sample("hdr", r->file.span());
}

KILN_TEST(Hdr, UsageRules) {
    u8 rgbe[4 * 4 * 4];
    hdr::pattern(rgbe, 16, 5);
    Vec<u8> const f = hdr::encode_flat(4, 4, Span<u8 const>(rgbe, sizeof rgbe));
    // An HDR source cannot become an 8-bit color texture.
    DiagLast d;
    KILN_CHECK_EQ(cook_hdr(f.span(), {.usage = TextureUsage::Color}, &d).code(), Code::Unsupported);
    KILN_CHECK_EQ(d.code, u32(kDiagImageUnsupported));
    // An 8-bit source with usage Hdr cooks to RGBA16F with a warning.
    u8 rgba[4 * 4 * 4];
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(i * 13);
    Vec<u8> const png =
        kiln::test::png::encode({.width = 4, .height = 4, .colorType = 6, .depth = 8, .pixels = rgba});
    DiagLast w;
    Result<CookedTexture> r = cook_hdr(png.span(), {.usage = TextureUsage::Hdr}, &w);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R16G16B16A16_SFLOAT);
    KILN_CHECK(w.code == kDiagImageNoHdrRange && w.sev == Severity::Warning);
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(half_at(kiln::test::corpus::texels(*v, 0).span(), 1, 0),
                  float_to_half(f32(rgba[4]) / 255.0f));
}

// A .hdr strip cooks into an HDR cube like any other strip.
KILN_TEST(Hdr, CubeStrip) {
    u8 rgbe[4 * 24 * 4];
    hdr::pattern(rgbe, 4 * 24, 9);
    Vec<u8> const f         = hdr::encode_flat(4, 24, Span<u8 const>(rgbe, sizeof rgbe));
    Result<CookedTexture> r = cook_hdr(f.span(), {.usage = TextureUsage::Hdr, .shape = CookShape::Cube});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->desc.isCube && r->desc.faces == 6 && r->desc.format == Format::R16G16B16A16_SFLOAT);
}
