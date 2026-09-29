// tests/test_texture_bc.cpp — BC encoding in the texture cooker: settings, usage table, quality
// floors (decoded with bcdec, independent of the encoders), shapes and thread invariance; cook-only.
// Outputs are written as <sample_dir>/cooked_bc_<case>.ktx2 for `ktx validate`.
#include "hdr_writer.h"
#include "kiln_test.h"
#include "ktx2_corpus.h" // texels
#include "png_writer.h"

#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/cook/sidecar.h"
#include "kiln/io.h"
#include "kiln/ktx2.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace kiln;
using namespace kiln::cook;
namespace png = kiln::test::png;

namespace {

struct DiagLog {
    u32 codes[16]    = {};
    Severity sev[16] = {};
    int count        = 0;
    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLog*>(user);
        if (self->count < 16) {
            self->codes[self->count] = d.code;
            self->sev[self->count]   = d.severity;
        }
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
    [[nodiscard]] bool has(u32 code, Severity s) const {
        for (int i = 0; i < count && i < 16; ++i)
            if (codes[i] == code && sev[i] == s) return true;
        return false;
    }
};

TargetProfile const kBc{.blockFamily = BlockFamily::BC};

/// Smooth content with some structure: what BC encoders see in real textures, unlike noise.
Vec<u8> smooth_rgba(u32 w, u32 h) {
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = px.data() + (usize(y) * w + x) * 4;
            p[0]  = u8(x * 255 / max(w - 1, 1u));
            p[1]  = u8(y * 255 / max(h - 1, 1u));
            p[2]  = u8(128.0 + 100.0 * std::sin(double(x + 2 * y) * 0.15));
            p[3]  = u8(255 - (x + y) * 127 / max(w + h - 2, 1u));
        }
    return px;
}

/// A normal map of a smooth bump field, encoded as (n * 0.5 + 0.5) * 255.
Vec<u8> normal_rgba(u32 w, u32 h) {
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            double const dx = 0.6 * std::cos(double(x) * 0.2), dy = 0.6 * std::cos(double(y) * 0.13);
            double const l = std::sqrt(dx * dx + dy * dy + 1.0);
            u8* p          = px.data() + (usize(y) * w + x) * 4;
            p[0]           = u8(std::lround((-dx / l * 0.5 + 0.5) * 255.0));
            p[1]           = u8(std::lround((-dy / l * 0.5 + 0.5) * 255.0));
            p[2]           = u8(std::lround((1.0 / l * 0.5 + 0.5) * 255.0));
            p[3]           = 255;
        }
    return px;
}

Vec<u8> encode_png(u32 w, u32 h, u8 colorType, Span<u8 const> px) {
    return png::encode({.width = w, .height = h, .colorType = colorType, .depth = 8, .pixels = px});
}

Result<CookedTexture> cook_bc(Span<u8 const> file, TextureCookSettings const& s, DiagLog* log = nullptr,
                              TargetProfile const& target = kBc, JobSystem const* jobs = nullptr) {
    DiagSink sink = log ? log->sink() : DiagSink{};
    Result<TextureCookSettings> rs =
        resolve_texture(s, SlotHint::None, target, CookSession{}, log ? &sink : nullptr, "test/bc");
    if (rs.failed()) return rs.status();
    return cook_texture({.bytes = file, .assetPath = "test/bc", .sourcePath = "bc.png"}, *rs, target,
                        {.diag = log ? &sink : nullptr, .jobs = jobs});
}

