// tests/test_texture_golden.cpp — texture cooker golden files (cook-only); see tests/golden/README.md.
// Cases mirror test_texture_cook.cpp's color/normal/height ones at smaller, distinct sizes,
// so a golden mismatch cannot be confused with theirs.
#include "hdr_writer.h"
#include "kiln_test.h"
#include "ktx2_corpus.h" // read_file, bytes_equal
#include "png_writer.h"

#include "kiln/containers.h"
#include "kiln/cook/cook.h"

#include <cerrno>
#include <cstdio>

#if defined(KILN_OS_WINDOWS)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif

using namespace kiln;
using namespace kiln::cook;
namespace png    = kiln::test::png;
namespace corpus = kiln::test::corpus;

namespace {

void pattern(u8* p, usize n, u32 seed) {
    u32 s = seed;
    for (usize i = 0; i < n; ++i) {
        s    = s * 1664525u + 1013904223u;
        p[i] = u8(s >> 24);
    }
}

bool ensure_dir(char const* dir) {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(dir) == 0) return true;
#else
    if (mkdir(dir, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// Compares `got` against the committed golden `<golden_dir>/ktx2/<case>.ktx2`, or
/// (with --update-golden) writes it.
void check_golden_ktx2(char const* name, Span<u8 const> got) {
    char const* golden = kiln::test::golden_dir();
    char path[1024];
    format(path, sizeof path, "%s/ktx2/%s.ktx2", golden, name);

    if (kiln::test::update_golden()) {
        char dir[1024];
        format(dir, sizeof dir, "%s/ktx2", golden);
        if (!KILN_CHECK_MSG(ensure_dir(dir), "cannot create %s (errno %d)", dir, errno)) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!KILN_CHECK_MSG(f != nullptr, "cannot write %s", path)) return;
        bool const ok = got.empty() || std::fwrite(got.data, 1, got.size, f) == got.size;
        std::fclose(f);
        KILN_CHECK_MSG(ok, "short write to %s", path);
        std::printf("golden: wrote %s (%zu bytes)\n", path, got.size);
        return;
    }

    Vec<u8> want(default_allocator(), Tag::Test);
    if (!KILN_CHECK_MSG(corpus::read_file(path, want),
                        "cannot read golden %s (run with --update-golden first)", path))
        return;
    if (corpus::bytes_equal(got, want.span())) return;

    usize const n = got.size < want.size() ? got.size : want.size();
    usize diffAt  = n;
    for (usize i = 0; i < n; ++i)
        if (got.data[i] != want[i]) {
            diffAt = i;
            break;
        }
    KILN_CHECK_MSG(false,
                   "%s: mismatch vs golden (got %zu bytes, golden %zu bytes; first differs at offset "
                   "%zu). If this is a deliberate change, bump kCookerVersion (kiln/cook/cook.h) and "
                   "rerun with --update-golden.",
                   name, got.size, want.size(), diffAt);
}

/// The uncompressed goldens predate block compression; the BC cases pass a BC target.
Result<CookedTexture> run_cook(Span<u8 const> bytes, TextureCookSettings const& s,
                               TargetProfile const& target = {.blockFormats = 0}) {
    return cook_texture({.bytes = bytes, .assetPath = "test/golden", .sourcePath = "golden.png"}, s, target);
}

/// Smooth content, so the encoders' search paths matter (noise would make every block alike).
void gradient(u8* rgba, u32 w, u32 h) {
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = rgba + (usize(y) * w + x) * 4;
            p[0]  = u8(x * 37 + y * 5);
            p[1]  = u8(y * 29 + 40);
            p[2]  = u8((x ^ y) * 17);
            p[3]  = u8(255 - x * 9);
        }
}

} // namespace

