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

Result<CookedTexture> run_cook(Span<u8 const> bytes, TextureCookSettings const& s) {
    return cook_texture({.bytes = bytes, .assetPath = "test/golden", .sourcePath = "golden.png"}, s,
                        TargetProfile{});
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
