// tests/test_image.cpp — cook-side image pipeline; decode_png input comes from png_writer.h.
#include "kiln_test.h"
#include "png_writer.h"

#include "kiln/containers.h"
#include "kiln/cook/image.h"

using namespace kiln;
using namespace kiln::cook;
namespace png = kiln::test::png;

namespace {

Image make(u32 w, u32 h, u32 channels, u32 bits, Span<u8 const> bytes) {
    Image img;
    img.width          = w;
    img.height         = h;
    img.channels       = channels;
    img.bitsPerChannel = bits;
    img.pixels.init(default_allocator(), Tag::Test);
    img.pixels.append(bytes);
    return img;
}

Image make16(u32 w, u32 h, u32 channels, Span<u16 const> values) {
    return make(w, h, channels, 16,
                Span<u8 const>(reinterpret_cast<u8 const*>(values.data), values.size * 2));
}

u16 px16(Image const& img, usize index) {
    u16 v;
    std::memcpy(&v, img.pixels.data() + index * 2, 2);
    return v;
}

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
    bool has(u32 code) const {
        for (int i = 0; i < count && i < 16; ++i)
            if (codes[i] == code) return true;
        return false;
    }
};

} // namespace

KILN_TEST(image, srgb_table) {
    KILN_CHECK_EQ(u32(srgb8_to_linear16(0)), 0u);
    KILN_CHECK_EQ(u32(srgb8_to_linear16(255)), 65535u);
    // Independently: ((128/255 + 0.055) / 1.055)^2.4 * 65535 = 14146.4
    KILN_CHECK_EQ(u32(srgb8_to_linear16(128)), 14146u);
    KILN_CHECK_EQ(u32(srgb8_to_linear16(1)), 20u);   // linear segment: 65535 / 255 / 12.92 = 19.9
    KILN_CHECK_EQ(u32(srgb8_to_linear16(10)), 199u); // 10 / 255 / 12.92 * 65535 = 198.9
    KILN_CHECK_EQ(u32(srgb8_to_linear16(188)), 32957u);
    for (u32 v = 1; v < 256; ++v)
        KILN_CHECK_MSG(srgb8_to_linear16(u8(v)) > srgb8_to_linear16(u8(v - 1)), "not monotonic at %u", v);
}

KILN_TEST(image, srgb_round_trip) {
    for (u32 v = 0; v < 256; ++v)
        KILN_CHECK_MSG(linear16_to_srgb8(srgb8_to_linear16(u8(v))) == v, "round trip failed for %u", v);
    // Nearest entry, ties to the lower index (entries 0 and 20 for sRGB 0 and 1).
    KILN_CHECK_EQ(u32(linear16_to_srgb8(10)), 0u);
    KILN_CHECK_EQ(u32(linear16_to_srgb8(11)), 1u);
    KILN_CHECK_EQ(u32(linear16_to_srgb8(65535)), 255u);
    KILN_CHECK_EQ(u32(linear16_to_srgb8(32768)), 188u); // linear mid-gray
    // Exhaustive: the result is always a nearest entry.
    for (u32 v = 0; v < 65536; v += 7) {
        u32 const s = linear16_to_srgb8(u16(v));
        u32 const d =
            v > srgb8_to_linear16(u8(s)) ? v - srgb8_to_linear16(u8(s)) : srgb8_to_linear16(u8(s)) - v;
        u32 const dlo = s > 0 ? v - min(v, u32(srgb8_to_linear16(u8(s - 1)))) : 0xFFFFFFFFu;
        u32 const dhi = s < 255
                            ? u32(srgb8_to_linear16(u8(s + 1))) - min(v, u32(srgb8_to_linear16(u8(s + 1))))
                            : 0xFFFFFFFFu;
        KILN_CHECK_MSG(d <= dlo && d <= dhi, "linear %u -> %u is not nearest", v, s);
    }
}

KILN_TEST(image, convert_gray_to_rgba) {
    u8 const px[2]  = {10, 200};
    Image src       = make(2, 1, 1, 8, px);
    Result<Image> r = convert_image(src, 4, 8, default_allocator());
    KILN_REQUIRE(r.ok());
    u8 const expect[8] = {10, 10, 10, 255, 200, 200, 200, 255};
    KILN_CHECK_EQ(r->pixels.size(), usize(8));
    KILN_CHECK(std::memcmp(r->pixels.data(), expect, 8) == 0);
}

