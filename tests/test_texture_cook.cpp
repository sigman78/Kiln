// tests/test_texture_cook.cpp — cook_texture (PNG -> KTX2, KTX2 pass-through); cook-only.
// Inputs come from png_writer.h, the KTX2 corpus and image_fixtures.h (JPEG). Every output is
// re-opened with Ktx2View and written as <sample_dir>/cooked_<case>.ktx2 for
// `ktx validate`.
#include "image_fixtures.h"
#include "kiln_test.h"
#include "ktx2_corpus.h"
#include "png_writer.h"

#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/io.h"
#include "kiln/ktx2.h"

#include <cmath>
#include <cstdio>

using namespace kiln;
using namespace kiln::cook;
namespace png = kiln::test::png;
namespace img = kiln::test::img;

namespace {

struct DiagLog {
    u32 codes[32]    = {};
    Severity sev[32] = {};
    int count        = 0;
    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLog*>(user);
        if (self->count < 32) {
            self->codes[self->count] = d.code;
            self->sev[self->count]   = d.severity;
        }
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
    [[nodiscard]] bool has(u32 code, Severity s) const {
        for (int i = 0; i < count && i < 32; ++i)
            if (codes[i] == code && sev[i] == s) return true;
        return false;
    }
};

void pattern(u8* p, usize n, u32 seed) {
    u32 s = seed;
    for (usize i = 0; i < n; ++i) {
        s    = s * 1664525u + 1013904223u;
        p[i] = u8(s >> 24);
    }
}

void write_sample(char const* name, Span<u8 const> bytes) {
    char const* dir = kiln::test::sample_dir();
    char path[1024];
    format(path, sizeof path, "%s/cooked_%s.ktx2", dir, name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", path)) return;
    KILN_CHECK(std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size);
    std::fclose(f);
}

Result<CookedTexture> run_cook(Span<u8 const> bytes, TextureCookSettings const& s, DiagLog* log = nullptr,
                               TargetProfile const& target = {}, JobSystem const* jobs = nullptr) {
    DiagSink sink = log ? log->sink() : DiagSink{};
    return cook_texture({.bytes = bytes, .assetPath = "test/tex", .sourcePath = "tex.png"}, s, target,
                        {.diag = &sink, .jobs = jobs});
}

/// Open the cooked file and check it matches `desc`. Returns false on failure.
bool check_file(CookedTexture const& t, char const* name) {
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(t.file.span());
    if (!KILN_CHECK_MSG(v.ok(), "%s: cooked KTX2 does not open", name)) return false;
    ktx2::TextureDesc const d = v->desc();
    KILN_CHECK_EQ(d.format, t.desc.format);
    KILN_CHECK_EQ(d.width, t.desc.width);
    KILN_CHECK_EQ(d.height, t.desc.height);
    KILN_CHECK_EQ(d.levels, t.desc.levels);
    KILN_CHECK(v->has_all_level_data());
    write_sample(name, t.file.span());
    return true;
}

constexpr TextureCookSettings kColor = {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color};

double srgb_to_linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double linear_to_srgb(double l) {
    return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

} // namespace

KILN_TEST(texture_cook, color_srgb_7x5) {
    u8 rgba[7 * 5 * 4];
    pattern(rgba, sizeof rgba, 11);
    Vec<u8> f = png::encode({.width = 7, .height = 5, .colorType = 6, .depth = 8, .pixels = rgba});

    Result<CookedTexture> r = run_cook(f.span(), kColor);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r->passthrough);
    KILN_CHECK_EQ(r->sourceHash, xxh64(f.span()));
    KILN_CHECK_EQ(r->desc.format, Format::R8G8B8A8_SRGB);
    KILN_CHECK_EQ(r->desc.width, 7u);
    KILN_CHECK_EQ(r->desc.height, 5u);
    KILN_CHECK_EQ(r->desc.levels, 3u);
    KILN_REQUIRE(check_file(*r, "color_srgb"));

    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    Span<u8 const> l0 = v->level_data(0);
    KILN_REQUIRE_EQ(l0.size, sizeof rgba);
    KILN_CHECK(std::memcmp(l0.data, rgba, sizeof rgba) == 0);

    // Level 1 is 3x2: each texel is the sRGB-correct 2x2 average of level 0.
    Span<u8 const> l1 = v->level_data(1);
    KILN_REQUIRE_EQ(l1.size, usize(3 * 2 * 4));
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            u8 const* p[4] = {rgba + ((2 * y) * 7 + 2 * x) * 4, rgba + ((2 * y) * 7 + 2 * x + 1) * 4,
                              rgba + ((2 * y + 1) * 7 + 2 * x) * 4, rgba + ((2 * y + 1) * 7 + 2 * x + 1) * 4};
            u8 const* got  = l1.data + (y * 3 + x) * 4;
            for (u32 c = 0; c < 4; ++c) {
                u32 expect;
                if (c < 3) {
                    u32 sum = 2;
                    for (u8 const* q : p)
                        sum += srgb8_to_linear16(q[c]);
                    expect = linear16_to_srgb8(u16(sum / 4));
                    // Independent float reference: within one step of the exact transfer.
                    double lin = 0.0;
                    for (u8 const* q : p)
                        lin += srgb_to_linear(q[c] / 255.0);
                    double const ref = linear_to_srgb(lin / 4.0) * 255.0;
                    KILN_CHECK_MSG(std::fabs(ref - double(got[c])) <= 1.0,
                                   "texel (%u,%u) c%u: %u vs float %.2f", x, y, c, u32(got[c]), ref);
                } else {
                    expect = (u32(p[0][3]) + p[1][3] + p[2][3] + p[3][3] + 2) / 4;
                }
                KILN_CHECK_MSG(got[c] == expect, "texel (%u,%u) c%u: %u != %u", x, y, c, u32(got[c]), expect);
            }
        }
    }
    // Level 2 is 1x1.
    KILN_CHECK_EQ(v->level_data(2).size, usize(4));
}