void write_sample(char const* name, Span<u8 const> bytes) {
    char path[1024];
    format(path, sizeof path, "%s/cooked_bc_%s.ktx2", kiln::test::sample_dir(), name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", path)) return;
    KILN_CHECK(std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size);
    std::fclose(f);
}

/// Decodes level 0 of a BC file to RGBA8 (BC4: R; BC5: R, G).
bool decode_level0(ktx2::Ktx2View const& v, Vec<u8>& out) {
    ktx2::TextureDesc const d = v.desc();
    Vec<u8> const texels      = kiln::test::corpus::texels(v, 0);
    Span<u8 const> const data = texels.span();
    u32 const bw = (d.width + 3) / 4, bh = (d.height + 3) / 4;
    out.resize(usize(bw) * 4 * bh * 4 * 4);
    usize const blockBytes = (d.format == Format::BC1_RGB_UNORM || d.format == Format::BC1_RGB_SRGB ||
                              d.format == Format::BC4_UNORM)
                                 ? 8
                                 : 16;
    if (data.size < usize(bw) * bh * blockBytes) return false;
    u8 px[64], one[32];
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            u8 const* b = data.data + (usize(by) * bw + bx) * blockBytes;
            std::memset(px, 0, sizeof px);
            switch (d.format) {
            case Format::BC1_RGB_UNORM:
            case Format::BC1_RGB_SRGB: bcdec_bc1(b, px, 16); break;
            case Format::BC3_UNORM:
            case Format::BC3_SRGB: bcdec_bc3(b, px, 16); break;
            case Format::BC4_UNORM:
                bcdec_bc4(b, one, 4);
                for (u32 i = 0; i < 16; ++i)
                    px[i * 4] = one[i];
                break;
            case Format::BC5_UNORM:
                bcdec_bc5(b, one, 8);
                for (u32 i = 0; i < 16; ++i) {
                    px[i * 4]     = one[i * 2];
                    px[i * 4 + 1] = one[i * 2 + 1];
                }
                break;
            case Format::BC7_UNORM:
            case Format::BC7_SRGB: bcdec_bc7(b, px, 16); break;
            default: return false;
            }
            for (u32 y = 0; y < 4; ++y)
                std::memcpy(out.data() + ((usize(by) * 4 + y) * bw * 4 + bx * 4) * 4, px + y * 16, 16);
        }
    return true;
}

/// PSNR over `channels` channels of level 0 against the source RGBA8.
double level0_psnr(CookedTexture const& t, Span<u8 const> src, u32 w, u32 h, u32 firstChannel, u32 channels) {
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(t.file.span());
    if (!KILN_CHECK(v.ok())) return 0;
    Vec<u8> dec(default_allocator(), Tag::Test);
    if (!KILN_CHECK(decode_level0(*v, dec))) return 0;
    u32 const stride = (w + 3) / 4 * 4;
    double sse       = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            for (u32 c = firstChannel; c < firstChannel + channels; ++c) {
                double const e =
                    double(dec[(usize(y) * stride + x) * 4 + c]) - double(src[(usize(y) * w + x) * 4 + c]);
                sse += e * e;
            }
    if (sse == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * double(w) * h * channels / sse);
}

} // namespace

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

KILN_TEST(TextureBc, ResolveEncoding) {
    TargetProfile const none{.blockFamily = BlockFamily::None};
    DiagLog log;
    DiagSink sink = log.sink();

    // Without a block family an explicit BC encoding becomes Uncompressed (K3003); Auto stays Auto.
    Result<TextureCookSettings> r =
        resolve_texture({.usage = TextureUsage::Color, .encoding = TextureEncoding::BC7}, SlotHint::None,
                        none, CookSession{}, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->encoding == TextureEncoding::Uncompressed);
    KILN_CHECK(log.has(kDiagSettingsClampedByTarget, Severity::Warning));
    r = resolve_texture({.usage = TextureUsage::Color, .quality = EncodeQuality::High}, SlotHint::None, none,
                        CookSession{});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->encoding == TextureEncoding::Auto);
    KILN_CHECK(r->quality == EncodeQuality::Normal); // nothing is encoded on this target

    // With BC: quality kept, fastPreview forces Fast.
    r = resolve_texture({.usage = TextureUsage::Color, .quality = EncodeQuality::High}, SlotHint::None, kBc,
                        CookSession{});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->quality == EncodeQuality::High);
    r = resolve_texture({.usage = TextureUsage::Color}, SlotHint::None, kBc,
                        CookSession{.fastPreview = true});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->quality == EncodeQuality::Fast);

    struct Bad {
        TextureCookSettings s;
        Code want;
        u32 diag;
    };
    Bad const bad[] = {
        {{.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Hdr, .encoding = TextureEncoding::BC6H},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.usage = TextureUsage::Color, .encoding = TextureEncoding::BC6H},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.usage = TextureUsage::Color, .encoding = TextureEncoding::BC4},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.usage = TextureUsage::Lut, .encoding = TextureEncoding::BC7},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.usage = TextureUsage::Hdr, .encoding = TextureEncoding::BC7},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.colorSpace = ColorSpace::Srgb, .usage = TextureUsage::Mask, .encoding = TextureEncoding::BC5},
         Code::InvalidArgument,
         kDiagSettingsInvalidCombo},
        {{.usage = TextureUsage::Color, .encoding = TextureEncoding(99)},
         Code::InvalidArgument,
         kDiagSettingsEnumRange   },
        {{.usage = TextureUsage::Color, .quality = EncodeQuality(9)},
         Code::InvalidArgument,
         kDiagSettingsEnumRange   },
    };
    for (Bad const& b : bad) {
        DiagLog l;
        DiagSink ls                         = l.sink();
        Result<TextureCookSettings> const e = resolve_texture(b.s, SlotHint::None, kBc, CookSession{}, &ls);
        KILN_CHECK_EQ(e.code(), b.want);
        KILN_CHECK(l.has(b.diag, Severity::Error));
    }
}