KILN_TEST(image, convert_depth_rules) {
    u16 const v16[4] = {65535, 32768, 257, 128};
    Image src        = make16(4, 1, 1, v16);
    Result<Image> r  = convert_image(src, 1, 8, default_allocator());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(u32(r->pixels[0]), 255u);
    KILN_CHECK_EQ(u32(r->pixels[1]), 128u); // (32768 * 255 + 32767) / 65535
    KILN_CHECK_EQ(u32(r->pixels[2]), 1u);
    KILN_CHECK_EQ(u32(r->pixels[3]), 0u);

    u8 const v8[2]   = {1, 255};
    Image s8         = make(2, 1, 1, 8, v8);
    Result<Image> r2 = convert_image(s8, 1, 16, default_allocator());
    KILN_REQUIRE(r2.ok());
    KILN_CHECK_EQ(u32(px16(*r2, 0)), 257u);
    KILN_CHECK_EQ(u32(px16(*r2, 1)), 65535u);
}

KILN_TEST(image, convert_channels) {
    u8 const rgba[4] = {1, 2, 3, 4};
    Image src        = make(1, 1, 4, 8, rgba);
    Result<Image> rg = convert_image(src, 2, 8, default_allocator());
    KILN_REQUIRE(rg.ok());
    KILN_CHECK_EQ(u32(rg->pixels[0]), 1u);
    KILN_CHECK_EQ(u32(rg->pixels[1]), 2u);
    Result<Image> back = convert_image(*rg, 4, 8, default_allocator());
    KILN_REQUIRE(back.ok());
    u8 const expect[4] = {1, 2, 0, 255};
    KILN_CHECK(std::memcmp(back->pixels.data(), expect, 4) == 0);
    Result<Image> same = convert_image(src, 4, 8, default_allocator());
    KILN_REQUIRE(same.ok());
    KILN_CHECK(std::memcmp(same->pixels.data(), rgba, 4) == 0);

    Result<Image> bad = convert_image(src, 5, 8, default_allocator());
    KILN_CHECK_EQ(bad.code(), Code::InvalidArgument);
}

KILN_TEST(image, downsample_linear_4x4) {
    u8 px[16];
    for (u32 i = 0; i < 16; ++i)
        px[i] = u8(i * 10); // (x, y) = (y * 4 + x) * 10
    Image src       = make(4, 4, 1, 8, px);
    Result<Image> r = downsample_2x(src, {}, default_allocator());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->width, 2u);
    KILN_CHECK_EQ(r->height, 2u);
    KILN_CHECK_EQ(u32(r->pixels[0]), 25u);  // (0 + 10 + 40 + 50 + 2) / 4
    KILN_CHECK_EQ(u32(r->pixels[1]), 45u);  // (20 + 30 + 60 + 70 + 2) / 4
    KILN_CHECK_EQ(u32(r->pixels[2]), 105u); // (80 + 90 + 120 + 130 + 2) / 4
    KILN_CHECK_EQ(u32(r->pixels[3]), 125u); // (100 + 110 + 140 + 150 + 2) / 4
}