KILN_TEST(texture_cook, color_rgb_and_linear) {
    u8 rgb[4 * 4 * 3];
    pattern(rgb, sizeof rgb, 12);
    Vec<u8> f = png::encode({.width = 4, .height = 4, .colorType = 2, .depth = 8, .pixels = rgb});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Color});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R8G8B8A8_UNORM);
    KILN_REQUIRE(check_file(*r, "color_linear"));
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    Span<u8 const> l0 = v->level_data(0);
    KILN_CHECK_EQ(u32(l0[0]), u32(rgb[0]));
    KILN_CHECK_EQ(u32(l0[3]), 255u); // alpha added
}

KILN_TEST(texture_cook, normal_7x5) {
    u8 rgba[7 * 5 * 4];
    pattern(rgba, sizeof rgba, 11);
    Vec<u8> f = png::encode({.width = 7, .height = 5, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R8G8B8A8_UNORM);
    KILN_CHECK_EQ(r->desc.levels, 3u);
    KILN_REQUIRE(check_file(*r, "normal"));

    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    for (u32 level = 0; level < 3; ++level) {
        Span<u8 const> d = v->level_data(level);
        for (usize i = 0; i + 4 <= d.size; i += 4) {
            double const x = d[i] / 127.5 - 1.0, y = d[i + 1] / 127.5 - 1.0, z = d[i + 2] / 127.5 - 1.0;
            double const len = std::sqrt(x * x + y * y + z * z);
            KILN_CHECK_MSG(std::fabs(len - 1.0) < 0.02, "level %u texel %u: length %.4f", level, u32(i / 4),
                           len);
        }
    }
    // Level 0 equals renormalize() of the source; alpha is untouched.
    Span<u8 const> l0 = v->level_data(0);
    u8 const n0[4]    = {rgba[0], rgba[1], rgba[2], rgba[3]};
    Image one;
    one.width          = 1;
    one.height         = 1;
    one.channels       = 4;
    one.bitsPerChannel = 8;
    one.pixels.init(default_allocator(), Tag::Test);
    one.pixels.append(Span<u8 const>(n0, 4));
    renormalize(one);
    KILN_CHECK(std::memcmp(l0.data, one.pixels.data(), 4) == 0);

    // flipGreen inverts G before renormalizing.
    Result<CookedTexture> fl = run_cook(
        f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal, .flipGreen = true});
    KILN_REQUIRE(fl.ok());
    KILN_CHECK(!(fl->file.size() == r->file.size() &&
                 std::memcmp(fl->file.data(), r->file.data(), r->file.size()) == 0));
}