KILN_TEST(TextureBc, HashAndSidecar) {
    TextureCookSettings const base{.usage = TextureUsage::Color};
    u64 const h0          = hash_settings(base);
    TextureCookSettings s = base;
    s.encoding            = TextureEncoding::BC1; // value 2
    TextureCookSettings q = base;
    q.quality             = EncodeQuality::High; // value 2: the tags keep the two apart
    KILN_CHECK(hash_settings(s) != h0 && hash_settings(q) != h0 && hash_settings(s) != hash_settings(q));
    KILN_CHECK(hash_target(kBc) != hash_target(TargetProfile{.blockFamily = BlockFamily::None}));
    KILN_CHECK(TargetProfile{}.blockFamily == BlockFamily::BC); // the desktop default

    TextureCookSettings t;
    KILN_REQUIRE(apply_sidecar("encoding = \"bc5\"\nquality = \"high\"\n", &t).ok());
    KILN_CHECK(t.encoding == TextureEncoding::BC5 && t.quality == EncodeQuality::High);
    KILN_CHECK_EQ(apply_sidecar("encoding = \"dxt5\"\n", &t).code, Code::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Cooking
// ---------------------------------------------------------------------------

KILN_TEST(TextureBc, UsageTable) {
    constexpr u32 kW = 64, kH = 32;
    Vec<u8> const rgba   = smooth_rgba(kW, kH);
    Vec<u8> const normal = normal_rgba(kW, kH);
    Vec<u8> gray(default_allocator(), Tag::Test), grayAlpha(default_allocator(), Tag::Test);
    for (usize i = 0; i < usize(kW) * kH; ++i) {
        gray.push_back(rgba[i * 4 + 1]);
        grayAlpha.push_back(rgba[i * 4]);
        grayAlpha.push_back(rgba[i * 4 + 1]);
    }
    Vec<u8> const rgbaPng   = encode_png(kW, kH, 6, rgba.span());
    Vec<u8> const normalPng = encode_png(kW, kH, 6, normal.span());
    Vec<u8> const grayPng   = encode_png(kW, kH, 0, gray.span());
    Vec<u8> const gaPng     = encode_png(kW, kH, 4, grayAlpha.span());

    struct Case {
        char const* name;
        Span<u8 const> file;
        TextureUsage usage;
        Format want;
    };
    Case const cases[] = {
        {"color",   rgbaPng.span(),   TextureUsage::Color,  Format::BC7_SRGB      },
        {"ui",      rgbaPng.span(),   TextureUsage::Ui,     Format::BC7_SRGB      },
        {"orm",     rgbaPng.span(),   TextureUsage::Orm,    Format::BC7_UNORM     },
        {"normal",  normalPng.span(), TextureUsage::Normal, Format::BC5_UNORM     },
        {"mask",    grayPng.span(),   TextureUsage::Mask,   Format::BC4_UNORM     },
        {"mask_rg", gaPng.span(),     TextureUsage::Mask,   Format::BC5_UNORM     },
        {"lut",     rgbaPng.span(),   TextureUsage::Lut,    Format::R8G8B8A8_UNORM},
        {"height",  grayPng.span(),   TextureUsage::Height, Format::R8_UNORM      },
    };
    for (Case const& c : cases) {
        Result<CookedTexture> r = cook_bc(c.file, {.usage = c.usage});
        if (!KILN_CHECK_MSG(r.ok(), "%s: cook failed", c.name)) continue;
        KILN_CHECK_MSG(r->desc.format == c.want, "%s: got %s, want %s", c.name, format_name(r->desc.format),
                       format_name(c.want));
        KILN_CHECK(r->desc.levels == 7);
        Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
        KILN_REQUIRE(v.ok());
        KILN_CHECK(v->has_all_level_data());
        KILN_CHECK((r->stats.encodeUs > 0 || !format_info(c.want)->compressed));
        if (format_info(c.want)->compressed) write_sample(c.name, r->file.span());
    }
}

KILN_TEST(TextureBc, QualityFloors) {
    constexpr u32 kW = 64, kH = 64;
    Vec<u8> const rgba   = smooth_rgba(kW, kH);
    Vec<u8> const normal = normal_rgba(kW, kH);
    Vec<u8> const png    = encode_png(kW, kH, 6, rgba.span());
    Vec<u8> const npng   = encode_png(kW, kH, 6, normal.span());

    TextureCookSettings const color{.colorSpace = ColorSpace::Linear, .usage = TextureUsage::Color};
    TextureCookSettings fast = color, bc1 = color, bc3 = color;
    fast.quality = EncodeQuality::Fast;
    bc1.encoding = TextureEncoding::BC1;
    bc3.encoding = TextureEncoding::BC3;
    TextureCookSettings const nrm{.usage = TextureUsage::Normal};
    TextureCookSettings bc7n = nrm;
    bc7n.encoding            = TextureEncoding::BC7;
    TextureCookSettings const mask{.usage = TextureUsage::Mask};

    struct Case {
        char const* name;
        TextureCookSettings const* s;
        bool normalSource;
        u32 channels; ///< compared from channel 0
        double floor;
    };
    Case const cases[] = {
        {"bc7",        &color, false, 4, 38},
        {"bc7_fast",   &fast,  false, 4, 38},
        {"bc1",        &bc1,   false, 3, 33},
        {"bc3",        &bc3,   false, 4, 34},
        {"bc5_normal", &nrm,   true,  2, 40},
        {"bc7_normal", &bc7n,  true,  3, 40},
        {"bc4_mask",   &mask,  false, 1, 44},
    };
    for (Case const& c : cases) {
        Result<CookedTexture> r = cook_bc(c.normalSource ? npng.span() : png.span(), *c.s);
        if (!KILN_CHECK_MSG(r.ok(), "%s: cook failed", c.name)) continue;
        double const p = level0_psnr(*r, c.normalSource ? normal.span() : rgba.span(), kW, kH, 0, c.channels);
        KILN_CHECK_MSG(p >= c.floor, "%s (%s): PSNR %.2f dB below %.0f dB", c.name,
                       format_name(r->desc.format), p, c.floor);
    }
}

// Mip levels below 4x4 are one block; a cube encodes each face; threads never change the bytes.
KILN_TEST(TextureBc, ShapesAndThreads) {
    Vec<u8> const small     = smooth_rgba(7, 5);
    Vec<u8> const png       = encode_png(7, 5, 6, small.span());
    Result<CookedTexture> r = cook_bc(png.span(), {.usage = TextureUsage::Color});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->desc.format == Format::BC7_SRGB && r->desc.width == 7 && r->desc.height == 5 &&
               r->desc.levels == 3);
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(r->file.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->levels()[0].uncompressedByteLength, u64(2 * 2 * 16));
    KILN_CHECK_EQ(v->levels()[1].uncompressedByteLength, u64(16));
    KILN_CHECK_EQ(v->levels()[2].uncompressedByteLength, u64(16));
    write_sample("npot", r->file.span());

    Vec<u8> const strip = smooth_rgba(16, 96);
    Vec<u8> const spng  = encode_png(16, 96, 6, strip.span());
    Result<CookedTexture> cube =
        cook_bc(spng.span(), {.usage = TextureUsage::Normal, .shape = CookShape::Cube});
    KILN_REQUIRE(cube.ok());
    KILN_CHECK(cube->desc.isCube && cube->desc.format == Format::BC5_UNORM && cube->desc.levels == 5);
    Result<ktx2::Ktx2View> cv = ktx2::Ktx2View::open(cube->file.span());
    KILN_REQUIRE(cv.ok());
    KILN_CHECK_EQ(cv->levels()[0].uncompressedByteLength, u64(4 * 4 * 16 * 6));
    write_sample("cube", cube->file.span());

    Vec<u8> const big      = smooth_rgba(256, 128);
    Vec<u8> const bpng     = encode_png(256, 128, 6, big.span());
    Result<JobSystem> pool = create_thread_pool({.threads = 4});
    KILN_REQUIRE(pool.ok());
    for (TextureUsage const u : {TextureUsage::Color, TextureUsage::Normal, TextureUsage::Mask}) {
        Result<CookedTexture> single   = cook_bc(bpng.span(), {.usage = u});
        Result<CookedTexture> threaded = cook_bc(bpng.span(), {.usage = u}, nullptr, kBc, &*pool);
        KILN_REQUIRE(single.ok() && threaded.ok());
        KILN_CHECK(single->file.size() == threaded->file.size() &&
                   std::memcmp(single->file.data(), threaded->file.data(), single->file.size()) == 0);
    }
    destroy_thread_pool(*pool);
}

// A cook without the BC family stays uncompressed, byte for byte as before.
KILN_TEST(TextureBc, NoFamilyIsUncompressed) {
    Vec<u8> const rgba      = smooth_rgba(16, 16);
    Vec<u8> const png       = encode_png(16, 16, 6, rgba.span());
    Result<CookedTexture> r = cook_bc(png.span(), {.usage = TextureUsage::Color}, nullptr,
                                      TargetProfile{.blockFamily = BlockFamily::None});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->desc.format == Format::R8G8B8A8_SRGB && r->stats.encodeUs == 0);
}