KILN_TEST(image, downsample_3x3_1x4_4x1) {
    u8 px[9];
    for (u32 i = 0; i < 9; ++i)
        px[i] = u8(i);
    Image s3         = make(3, 3, 1, 8, px);
    Result<Image> r3 = downsample_2x(s3, {}, default_allocator());
    KILN_REQUIRE(r3.ok());
    KILN_CHECK_EQ(r3->width, 1u);
    KILN_CHECK_EQ(r3->height, 1u);
    KILN_CHECK_EQ(u32(r3->pixels[0]), 2u); // (0 + 1 + 3 + 4 + 2) / 4

    u8 const col[4]  = {10, 20, 30, 41};
    Image tall       = make(1, 4, 1, 8, col);
    Result<Image> rt = downsample_2x(tall, {}, default_allocator());
    KILN_REQUIRE(rt.ok());
    KILN_CHECK_EQ(rt->width, 1u);
    KILN_CHECK_EQ(rt->height, 2u);
    KILN_CHECK_EQ(u32(rt->pixels[0]), 15u); // single column sampled twice: (10+10+20+20+2)/4
    KILN_CHECK_EQ(u32(rt->pixels[1]), 36u); // (30+30+41+41+2)/4

    Image wide       = make(4, 1, 1, 8, col);
    Result<Image> rw = downsample_2x(wide, {}, default_allocator());
    KILN_REQUIRE(rw.ok());
    KILN_CHECK_EQ(rw->width, 2u);
    KILN_CHECK_EQ(rw->height, 1u);
    KILN_CHECK_EQ(u32(rw->pixels[0]), 15u);
    KILN_CHECK_EQ(u32(rw->pixels[1]), 36u);

    u8 const one[1]  = {77};
    Image dot        = make(1, 1, 1, 8, one);
    Result<Image> rd = downsample_2x(dot, {}, default_allocator());
    KILN_REQUIRE(rd.ok());
    KILN_CHECK_EQ(u32(rd->pixels[0]), 77u);
}

KILN_TEST(image, downsample_srgb) {
    // Black/white checker in RGB, alpha 0/255: sRGB-correct average is 188, alpha 128.
    u8 const px[16] = {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255};
    Image src       = make(2, 2, 4, 8, px);
    Result<Image> s = downsample_2x(src, {.srgb = true}, default_allocator());
    KILN_REQUIRE(s.ok());
    KILN_CHECK_EQ(u32(s->pixels[0]), 188u); // linear16 (65535 * 2 + 2) / 4 = 32768 -> sRGB 188
    KILN_CHECK_EQ(u32(s->pixels[1]), 188u);
    KILN_CHECK_EQ(u32(s->pixels[2]), 188u);
    KILN_CHECK_EQ(u32(s->pixels[3]), 128u); // alpha is linear: (510 + 2) / 4
    Result<Image> l = downsample_2x(src, {}, default_allocator());
    KILN_REQUIRE(l.ok());
    KILN_CHECK_EQ(u32(l->pixels[0]), 128u);

    // Uneven: sRGB 10, 100, 200, 250 -> linear (199 + 8352 + 37852 + 62650 + 2) / 4 = 27263 -> 173
    u8 const q[4]    = {10, 100, 200, 250};
    Image g          = make(2, 2, 1, 8, q);
    Result<Image> gs = downsample_2x(g, {.srgb = true}, default_allocator());
    KILN_REQUIRE(gs.ok());
    u32 const lin = (u32(srgb8_to_linear16(10)) + srgb8_to_linear16(100) + srgb8_to_linear16(200) +
                     srgb8_to_linear16(250) + 2) /
                    4;
    KILN_CHECK_EQ(u32(gs->pixels[0]), u32(linear16_to_srgb8(u16(lin))));
    KILN_CHECK_EQ(u32(gs->pixels[0]), 173u);
}

KILN_TEST(image, downsample_16bit) {
    u16 const v[4]  = {65535, 0, 1, 2};
    Image src       = make16(2, 2, 1, v);
    Result<Image> r = downsample_2x(src, {}, default_allocator());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(u32(px16(*r, 0)), 16385u); // (65538 + 2) / 4
}

KILN_TEST(image, renormalize_vectors) {
    // 0 decodes to -1 on every axis: (-1,-1,-1)/sqrt(3) encodes to round(0.42265 * 127.5) = 54.
    u8 px[20] = {0, 0, 0, 9, 128, 128, 255, 9, 200, 200, 128, 9, 10, 20, 30, 9, 255, 128, 128, 9};
    Image img = make(5, 1, 4, 8, px);
    renormalize(img);
    u8 const expect[20] = {54,  54, 54, 9,  128, 128, 255, 9,   218, 218,
                           128, 9,  47, 54, 61,  9,   255, 128, 128, 9};
    for (u32 i = 0; i < 20; ++i)
        KILN_CHECK_MSG(img.pixels[i] == expect[i], "byte %u: %u != %u", i, u32(img.pixels[i]),
                       u32(expect[i]));

    u16 const v16[3] = {0, 0, 0};
    Image n16        = make16(1, 1, 3, v16);
    renormalize(n16);
    KILN_CHECK_EQ(u32(px16(n16, 0)), 13849u);
    KILN_CHECK_EQ(u32(px16(n16, 2)), 13849u);
}