KILN_TEST(texture_cook, height_r16) {
    u16 g[8 * 8];
    for (u32 i = 0; i < 64; ++i)
        g[i] = u16(i * 1021u);
    Vec<u8> gb = png::be16(g);
    Vec<u8> f  = png::encode({.width = 8, .height = 8, .colorType = 0, .depth = 16, .pixels = gb.span()});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Height});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R16_UNORM);
    KILN_CHECK_EQ(r->desc.levels, 4u);
    KILN_REQUIRE(check_file(*r, "height16"));
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    Span<u8 const> l0 = v->level_data(0);
    KILN_REQUIRE_EQ(l0.size, usize(128));
    for (u32 i = 0; i < 64; ++i)
        KILN_CHECK_EQ(u32(l0[i * 2] | (l0[i * 2 + 1] << 8)), u32(g[i]));
}

KILN_TEST(texture_cook, mask_rg8) {
    u8 ga[8 * 8 * 2];
    pattern(ga, sizeof ga, 13);
    Vec<u8> f = png::encode({.width = 8, .height = 8, .colorType = 4, .depth = 8, .pixels = ga});
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Mask});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.format, Format::R8G8_UNORM);
    KILN_REQUIRE(check_file(*r, "mask_rg"));
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK(std::memcmp(v->level_data(0).data, ga, sizeof ga) == 0);
}

KILN_TEST(texture_cook, channel_mismatch) {
    u8 rgba[4 * 4 * 4];
    pattern(rgba, sizeof rgba, 14);
    Vec<u8> f = png::encode({.width = 4, .height = 4, .colorType = 6, .depth = 8, .pixels = rgba});
    DiagLog log;
    Result<CookedTexture> h =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Height}, &log);
    KILN_REQUIRE(h.ok());
    KILN_CHECK_EQ(h->desc.format, Format::R8_UNORM);
    KILN_CHECK(log.has(kDiagImageChannelMismatch, Severity::Warning));

    Result<CookedTexture> lut =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Lut});
    KILN_REQUIRE(lut.ok());
    KILN_CHECK_EQ(lut->desc.format, Format::R8G8B8A8_UNORM);

    Result<CookedTexture> orm =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Orm});
    KILN_REQUIRE(orm.ok());
    KILN_CHECK_EQ(orm->desc.format, Format::R8G8B8A8_UNORM);
}

KILN_TEST(texture_cook, no_mips) {
    u8 rgba[8 * 8 * 4];
    pattern(rgba, sizeof rgba, 15);
    Vec<u8> f = png::encode({.width = 8, .height = 8, .colorType = 6, .depth = 8, .pixels = rgba});
    TextureCookSettings s   = kColor;
    s.genMips               = false;
    Result<CookedTexture> r = run_cook(f.span(), s);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.levels, 1u);
    KILN_CHECK(check_file(*r, "nomips"));
}

KILN_TEST(texture_cook, max_size) {
    u8 rgba[16 * 16 * 4];
    pattern(rgba, sizeof rgba, 16);
    Vec<u8> f = png::encode({.width = 16, .height = 16, .colorType = 6, .depth = 8, .pixels = rgba});
    TextureCookSettings s = kColor;
    s.maxSize             = 4;
    DiagLog log;
    Result<CookedTexture> r = run_cook(f.span(), s, &log);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc.width, 4u);
    KILN_CHECK_EQ(r->desc.height, 4u);
    KILN_CHECK_EQ(r->desc.levels, 3u);
    KILN_CHECK(log.has(kDiagImageDownscaled, Severity::Info));
    KILN_CHECK(check_file(*r, "maxsize"));

    // The top level equals level 2 of the uncapped cook.
    Result<CookedTexture> full = run_cook(f.span(), kColor);
    KILN_REQUIRE(full.ok());
    Result<ktx2::Ktx2View> a = ktx2::Ktx2View::open(r->file.span());
    Result<ktx2::Ktx2View> b = ktx2::Ktx2View::open(full->file.span());
    KILN_REQUIRE(a.ok() && b.ok());
    KILN_CHECK(kiln::test::corpus::bytes_equal(a->level_data(0), b->level_data(2)));

    // The target cap applies too, and genMips=false keeps a single level.
    s.maxSize                = 0;
    s.genMips                = false;
    TargetProfile const tiny = {.name = "tiny", .maxTextureSize = 8};
    DiagLog log2;
    Result<CookedTexture> t = run_cook(f.span(), s, &log2, tiny);
    KILN_REQUIRE(t.ok());
    KILN_CHECK_EQ(t->desc.width, 8u);
    KILN_CHECK_EQ(t->desc.levels, 1u);
    KILN_CHECK(log2.has(kDiagImageDownscaled, Severity::Info));
    Result<ktx2::Ktx2View> c = ktx2::Ktx2View::open(t->file.span());
    KILN_REQUIRE(c.ok());
    KILN_CHECK(kiln::test::corpus::bytes_equal(c->level_data(0), b->level_data(1)));
}