KILN_TEST(TextureGolden, ColorSrgb7x5) {
    u8 rgba[7 * 5 * 4];
    pattern(rgba, sizeof rgba, 101);
    Vec<u8> f = png::encode({.width = 7, .height = 5, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    check_golden_ktx2("color_srgb", r->file.span());
}

// Big and smooth enough for Zstd to pay (the small cases stay plain): pins the encoder's frames.
KILN_TEST(TextureGolden, ColorZstd64x64) {
    u8 rgba[64 * 64 * 4];
    for (u32 i = 0; i < 64 * 64; ++i) {
        u32 const x = i % 64, y = i / 64;
        u8 const px[4] = {u8(x * 4), u8(y * 4), u8(x + y), 255};
        std::memcpy(rgba + i * 4, px, 4);
    }
    Vec<u8> f = png::encode({.width = 64, .height = 64, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_CHECK(v.ok() && v->supercompressed());
    check_golden_ktx2("color_zstd", r->file.span());
}

KILN_TEST(TextureGolden, Normal8x8) {
    u8 rgba[8 * 8 * 4];
    pattern(rgba, sizeof rgba, 102);
    Vec<u8> f = png::encode({.width = 8, .height = 8, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    check_golden_ktx2("normal", r->file.span());
}

// f32 mips and half conversion must give the same bytes on every compiler.
KILN_TEST(TextureGolden, Hdr8x8) {
    u8 rgbe[8 * 8 * 4];
    kiln::test::hdr::pattern(rgbe, 64, 11);
    Vec<u8> f               = kiln::test::hdr::encode_flat(8, 8, Span<u8 const>(rgbe, sizeof rgbe));
    Result<CookedTexture> r = run_cook(f.span(), {.usage = TextureUsage::Hdr});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    check_golden_ktx2("hdr", r->file.span());
}

KILN_TEST(TextureGolden, Height16_4x4) {
    u16 g[4 * 4];
    for (u32 i = 0; i < 16; ++i)
        g[i] = u16(i * 4093u);
    Vec<u8> gb = png::be16(g);
    Vec<u8> f  = png::encode({.width = 4, .height = 4, .colorType = 0, .depth = 16, .pixels = gb.span()});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Height});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    check_golden_ktx2("height16", r->file.span());
}

// BC goldens: the encoders must give the same bytes with every compiler and OS in CI.
KILN_TEST(TextureGolden, Bc7Color12x9) {
    u8 rgba[12 * 9 * 4];
    gradient(rgba, 12, 9);
    Vec<u8> f = png::encode({.width = 12, .height = 9, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color},
                 {.blockFormats = kDesktopBlockFormats});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    KILN_CHECK(r->desc.format == Format::BC7_SRGB);
    check_golden_ktx2("bc7_color_srgb", r->file.span());
}

KILN_TEST(TextureGolden, Bc5Normal16x8) {
    u8 rgba[16 * 8 * 4];
    gradient(rgba, 16, 8);
    Vec<u8> f = png::encode({.width = 16, .height = 8, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal},
                 {.blockFormats = kDesktopBlockFormats});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    KILN_CHECK(r->desc.format == Format::BC5_UNORM);
    check_golden_ktx2("bc5_normal", r->file.span());
}

KILN_TEST(TextureGolden, Bc6hHdr8x8) {
    u8 rgbe[8 * 8 * 4];
    kiln::test::hdr::pattern(rgbe, 64, 12);
    Vec<u8> f = kiln::test::hdr::encode_flat(8, 8, Span<u8 const>(rgbe, sizeof rgbe));
    Result<CookedTexture> r =
        run_cook(f.span(), {.usage = TextureUsage::Hdr}, {.blockFormats = kDesktopBlockFormats});
    if (!KILN_CHECK_MSG(r.ok(), "cook failed")) return;
    KILN_CHECK(r->desc.format == Format::BC6H_UFLOAT);
    check_golden_ktx2("bc6h_hdr", r->file.span());
}

// BC7 at every quality (bc7f for Fast and Normal, bc7enc for High), on opaque and translucent blocks.
KILN_TEST(TextureGolden, Bc7Qualities16x16) {
    u8 rgba[16 * 16 * 4];
    for (u32 i = 0; i < 16 * 16; ++i) {
        u32 const x = i % 16, y = i / 16;
        rgba[i * 4 + 0] = u8(x * 16 + ((i * 37u) & 15u));
        rgba[i * 4 + 1] = u8(y * 16 + ((i * 91u) & 31u));
        rgba[i * 4 + 2] = u8((x ^ y) * 17u);
        rgba[i * 4 + 3] = x < 8 ? u8(255) : u8(y * 16 + x);
    }
    Vec<u8> f = png::encode({.width = 16, .height = 16, .colorType = 6, .depth = 8, .pixels = rgba});
    struct Case {
        EncodeQuality quality;
        char const* golden;
    };
    Case const cases[] = {
        {EncodeQuality::Fast,   "bc7_alpha_fast"  },
        {EncodeQuality::Normal, "bc7_alpha_normal"},
        {EncodeQuality::High,   "bc7_alpha_high"  },
    };
    for (Case const& c : cases) {
        Result<CookedTexture> r = run_cook(
            f.span(), {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color, .quality = c.quality},
            {.blockFormats = kDesktopBlockFormats});
        if (!KILN_CHECK_MSG(r.ok(), "%s: cook failed", c.golden)) return;
        KILN_CHECK(r->desc.format == Format::BC7_SRGB);
        check_golden_ktx2(c.golden, r->file.span());
    }
}

// BC1 and BC4 at High quality, the rgbcx paths the defaults do not take.
KILN_TEST(TextureGolden, Bc1Bc4High8x8) {
    u8 rgba[8 * 8 * 4];
    gradient(rgba, 8, 8);
    Vec<u8> f = png::encode({.width = 8, .height = 8, .colorType = 6, .depth = 8, .pixels = rgba});
    TargetProfile const bc{.blockFormats = kDesktopBlockFormats};
    Result<CookedTexture> r1 = run_cook(f.span(),
                                        {.colorSpace = ColorSpace::Linear,
                                         .usage      = TextureUsage::Color,
                                         .encoding   = TextureEncoding::BC1,
                                         .quality    = EncodeQuality::High},
                                        bc);
    if (!KILN_CHECK_MSG(r1.ok(), "BC1 cook failed")) return;
    check_golden_ktx2("bc1_high", r1->file.span());
    Result<CookedTexture> r4 = run_cook(
        f.span(),
        {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Mask, .quality = EncodeQuality::High}, bc);
    if (!KILN_CHECK_MSG(r4.ok(), "BC4 cook failed")) return;
    KILN_CHECK(r4->desc.format == Format::BC4_UNORM);
    check_golden_ktx2("bc4_mask_high", r4->file.span());
}