KILN_TEST(image, flip_green) {
    u8 px[8]  = {1, 10, 3, 4, 5, 255, 7, 8};
    Image img = make(2, 1, 4, 8, px);
    flip_green(img);
    KILN_CHECK_EQ(u32(img.pixels[1]), 245u);
    KILN_CHECK_EQ(u32(img.pixels[5]), 0u);
    KILN_CHECK_EQ(u32(img.pixels[0]), 1u);
}

KILN_TEST(image, mip_chain_levels) {
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(7 * 5 * 4, u8(100));
    Result<Vec<Image>> c =
        build_mip_chain(make(7, 5, 4, 8, px.span()), {.srgb = true}, 0, default_allocator());
    KILN_REQUIRE(c.ok());
    KILN_REQUIRE_EQ(c->size(), usize(3));
    KILN_CHECK_EQ((*c)[1].width, 3u);
    KILN_CHECK_EQ((*c)[1].height, 2u);
    KILN_CHECK_EQ((*c)[2].width, 1u);
    KILN_CHECK_EQ((*c)[2].height, 1u);
    KILN_CHECK_EQ(u32((*c)[2].pixels[0]), 100u); // constant image stays constant through sRGB mips

    px.resize(16 * 16 * 4, u8(0));
    Result<Vec<Image>> c16 = build_mip_chain(make(16, 16, 4, 8, px.span()), {}, 0, default_allocator());
    KILN_REQUIRE(c16.ok());
    KILN_CHECK_EQ(c16->size(), usize(5));

    Result<Vec<Image>> capped = build_mip_chain(make(16, 16, 4, 8, px.span()), {}, 2, default_allocator());
    KILN_REQUIRE(capped.ok());
    KILN_CHECK_EQ(capped->size(), usize(2));
}

namespace {

Result<Image> decode(Vec<u8> const& file, DiagLog* log = nullptr) {
    DiagSink sink = log ? log->sink() : DiagSink{};
    return decode_png(file.span(), default_allocator(), &sink, "test.png");
}

/// Deterministic test pattern of `n` bytes.
void pattern(u8* p, usize n, u32 seed) {
    u32 s = seed;
    for (usize i = 0; i < n; ++i) {
        s    = s * 1664525u + 1013904223u;
        p[i] = u8(s >> 24);
    }
}

} // namespace

KILN_TEST(image, png_signature) {
    u8 const sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    KILN_CHECK(is_png(sig));
    KILN_CHECK(!is_ktx2(sig));
    u8 const ktx[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    KILN_CHECK(is_ktx2(ktx));
    KILN_CHECK(!is_png(ktx));
    KILN_CHECK(!is_png(Span<u8 const>(sig, 4)));
}

KILN_TEST(image, png_rgba8_rgb8) {
    u8 rgba[5 * 3 * 4];
    pattern(rgba, sizeof rgba, 1);
    Vec<u8> f       = png::encode({.width = 5, .height = 3, .colorType = 6, .depth = 8, .pixels = rgba});
    Result<Image> r = decode(f);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->width, 5u);
    KILN_CHECK_EQ(r->height, 3u);
    KILN_CHECK_EQ(r->channels, 4u);
    KILN_CHECK_EQ(r->bitsPerChannel, 8u);
    KILN_REQUIRE_EQ(r->pixels.size(), sizeof rgba);
    KILN_CHECK(std::memcmp(r->pixels.data(), rgba, sizeof rgba) == 0);

    u8 rgb[4 * 4 * 3];
    pattern(rgb, sizeof rgb, 2);
    Vec<u8> f3       = png::encode({.width = 4, .height = 4, .colorType = 2, .depth = 8, .pixels = rgb});
    Result<Image> r3 = decode(f3);
    KILN_REQUIRE(r3.ok());
    KILN_CHECK_EQ(r3->channels, 3u);
    KILN_REQUIRE_EQ(r3->pixels.size(), sizeof rgb);
    KILN_CHECK(std::memcmp(r3->pixels.data(), rgb, sizeof rgb) == 0);
}