KILN_TEST(texture_cook, npot_info) {
    u8 rgba[6 * 3 * 4];
    pattern(rgba, sizeof rgba, 17);
    Vec<u8> f = png::encode({.width = 6, .height = 3, .colorType = 6, .depth = 8, .pixels = rgba});
    DiagLog log;
    Result<CookedTexture> r = run_cook(f.span(), kColor, &log);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(log.has(kDiagImageNpotMips, Severity::Info));
}

KILN_TEST(texture_cook, ktx2_passthrough) {
    char const* dir = kiln::test::corpus_dir();
    char path[1024];
    format(path, sizeof path, "%s/khronos/r8g8b8a8_srgb_mip.ktx2", dir);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(kiln::test::corpus::read_file(path, bytes));

    Result<CookedTexture> r = run_cook(bytes.span(), kColor);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->passthrough);
    KILN_CHECK(kiln::test::corpus::bytes_equal(r->file.span(), bytes.span()));
    KILN_CHECK_EQ(r->desc.format, Format::R8G8B8A8_SRGB);
    KILN_CHECK_EQ(r->desc.width, 64u);
    KILN_CHECK_EQ(r->desc.levels, 7u);
    KILN_CHECK_EQ(r->sourceHash, xxh64(bytes.span()));

    // Too large for the cap: pass-through cannot downscale.
    TextureCookSettings s = kColor;
    s.maxSize             = 32;
    DiagLog log;
    Result<CookedTexture> big = run_cook(bytes.span(), s, &log);
    KILN_CHECK_EQ(big.code(), Code::Unsupported);
    KILN_CHECK(log.has(kDiagImagePassthroughBad, Severity::Error));
}

// A cube or array KTX2 passes through with its shape; the target caps the array layers.
KILN_TEST(texture_cook, ktx2_cube_and_array_passthrough) {
    char const* dir = kiln::test::corpus_dir();
    char path[1024];
    Vec<u8> cube(default_allocator(), Tag::Test), array(default_allocator(), Tag::Test);
    format(path, sizeof path, "%s/generated/cube_rgba8_srgb_mip.ktx2", dir);
    KILN_REQUIRE(kiln::test::corpus::read_file(path, cube));
    format(path, sizeof path, "%s/khronos/r8g8b8a8_srgb_array_7_mip.ktx2", dir);
    KILN_REQUIRE(kiln::test::corpus::read_file(path, array));

    Result<CookedTexture> c = run_cook(cube.span(), kColor);
    KILN_REQUIRE(c.ok());
    KILN_CHECK(c->passthrough && c->desc.isCube && c->desc.faces == 6);
    Result<CookedTexture> a = run_cook(array.span(), kColor);
    KILN_REQUIRE(a.ok());
    KILN_CHECK(a->passthrough && a->desc.isArray && a->desc.layers == 7);

    TargetProfile small;
    small.maxArrayLayers = 4;
    DiagLog log;
    Result<CookedTexture> over = run_cook(array.span(), kColor, &log, small);
    KILN_CHECK_EQ(over.code(), Code::Unsupported);
    KILN_CHECK(log.has(kDiagImagePassthroughBad, Severity::Error));
}

KILN_TEST(texture_cook, ktx2_zstd_rejected) {
    char const* dir = kiln::test::corpus_dir();
    char path[1024];
    format(path, sizeof path, "%s/generated/rgba8_srgb_mip_zstd.ktx2", dir);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(kiln::test::corpus::read_file(path, bytes));
    DiagLog log;
    Result<CookedTexture> r = run_cook(bytes.span(), kColor, &log);
    KILN_CHECK_EQ(r.code(), Code::Unsupported);
    KILN_CHECK(log.has(kDiagImagePassthroughBad, Severity::Error));
}

KILN_TEST(texture_cook, jpeg_color) {
    Result<CookedTexture> r = run_cook(img::kJpegGradientRgbBytes, kColor);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r->passthrough);
    KILN_CHECK_EQ(r->desc.format, Format::R8G8B8A8_SRGB);
    KILN_CHECK_EQ(r->desc.width, u32(img::kWJpegGradientRgb));
    KILN_CHECK_EQ(r->desc.height, u32(img::kHJpegGradientRgb));
    KILN_REQUIRE(check_file(*r, "jpeg_color"));
}