// HDR: BC6H UFLOAT against the RGBA16F cook of the same source, in log2 space.
KILN_TEST(TextureBc, Hdr) {
    constexpr u32 kW = 32, kH = 16;
    Vec<u8> rgbe(default_allocator(), Tag::Test);
    rgbe.resize(usize(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            u8* p = rgbe.data() + (usize(y) * kW + x) * 4;
            p[0]  = u8(40 + x * 6);
            p[1]  = u8(60 + y * 9);
            p[2]  = u8(200 - x * 4);
            p[3]  = u8(128 + y / 4); // one exponent per block row: 2^0 to 2^3
        }
    Vec<u8> const file = kiln::test::hdr::encode_flat(kW, kH, rgbe.span());

    Result<CookedTexture> bc = cook_bc(file.span(), {.usage = TextureUsage::Hdr});
    KILN_REQUIRE(bc.ok());
    KILN_CHECK(bc->desc.format == Format::BC6H_UFLOAT && bc->desc.levels == 6);
    write_sample("hdr", bc->file.span());
    Result<CookedTexture> half = cook_bc(file.span(), {.usage = TextureUsage::Hdr}, nullptr,
                                         TargetProfile{.blockFamily = BlockFamily::None});
    KILN_REQUIRE(half.ok());
    KILN_CHECK(half->desc.format == Format::R16G16B16A16_SFLOAT);

    Result<ktx2::Ktx2View> vb = ktx2::Ktx2View::open(bc->file.span());
    Result<ktx2::Ktx2View> vh = ktx2::Ktx2View::open(half->file.span());
    KILN_REQUIRE(vb.ok() && vh.ok());
    Vec<u8> const tb            = kiln::test::corpus::texels(*vb, 0);
    Vec<u8> const th            = kiln::test::corpus::texels(*vh, 0);
    Span<u8 const> const blocks = tb.span();
    Span<u8 const> const ref    = th.span();
    KILN_REQUIRE(blocks.size == usize(kW / 4) * (kH / 4) * 16 && ref.size == usize(kW) * kH * 8);
    double sq = 0;
    float px[48];
    for (u32 by = 0; by < kH / 4; ++by)
        for (u32 bx = 0; bx < kW / 4; ++bx) {
            bcdec_bc6h_float(blocks.data + (usize(by) * (kW / 4) + bx) * 16, px, 12, 0);
            for (u32 i = 0; i < 16; ++i)
                for (u32 c = 0; c < 3; ++c) {
                    u32 const x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    u16 h;
                    std::memcpy(&h, ref.data + (usize(y) * kW + x) * 8 + c * 2, 2);
                    double const e =
                        std::log2(1.0 + double(px[i * 3 + c])) - std::log2(1.0 + double(half_to_float(h)));
                    sq += e * e;
                }
        }
    double const rmse = std::sqrt(sq / (kW * kH * 3));
    KILN_CHECK_MSG(rmse < 0.02, "BC6H log2 RMSE %.4f", rmse);
}