KILN_TEST(image, png_gray8_gray_alpha8) {
    u8 g[3 * 2];
    pattern(g, sizeof g, 3);
    Vec<u8> f       = png::encode({.width = 3, .height = 2, .colorType = 0, .depth = 8, .pixels = g});
    Result<Image> r = decode(f);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->channels, 1u);
    KILN_CHECK_EQ(r->bitsPerChannel, 8u);
    KILN_REQUIRE_EQ(r->pixels.size(), sizeof g);
    KILN_CHECK(std::memcmp(r->pixels.data(), g, sizeof g) == 0);

    u8 ga[3 * 3 * 2];
    pattern(ga, sizeof ga, 4);
    Vec<u8> f2       = png::encode({.width = 3, .height = 3, .colorType = 4, .depth = 8, .pixels = ga});
    Result<Image> r2 = decode(f2);
    KILN_REQUIRE(r2.ok());
    KILN_CHECK_EQ(r2->channels, 2u);
    KILN_REQUIRE_EQ(r2->pixels.size(), sizeof ga);
    KILN_CHECK(std::memcmp(r2->pixels.data(), ga, sizeof ga) == 0);
}

KILN_TEST(image, png_gray16_rgba16) {
    u16 const g[4] = {0, 1, 0x1234, 65535};
    Vec<u8> gb     = png::be16(g);
    Vec<u8> f      = png::encode({.width = 2, .height = 2, .colorType = 0, .depth = 16, .pixels = gb.span()});
    Result<Image> r = decode(f);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->channels, 1u);
    KILN_CHECK_EQ(r->bitsPerChannel, 16u);
    KILN_REQUIRE_EQ(r->pixels.size(), usize(8));
    for (usize i = 0; i < 4; ++i)
        KILN_CHECK_EQ(u32(px16(*r, i)), u32(g[i]));

    u16 v[3 * 2 * 4];
    for (u32 i = 0; i < 24; ++i)
        v[i] = u16(i * 2731u + 7u);
    Vec<u8> vb = png::be16(v);
    Vec<u8> f2 = png::encode({.width = 3, .height = 2, .colorType = 6, .depth = 16, .pixels = vb.span()});
    Result<Image> r2 = decode(f2);
    KILN_REQUIRE(r2.ok());
    KILN_CHECK_EQ(r2->channels, 4u);
    KILN_CHECK_EQ(r2->bitsPerChannel, 16u);
    KILN_REQUIRE_EQ(r2->pixels.size(), usize(48));
    for (usize i = 0; i < 24; ++i)
        KILN_CHECK_MSG(px16(*r2, i) == v[i], "sample %u: %u != %u", u32(i), u32(px16(*r2, i)), u32(v[i]));
}

KILN_TEST(image, png_palette_trns) {
    u8 const pal[9]  = {255, 0, 0, 0, 255, 0, 0, 0, 255};
    u8 const trns[2] = {0, 128}; // entry 2 stays opaque
    u8 const idx[4]  = {0, 1, 2, 1};
    Vec<u8> f        = png::encode(
        {.width = 2, .height = 2, .colorType = 3, .depth = 8, .pixels = idx, .palette = pal, .trns = trns});
    Result<Image> r = decode(f);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->channels, 4u);
    u8 const expect[16] = {255, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 255, 0, 255, 0, 128};
    KILN_REQUIRE_EQ(r->pixels.size(), usize(16));
    KILN_CHECK(std::memcmp(r->pixels.data(), expect, 16) == 0);

    Vec<u8> f3 =
        png::encode({.width = 2, .height = 2, .colorType = 3, .depth = 8, .pixels = idx, .palette = pal});
    Result<Image> r3 = decode(f3);
    KILN_REQUIRE(r3.ok());
    KILN_CHECK_EQ(r3->channels, 3u);
    KILN_CHECK_EQ(u32(r3->pixels[3 * 2 + 2]), 255u); // index 2 -> blue
}