KILN_TEST(texture_cook, jpeg_normal_warns_lossy_source) {
    DiagLog log;
    Result<CookedTexture> r = run_cook(
        img::kJpegGradientRgbBytes, {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal}, &log);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(log.has(kDiagImageLossySource, Severity::Warning));
}

KILN_TEST(texture_cook, png_normal_does_not_warn_lossy_source) {
    u8 rgba[7 * 5 * 4];
    pattern(rgba, sizeof rgba, 11);
    Vec<u8> f = png::encode({.width = 7, .height = 5, .colorType = 6, .depth = 8, .pixels = rgba});
    DiagLog log;
    Result<CookedTexture> r =
        run_cook(f.span(), {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal}, &log);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!log.has(kDiagImageLossySource, Severity::Warning));
}

KILN_TEST(texture_cook, unknown_format) {
    u8 const junk[32] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    DiagLog log;
    Result<CookedTexture> r = run_cook(junk, kColor, &log);
    KILN_CHECK_EQ(r.code(), Code::Unsupported);
    KILN_CHECK(log.has(kDiagImageUnknownFormat, Severity::Error));
}

KILN_TEST(texture_cook, deterministic) {
    u8 rgba[13 * 7 * 4];
    pattern(rgba, sizeof rgba, 18);
    Vec<u8> f = png::encode({.width = 13, .height = 7, .colorType = 6, .depth = 8, .pixels = rgba});
    for (TextureUsage u : {TextureUsage::Color, TextureUsage::Normal}) {
        TextureCookSettings s   = {.colorSpace = color_space_for(u), .usage = u};
        Result<CookedTexture> a = run_cook(f.span(), s);
        Result<CookedTexture> b = run_cook(f.span(), s);
        KILN_REQUIRE(a.ok() && b.ok());
        KILN_CHECK(kiln::test::corpus::bytes_equal(a->file.span(), b->file.span()));
    }
}

// The row-band split must not change a byte: a texture large enough to split, cooked
// with and without a pool, as color (sRGB mips) and as a normal map (flip + renormalize).
KILN_TEST(texture_cook, threads_byte_identical) {
    constexpr u32 kW = 1030, kH = 700;
    Vec<u8> rgba(default_allocator(), Tag::Test);
    rgba.resize(usize(kW) * kH * 4);
    pattern(rgba.data(), rgba.size(), 21);
    Vec<u8> f = png::encode({.width = kW, .height = kH, .colorType = 6, .depth = 8, .pixels = rgba.span()});

    constexpr TextureCookSettings kCases[] = {
        {.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Color},
        {.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Normal, .flipGreen = true},
    };
    Result<JobSystem> pool = create_thread_pool({.threads = 4});
    KILN_REQUIRE(pool.ok());
    for (TextureCookSettings const& s : kCases) {
        Result<CookedTexture> single   = run_cook(f.span(), s);
        Result<CookedTexture> threaded = run_cook(f.span(), s, nullptr, {}, &*pool);
        KILN_REQUIRE(single.ok() && threaded.ok());
        KILN_CHECK(single->file.size() == threaded->file.size() &&
                   std::memcmp(single->file.data(), threaded->file.data(), single->file.size()) == 0);
    }
    destroy_thread_pool(*pool);
}

// ---------------------------------------------------------------------------
// Strips: cube and array textures from one image (docs/design/texture-shapes.md)
// ---------------------------------------------------------------------------

namespace {

/// A vertical strip of `count` slices of w x h; slice i is filled with (40i, 255 - 40i, i, 255).
Vec<u8> strip_png(u32 w, u32 h, u32 count) {
    Vec<u8> rgba(default_allocator(), Tag::Test);
    rgba.resize(usize(w) * h * count * 4);
    for (u32 s = 0; s < count; ++s)
        for (usize p = 0; p < usize(w) * h; ++p) {
            u8* px = rgba.data() + (usize(s) * w * h + p) * 4;
            px[0]  = u8(40 * s);
            px[1]  = u8(255 - 40 * s);
            px[2]  = u8(s);
            px[3]  = 255;
        }
    return png::encode({.width = w, .height = h * count, .colorType = 6, .depth = 8, .pixels = rgba.span()});
}

/// True if every texel of `slice` in `level` has the color of strip slice `slice`.
bool slice_is(ktx2::Ktx2View const& v, u32 level, u32 slice) {
    Span<u8 const> const data = v.level_data(level);
    usize const bytes         = usize(v.level_image_bytes(level));
    for (usize i = 0; i < bytes; i += 4) {
        u8 const* px = data.data + usize(slice) * bytes + i;
        if (px[0] != u8(40 * slice) || px[1] != u8(255 - 40 * slice) || px[2] != u8(slice)) return false;
    }
    return true;
}

} // namespace