KILN_TEST(image, png_interlaced) {
    u8 rgba[11 * 9 * 4];
    pattern(rgba, sizeof rgba, 5);
    Vec<u8> f = png::encode(
        {.width = 11, .height = 9, .colorType = 6, .depth = 8, .interlace = true, .pixels = rgba});
    Result<Image> r = decode(f);
    KILN_REQUIRE(r.ok());
    KILN_REQUIRE_EQ(r->pixels.size(), sizeof rgba);
    KILN_CHECK(std::memcmp(r->pixels.data(), rgba, sizeof rgba) == 0);

    u16 g[5 * 6];
    for (u32 i = 0; i < 30; ++i)
        g[i] = u16(i * 2111u);
    Vec<u8> gb = png::be16(g);
    Vec<u8> f2 = png::encode(
        {.width = 5, .height = 6, .colorType = 0, .depth = 16, .interlace = true, .pixels = gb.span()});
    Result<Image> r2 = decode(f2);
    KILN_REQUIRE(r2.ok());
    KILN_CHECK_EQ(r2->channels, 1u);
    KILN_CHECK_EQ(r2->bitsPerChannel, 16u);
    for (usize i = 0; i < 30; ++i)
        KILN_CHECK_EQ(u32(px16(*r2, i)), u32(g[i]));
}

KILN_TEST(image, png_truncated_and_bad) {
    u8 rgba[8 * 8 * 4];
    pattern(rgba, sizeof rgba, 6);
    Vec<u8> f = png::encode({.width = 8, .height = 8, .colorType = 6, .depth = 8, .pixels = rgba});
    KILN_REQUIRE(f.size() > 100);

    Vec<u8> cut(default_allocator(), Tag::Test);
    cut.append(Span<u8 const>(f.data(), f.size() - 100));
    DiagLog log;
    Result<Image> r = decode(cut, &log);
    KILN_CHECK_EQ(r.code(), Code::ParseError);
    KILN_CHECK(log.has(kDiagImageDecodeFailed));

    Vec<u8> head(default_allocator(), Tag::Test);
    head.append(Span<u8 const>(f.data(), 20));
    DiagLog log2;
    Result<Image> r2 = decode(head, &log2);
    KILN_CHECK_EQ(r2.code(), Code::ParseError);
    KILN_CHECK(log2.has(kDiagImageDecodeFailed));

    DiagLog log3;
    DiagSink const sink3 = log3.sink();
    u8 const junk[4]     = {1, 2, 3, 4};
    Result<Image> r3     = decode_png(junk, default_allocator(), &sink3);
    KILN_CHECK_EQ(r3.code(), Code::ParseError);

    // Oversized extent (IHDR says 20000 wide) -> K2008 before any decoding.
    Vec<u8> big = f.clone();
    big[16]     = 0;
    big[17]     = 0;
    big[18]     = 0x4E;
    big[19]     = 0x20;
    DiagLog log4;
    Result<Image> r4 = decode(big, &log4);
    KILN_CHECK_EQ(r4.code(), Code::Unsupported);
    KILN_CHECK(log4.has(kDiagImageTooLarge));
}

// ---------------------------------------------------------------------------
// Kernel equivalence: the specialized kernels against slow references
// ---------------------------------------------------------------------------

namespace {

/// The pre-table linear16_to_srgb8: binary search for the smallest entry >= v, then
/// the nearer of it and its predecessor (ties to the lower index).
u8 slow_linear16_to_srgb8(u16 v) {
    u32 lo = 0, hi = 255;
    while (lo < hi) {
        u32 const mid = (lo + hi) / 2;
        if (srgb8_to_linear16(u8(mid)) < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0 && u32(v) - srgb8_to_linear16(u8(lo - 1)) <= u32(srgb8_to_linear16(u8(lo))) - v) --lo;
    return u8(lo);
}

struct Lcg {
    u32 s;
    u32 next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
};

Image random_image(Lcg& rng, u32 w, u32 h, u32 channels, u32 bits) {
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(usize(w) * h * channels * (bits / 8));
    for (usize i = 0; i < px.size(); ++i)
        px[i] = u8(rng.next());
    // Some flat and extreme texels so zero vectors and clamps get exercised.
    if (px.size() >= 8) std::memset(px.data(), rng.next() & 1 ? 0 : 0xFF, 8);
    return make(w, h, channels, bits, px.span());
}

u32 texel(Image const& img, u32 x, u32 y, u32 c) {
    usize const i = (usize(y) * img.width + x) * img.channels + c;
    return img.bitsPerChannel == 8 ? u32(img.pixels[i]) : u32(px16(img, i));
}

void set_texel(Image& img, u32 x, u32 y, u32 c, u32 v) {
    usize const i = (usize(y) * img.width + x) * img.channels + c;
    if (img.bitsPerChannel == 8) {
        img.pixels[i] = u8(v);
    } else {
        u16 const s = u16(v);
        std::memcpy(img.pixels.data() + i * 2, &s, 2);
    }
}

bool same_image(Image const& a, Image const& b) {
    return a.width == b.width && a.height == b.height && a.channels == b.channels &&
           a.bitsPerChannel == b.bitsPerChannel && a.pixels.size() == b.pixels.size() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()) == 0;
}

/// prepare_image's documented meaning, built from the separate operations.
Image reference_prepare(Image const& src, u32 channels, u32 bits, PrepareOptions const& opt) {
    Image out;
    if (opt.grayAlpha && src.channels == 2 && channels >= 3) {
        Result<Image> ga = convert_image(src, 2, bits, default_allocator());
        Result<Image> r  = convert_image(*ga, channels, bits, default_allocator()); // allocation only
        out              = std::move(r).value();
        for (u32 y = 0; y < src.height; ++y)
            for (u32 x = 0; x < src.width; ++x)
                for (u32 c = 0; c < channels; ++c)
                    set_texel(out, x, y, c, texel(*ga, x, y, c == 3 ? 1 : 0));
    } else {
        out = std::move(convert_image(src, channels, bits, default_allocator())).value();
    }
    if (opt.flipGreen) flip_green(out);
    if (opt.renormalize) renormalize(out);
    return out;
}

/// convert_image's rules from image.h, one texel and channel at a time.
Image reference_convert(Image const& src, u32 channels, u32 bits) {
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(usize(src.width) * src.height * channels * (bits / 8));
    Image dst       = make(src.width, src.height, channels, bits, px.span());
    u32 const smax  = src.bitsPerChannel == 8 ? 255u : 65535u;
    auto const conv = [&](u32 v) {
        if (src.bitsPerChannel == 16 && bits == 8) return (v * 255u + 32767u) / 65535u;
        if (src.bitsPerChannel == 8 && bits == 16) return v * 257u;
        return v;
    };
    for (u32 y = 0; y < src.height; ++y)
        for (u32 x = 0; x < src.width; ++x)
            for (u32 c = 0; c < channels; ++c) {
                u32 v = 0;
                if (c < src.channels)
                    v = texel(src, x, y, c);
                else if (c == 3)
                    v = smax;
                else if (src.channels == 1)
                    v = texel(src, x, y, 0);
                set_texel(dst, x, y, c, conv(v));
            }
    return dst;
}

/// Straightforward 2x2 box filter with per-sample clamps (the pre-kernel code).
Image reference_downsample(Image const& src, MipOptions const& opt) {
    u32 const w     = max(src.width / 2, 1u);
    u32 const h     = max(src.height / 2, 1u);
    bool const rn   = opt.renormalize && src.channels >= 3;
    bool const srgb = opt.srgb && src.bitsPerChannel == 8 && !rn;
    Vec<u8> px(default_allocator(), Tag::Test);
    px.resize(usize(w) * h * src.channels * (src.bitsPerChannel / 8));
    Image dst = make(w, h, src.channels, src.bitsPerChannel, px.span());
    for (u32 y = 0; y < h; ++y) {
        u32 const ys[2] = {min(2 * y, src.height - 1), min(2 * y + 1, src.height - 1)};
        for (u32 x = 0; x < w; ++x) {
            u32 const xs[2] = {min(2 * x, src.width - 1), min(2 * x + 1, src.width - 1)};
            for (u32 c = 0; c < src.channels; ++c) {
                u32 sum = 2;
                for (u32 sy : ys)
                    for (u32 sx : xs)
                        sum += srgb && c < 3 ? srgb8_to_linear16(u8(texel(src, sx, sy, c)))
                                             : texel(src, sx, sy, c);
                set_texel(dst, x, y, c, srgb && c < 3 ? linear16_to_srgb8(u16(sum / 4)) : sum / 4);
            }
        }
    }
    if (rn) renormalize(dst);
    return dst;
}

struct Size {
    u32 w, h;
};
constexpr Size kOddSizes[] = {
    {1,  1 },
    {1,  2 },
    {2,  1 },
    {1,  5 },
    {5,  1 },
    {2,  2 },
    {3,  3 },
    {4,  5 },
    {7,  6 },
    {9,  13},
    {16, 1 },
    {17, 11},
};

} // namespace