KILN_TEST(texture_cook, cube_strip) {
    Vec<u8> const f         = strip_png(4, 4, 6);
    TextureCookSettings s   = kColor;
    s.shape                 = CookShape::Cube;
    Result<CookedTexture> r = run_cook(f.span(), s);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->desc.isCube && r->desc.faces == 6 && r->desc.width == 4 && r->desc.height == 4);
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->desc().levels, 3u);
    for (u32 level = 0; level < 3; ++level)
        for (u32 face = 0; face < 6; ++face)
            KILN_CHECK_MSG(slice_is(*v, level, face), "level %u face %u", level, face);
    write_sample("cube_strip", r->file.span());

    // The size cap applies to each face.
    s.maxSize                    = 2;
    Result<CookedTexture> capped = run_cook(f.span(), s);
    KILN_REQUIRE(capped.ok());
    KILN_CHECK(capped->desc.width == 2 && capped->desc.height == 2 && capped->desc.faces == 6);
}

KILN_TEST(texture_cook, array_strip) {
    // Square slices by default: 4x12 is 3 layers.
    TextureCookSettings s   = kColor;
    s.shape                 = CookShape::Array;
    Result<CookedTexture> r = run_cook(strip_png(4, 4, 3).span(), s);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->desc.isArray && r->desc.layers == 3 && r->desc.height == 4);
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    for (u32 layer = 0; layer < 3; ++layer)
        KILN_CHECK(slice_is(*v, 0, layer));
    write_sample("array_strip", r->file.span());

    // An explicit count: 4x8 as 4 layers of 4x2.
    s.slices                 = 4;
    Result<CookedTexture> r4 = run_cook(strip_png(4, 2, 4).span(), s);
    KILN_REQUIRE(r4.ok());
    KILN_CHECK(r4->desc.layers == 4 && r4->desc.width == 4 && r4->desc.height == 2);
}

KILN_TEST(texture_cook, strip_layout_errors) {
    struct Case {
        CookShape shape;
        u32 slices;
        u32 w, h, count;
        u32 maxLayers;
    };
    Case const cases[] = {
        {CookShape::Cube,  0, 4, 4, 5, 2048}, // 5 faces
        {CookShape::Cube,  0, 4, 2, 6, 2048}, // faces not square
        {CookShape::Array, 0, 4, 5, 2, 2048}, // 4x10 is not square slices
        {CookShape::Array, 3, 4, 2, 4, 2048}, // 8 rows do not divide into 3
        {CookShape::Array, 0, 4, 4, 3, 2   }, // 3 layers over the limit
    };
    for (Case const& c : cases) {
        TextureCookSettings s = kColor;
        s.shape               = c.shape;
        s.slices              = c.slices;
        TargetProfile target;
        target.maxArrayLayers = c.maxLayers;
        DiagLog log;
        Result<CookedTexture> r = run_cook(strip_png(c.w, c.h, c.count).span(), s, &log, target);
        KILN_CHECK_EQ(r.code(), Code::InvalidArgument);
        KILN_CHECK(log.has(kDiagImageSliceLayout, Severity::Error));
    }
}

// A KTX2 source keeps its shape; asking for another one is an error, not a reshape.
KILN_TEST(texture_cook, ktx2_shape_must_match) {
    char path[1024];
    format(path, sizeof path, "%s/khronos/r8g8b8a8_srgb_array_7_mip.ktx2", kiln::test::corpus_dir());
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(kiln::test::corpus::read_file(path, bytes));
    TextureCookSettings s = kColor;
    s.shape               = CookShape::Array;
    KILN_CHECK(run_cook(bytes.span(), s).ok());
    s.shape = CookShape::Cube;
    DiagLog log;
    KILN_CHECK_EQ(run_cook(bytes.span(), s, &log).code(), Code::InvalidArgument);
    KILN_CHECK(log.has(kDiagImagePassthroughBad, Severity::Error));
}