KILN_TEST(image, srgb_inverse_table_exhaustive) {
    u32 bad = 0;
    for (u32 v = 0; v < 65536; ++v)
        if (linear16_to_srgb8(u16(v)) != slow_linear16_to_srgb8(u16(v)) && bad++ < 8)
            KILN_CHECK_MSG(false, "linear %u: table %u, search %u", v, u32(linear16_to_srgb8(u16(v))),
                           u32(slow_linear16_to_srgb8(u16(v))));
    KILN_CHECK_EQ(bad, 0u);
}

KILN_TEST(image, prepare_matches_separate_ops) {
    Lcg rng{0x5EEDu};
    u32 const bitsList[2] = {8, 16};
    for (Size const sz : kOddSizes)
        for (u32 sb : bitsList)
            for (u32 sc = 1; sc <= 4; ++sc) {
                Image const src = random_image(rng, sz.w, sz.h, sc, sb);
                for (u32 db : bitsList)
                    for (u32 dc = 1; dc <= 4; ++dc)
                        for (u32 flags = 0; flags < 8; ++flags) {
                            PrepareOptions const opt = {.grayAlpha   = (flags & 1) != 0,
                                                        .flipGreen   = (flags & 2) != 0,
                                                        .renormalize = (flags & 4) != 0};
                            Result<Image> got        = prepare_image(src, dc, db, opt, default_allocator());
                            KILN_REQUIRE(got.ok());
                            Image const want = reference_prepare(src, dc, db, opt);
                            KILN_CHECK_MSG(same_image(*got, want),
                                           "%ux%u %u-bit %uch -> %u-bit %uch, flags %u differ", sz.w, sz.h,
                                           sb, sc, db, dc, flags);
                        }
            }

    Image const src = random_image(rng, 3, 3, 4, 8);
    KILN_CHECK_EQ(prepare_image(src, 0, 8, {}, default_allocator()).code(), Code::InvalidArgument);
    KILN_CHECK_EQ(prepare_image(src, 4, 12, {}, default_allocator()).code(), Code::InvalidArgument);
}

KILN_TEST(image, convert_matches_reference) {
    Lcg rng{0xC0DEu};
    u32 const bitsList[2] = {8, 16};
    for (Size const sz : kOddSizes)
        for (u32 sb : bitsList)
            for (u32 sc = 1; sc <= 4; ++sc) {
                Image const src = random_image(rng, sz.w, sz.h, sc, sb);
                for (u32 db : bitsList)
                    for (u32 dc = 1; dc <= 4; ++dc) {
                        Result<Image> got = convert_image(src, dc, db, default_allocator());
                        KILN_REQUIRE(got.ok());
                        KILN_CHECK_MSG(same_image(*got, reference_convert(src, dc, db)),
                                       "%ux%u %u-bit %uch -> %u-bit %uch differ", sz.w, sz.h, sb, sc, db, dc);
                    }
            }
}

KILN_TEST(image, downsample_matches_reference) {
    Lcg rng{0xB0Bu};
    u32 const bitsList[2] = {8, 16};
    for (Size const sz : kOddSizes)
        for (u32 bits : bitsList)
            for (u32 ch = 1; ch <= 4; ++ch) {
                Image const src = random_image(rng, sz.w, sz.h, ch, bits);
                for (u32 mode = 0; mode < 4; ++mode) {
                    MipOptions const opt = {.srgb = (mode & 1) != 0, .renormalize = (mode & 2) != 0};
                    Result<Image> got    = downsample_2x(src, opt, default_allocator());
                    KILN_REQUIRE(got.ok());
                    Image const want = reference_downsample(src, opt);
                    KILN_CHECK_MSG(same_image(*got, want), "%ux%u %u-bit %uch, srgb %u renorm %u differ",
                                   sz.w, sz.h, bits, ch, mode & 1, (mode >> 1) & 1);
                }
            }
}
