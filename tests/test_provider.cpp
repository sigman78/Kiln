// tests/test_provider.cpp — cook-on-miss provider (kiln/cook/provider.h) on a null-adapter Context;
// cook-only. Scratch stores and sources live under sample_dir(); models come from
// `<corpus_dir>/../gltf/generated` and `<corpus_dir>/../gltf/khronos`.
#include "hdr_writer.h"
#include "image_fixtures.h"
#include "kiln_test.h"
#include "ktx2_corpus.h" // texels
#include "png_writer.h"

#include "kiln/assets.h"
#include "kiln/cook/cli.h"
#include "kiln/cook/provider.h"
#include "kiln/ktx2.h"
#include "kiln/null_adapter.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif

using namespace kiln;

namespace {

bool file_exists(char const* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool store_path(char const* storeDir, char const* rel, char* out, usize cap) {
    format(out, cap, "%s/%s", storeDir, rel);
    return true;
}

/// An empty `<sample_dir()>/<suffix>`: files from an earlier run would load without a cook.
void scratch_dir(char const* suffix, char* out, usize cap) {
    format(out, cap, "%s/%s", kiln::test::sample_dir(), suffix);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

void gltf_khronos_dir(char* out, usize cap) {
    format(out, cap, "%s/../gltf/khronos", kiln::test::corpus_dir());
}

void gltf_generated_dir(char* out, usize cap) {
    format(out, cap, "%s/../gltf/generated", kiln::test::corpus_dir());
}

struct DiagCapture {
    u32 firstCode      = 0;
    int count          = 0;
    char firstMsg[256] = {};

    static void fn(void* user, Diagnostic const& d) noexcept {
        auto* self = static_cast<DiagCapture*>(user);
        if (self->firstCode == 0) {
            self->firstCode = d.code;
            format(self->firstMsg, sizeof self->firstMsg, "%.*s", KILN_SV(d.message));
        }
        ++self->count;
    }
    DiagSink sink() noexcept { return DiagSink{&fn, this}; }
};

/// Owns a null adapter + a runtime Context; tears both down.
struct TestContext {
    Adapter adapter{};
    NullAdapter* na = nullptr;
    Context* ctx    = nullptr;

    TestContext(TestContext const&)            = delete;
    TestContext& operator=(TestContext const&) = delete;
    TestContext() noexcept                     = default;

    bool init(StrView storeDir, Span<Root const> roots, DiagSink diag = {}) noexcept {
        Result<NullAdapter*> na_ = null_adapter_create({}, &adapter);
        if (!KILN_CHECK_MSG(na_.ok(), "null_adapter_create failed")) return false;
        na = na_.value();

        ContextDesc desc{};
        desc.adapter  = &adapter;
        desc.storeDir = storeDir;
        desc.roots    = roots;
        desc.diag     = diag;

        Result<Context*> c = create(desc);
        if (!KILN_CHECK_MSG(c.ok(), "create() failed (%s)", code_name(c.code()))) return false;
        ctx = c.value();
        return true;
    }

    ~TestContext() noexcept {
        if (ctx) cook::uninstall_provider(ctx); // optional: destroy() releases it too
        if (ctx) destroy(ctx);
        if (na) null_adapter_destroy(na);
    }
};

/// Pumps until `h`'s state settles (Ready/Failed) or `maxIters` is reached.
template <class Handle> State pump_until_settled(Context* ctx, Handle h, int maxIters = 500) noexcept {
    for (int i = 0; i < maxIters; ++i) {
        pump(ctx, {});
        State const s = state(ctx, h);
        if (s == State::Ready || s == State::Failed) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return state(ctx, h);
}

// Source poller helpers.

void make_dir(char const* path) {
#if defined(KILN_OS_WINDOWS)
    int const r = _mkdir(path);
#else
    int const r = ::mkdir(path, 0755);
#endif
    KILN_REQUIRE(r == 0 || errno == EEXIST);
}

bool read_file(char const* path, Vec<u8>& out) {
    out.clear();
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    u8 buf[4096];
    for (;;) {
        usize const n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        out.append(Span<u8 const>(buf, n));
    }
    std::fclose(f);
    return true;
}

/// Replaces `path` with `bytes` through a temp file, so the poller never reads a half-written source.
void replace_file(char const* path, Span<u8 const> bytes) {
    char tmp[1100];
    format(tmp, sizeof tmp, "%s.tmp", path);
    std::FILE* f = std::fopen(tmp, "wb");
    KILN_REQUIRE(f != nullptr);
    KILN_REQUIRE(std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size);
    std::fclose(f);
    std::remove(path);
    KILN_REQUIRE(std::rename(tmp, path) == 0);
}

void copy_file(char const* from, char const* to) {
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(from, bytes));
    replace_file(to, bytes.span());
}

bool same_bytes(Vec<u8> const& a, Vec<u8> const& b) {
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

/// Reads `path` every 10 ms for up to `ms` until it exists and differs from `old`; the
/// new bytes land in `out`. False on timeout.
bool wait_for_change(char const* path, Vec<u8> const& old, Vec<u8>& out, int ms = 3000) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (read_file(path, out) && !same_bytes(out, old)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// Waits up to `ms` for `path` to exist.
[[maybe_unused]] bool wait_for_file(char const* path, int ms = 3000) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (file_exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// 4x4 RGBA8, every texel a function of `seed`.
void test_pixels(u8 (&rgba)[4 * 4 * 4], u8 seed) {
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(seed + i * 7);
}

Vec<u8> test_png(u8 const (&rgba)[4 * 4 * 4]) {
    return kiln::test::png::encode({.width = 4, .height = 4, .colorType = 6, .depth = 8, .pixels = rgba});
}

// Uncompressed, so a re-cooked PNG's texels compare directly with its pixels.
cook::ProviderDesc const kWatchDesc{
    .storeMode = cook::StoreMode::Disk, .target = {.blockFormats = 0}, .watchSources = true, .pollMs = 20};

} // namespace

KILN_TEST(Provider, NoMountsIsInvalidArgument) {
    char storeDir[1024];
    scratch_dir("provider_store_empty_roots", storeDir, sizeof storeDir);

    TestContext tc;
    if (!tc.init(StrView(storeDir), {})) return;

    Status st = cook::install_provider(tc.ctx, cook::ProviderDesc{});
    KILN_CHECK(st.failed());
    KILN_CHECK_EQ(st.code, Code::InvalidArgument);
}

// destroy() frees a provider the host did not uninstall, and drops its registry entry, so a later
// context (possibly at the same address) starts without it.
KILN_TEST(Provider, DestroyReleasesAnInstalledProvider) {
    char storeDir[1024];
    scratch_dir("provider_store_release", storeDir, sizeof storeDir);
    Root const roots[] = {
        {{}, StrView(storeDir)}
    };
    {
        TestContext warm; // the registry's table is allocated once and kept
        if (!warm.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
        KILN_REQUIRE(cook::install_provider(warm.ctx, cook::ProviderDesc{}).ok());
    }
    u64 const before = default_alloc_stats(Tag::Cook).bytesCurrent;
    for (int i = 0; i < 2; ++i) {
        Adapter adapter{};
        Result<NullAdapter*> na = null_adapter_create({}, &adapter);
        KILN_REQUIRE(na.ok());
        ContextDesc desc{};
        desc.adapter         = &adapter;
        desc.storeDir        = StrView(storeDir);
        desc.roots           = Span<Root const>(roots, 1);
        Result<Context*> ctx = create(desc);
        KILN_REQUIRE(ctx.ok());
        KILN_REQUIRE(cook::install_provider(*ctx, cook::ProviderDesc{.watchSources = true}).ok());
        KILN_CHECK(cook_provider(*ctx).release != nullptr);
        destroy(*ctx); // no uninstall_provider
        null_adapter_destroy(*na);
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Cook).bytesCurrent, before);
}

#if KILN_MESH
// Disk mode writes the Named store layout; a second context without a provider loads those files.
KILN_TEST(Provider, DiskModeCooksAndWritesNamedStoreFiles) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };

    {
        TestContext tc;
        if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;

        Status const installed =
            cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
        KILN_REQUIRE(installed.ok());

        MeshHandle const mesh = request_mesh(tc.ctx, "cube_basic.glb");
        KILN_REQUIRE(mesh);
        KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Ready);

        char meshPath[1024];
        store_path(storeDir, "cube_basic.glb.mesh", meshPath, sizeof meshPath);
        KILN_CHECK(file_exists(meshPath));

        // pbr_textures.glb#hull_albedo is embedded in pbr_textures.glb: cooking it cooks that
        // mesh, which writes the mesh plus every embedded image it references.
        TextureHandle const tex = request_texture(tc.ctx, "pbr_textures.glb#hull_albedo");
        KILN_REQUIRE(tex);
        KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);

        char pbrMeshPath[1024];
        store_path(storeDir, "pbr_textures.glb.mesh", pbrMeshPath, sizeof pbrMeshPath);
        KILN_CHECK(file_exists(pbrMeshPath));

        // Distinct *referenced* textures in generated/pbr_textures.glb (manifest.txt):
        // hull_albedo, hull_normal, hull_orm (bound twice, one file) and hull_emissive.
        // hull_height is an unreferenced images[] entry and is never cooked.
        static char const* const kTextures[] = {"hull_albedo", "hull_normal", "hull_orm", "hull_emissive"};
        for (char const* name : kTextures) {
            char rel[256];
            format(rel, sizeof rel, "pbr_textures.glb#%s.ktx2", name);
            char path[1024];
            store_path(storeDir, rel, path, sizeof path);
            KILN_CHECK_MSG(file_exists(path), "missing %s", path);
        }
        char heightPath[1024];
        store_path(storeDir, "pbr_textures.glb#hull_height.ktx2", heightPath, sizeof heightPath);
        KILN_CHECK_MSG(!file_exists(heightPath), "hull_height should never be cooked (unreferenced)");
    }

    // A second context on the same store, without a provider: loads straight
    // from the files the first context just wrote (cache reuse).
    {
        TestContext tc2;
        if (!tc2.init(StrView(storeDir), {})) return;

        MeshHandle const mesh = request_mesh(tc2.ctx, "cube_basic.glb");
        KILN_REQUIRE(mesh);
        KILN_CHECK_EQ(pump_until_settled(tc2.ctx, mesh), State::Ready);

        TextureHandle const tex = request_texture(tc2.ctx, "pbr_textures.glb#hull_albedo");
        KILN_REQUIRE(tex);
        KILN_CHECK_EQ(pump_until_settled(tc2.ctx, tex), State::Ready);
    }
}

// generated/jpeg_texture.glb has no source of its own for its "albedo" image (embedded
// JPEG): cooking it cooks the owning mesh, same path as pbr_textures.glb#hull_albedo (PNG).
KILN_TEST(Provider, DiskModeCooksEmbeddedJpegTexture) {
    char storeDir[1024], gltfDir[1024];
    scratch_dir("provider_jpeg_embedded_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    TextureHandle const tex = request_texture(tc.ctx, "jpeg_texture.glb#albedo");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);

    char meshPath[1024], texPath[1024];
    store_path(storeDir, "jpeg_texture.glb.mesh", meshPath, sizeof meshPath);
    store_path(storeDir, "jpeg_texture.glb#albedo.ktx2", texPath, sizeof texPath);
    KILN_CHECK(file_exists(meshPath));
    KILN_CHECK_MSG(file_exists(texPath), "cook-on-miss did not write %s", texPath);
}

// Memory mode: cache-less, cooks every miss, never touches the store.
KILN_TEST(Provider, MemoryModeNeverWritesTheStore) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store_memory", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;

    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory});
    KILN_REQUIRE(installed.ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "cube_basic.glb");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Ready);

    char meshPath[1024];
    store_path(storeDir, "cube_basic.glb.mesh", meshPath, sizeof meshPath);
    KILN_CHECK_MSG(!file_exists(meshPath), "Memory mode must not write %s", meshPath);
}
#else
// KILN_MESH=OFF: a model source, and a texture embedded in one, fail with K5002 carrying the
// cooker's K1021; nothing is written.
KILN_TEST(Provider, MeshCookNotBuilt) {
    char storeDir[1024], gltfDir[1024];
    scratch_dir("provider_mesh_off_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), diags.sink())) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk}).ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "cube_basic.glb");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagCookOnMissFailed));
    KILN_CHECK_MSG(std::strstr(diags.firstMsg, "K1021") != nullptr, "message: %s", diags.firstMsg);

    diags                   = {};
    TextureHandle const tex = request_texture(tc.ctx, "pbr_textures.glb#hull_albedo");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagCookOnMissFailed));
    KILN_CHECK_MSG(std::strstr(diags.firstMsg, "K1021") != nullptr, "message: %s", diags.firstMsg);

    char meshPath[1024];
    store_path(storeDir, "cube_basic.glb.mesh", meshPath, sizeof meshPath);
    KILN_CHECK(!file_exists(meshPath));
}
#endif

// Missing source: the asset Fails with a K5001 store-miss diagnostic.
KILN_TEST(Provider, MissingSourceFailsWithStoreMiss) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store_missing", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), diags.sink())) return;

    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "does_not_exist_in_the_corpus.glb");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagStoreMiss));
}

// A texture named `tex.jpg` cooks from `<root>/tex.jpg` like any other source extension.
KILN_TEST(Provider, DiskModeCooksJpegSource) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_jpeg_src", root, sizeof root);
    scratch_dir("provider_jpeg_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100], texPath[1100];
    format(srcPath, sizeof srcPath, "%s/tex.jpg", root);
    format(texPath, sizeof texPath, "%s/tex.jpg.ktx2", storeDir);
    replace_file(srcPath, kiln::test::img::kJpegGradientRgbBytes);

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex.jpg");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    KILN_CHECK_MSG(file_exists(texPath), "cook-on-miss did not write %s", texPath);
}

// A standalone texture gets its usage from the name rules: `wall_n.png` is a linear normal map,
// `wall.png` (no rule) stays sRGB color.
KILN_TEST(Provider, StandaloneTextureUsageFromName) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_name_rules_src", root, sizeof root);
    scratch_dir("provider_name_rules_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(i * 7);
    char path[1100];
    for (char const* name : {"wall", "wall_n"}) {
        format(path, sizeof path, "%s/%s.png", root, name);
        replace_file(path, test_png(rgba).span());
    }

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk}).ok());

    TextureHandle const color  = request_texture(tc.ctx, "wall.png");
    TextureHandle const normal = request_texture(tc.ctx, "wall_n.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, color), State::Ready);
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, normal), State::Ready);
    KILN_CHECK(texture_info(tc.ctx, color).desc.format == Format::BC7_SRGB);
    Format const nf = texture_info(tc.ctx, normal).desc.format;
    KILN_CHECK_MSG(nf == Format::BC5_UNORM, "wall_n.png was not cooked as a normal map");
}

// A sidecar beats the name rule: `wall_n.png` with `usage = "color"` in `wall_n.png.kiln` is sRGB.
// A broken sidecar fails the cook instead of being ignored.
KILN_TEST(Provider, SidecarSetsTextureUsage) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_sidecar_src", root, sizeof root);
    scratch_dir("provider_sidecar_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(i * 5);
    char path[1100];
    for (char const* name : {"wall_n", "broken"}) {
        format(path, sizeof path, "%s/%s.png", root, name);
        replace_file(path, test_png(rgba).span());
    }
    StrView const color = "usage = \"color\"\n";
    format(path, sizeof path, "%s/wall_n.png.kiln", root);
    replace_file(path, Span<u8 const>(reinterpret_cast<u8 const*>(color.data), color.size));
    StrView const broken = "usage = \"bump\"\n";
    format(path, sizeof path, "%s/broken.png.kiln", root);
    replace_file(path, Span<u8 const>(reinterpret_cast<u8 const*>(broken.data), broken.size));

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk}).ok());

    TextureHandle const wall = request_texture(tc.ctx, "wall_n.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, wall), State::Ready);
    KILN_CHECK(texture_info(tc.ctx, wall).desc.format == Format::BC7_SRGB);
    TextureHandle const bad = request_texture(tc.ctx, "broken.png");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, bad), State::Failed);
}

// A sidecar is part of its source: adding one re-cooks the texture.
KILN_TEST(Provider, SourcePollerRecooksOnSidecarChange) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_sidecar_src", root, sizeof root);
    scratch_dir("provider_watch_sidecar_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(i * 3);
    char srcPath[1100], sidecar[1100], storeFile[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);
    format(sidecar, sizeof sidecar, "%s/tex.png.kiln", root);
    format(storeFile, sizeof storeFile, "%s/tex.png.ktx2", storeDir);
    replace_file(srcPath, test_png(rgba).span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());
    TextureHandle const tex = request_texture(tc.ctx, "tex.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    Vec<u8> before(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(storeFile, before));

    StrView const noMips = "genMips = false\n";
    replace_file(sidecar, Span<u8 const>(reinterpret_cast<u8 const*>(noMips.data), noMips.size));
    Vec<u8> after(default_allocator(), Tag::Test);
    bool const changed = wait_for_change(storeFile, before, after);
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK_MSG(changed, "adding %s did not re-cook %s", sidecar, storeFile);
}

// The source poller re-cooks a PNG whose file changed and overwrites its store file. This
// checks the store only; the runtime reloading from it is the runtime's own test.
KILN_TEST(Provider, SourcePollerRecooksPng) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_png_src", root, sizeof root);
    scratch_dir("provider_watch_png_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100], storeFile[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);
    format(storeFile, sizeof storeFile, "%s/tex.png.ktx2", storeDir);

    u8 first[4 * 4 * 4], second[4 * 4 * 4];
    test_pixels(first, 1);
    test_pixels(second, 100);
    replace_file(srcPath, test_png(first).span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex.png");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);

    Vec<u8> before(default_allocator(), Tag::Test);
    KILN_CHECK(read_file(storeFile, before));

    // Past the file-time granularity, so the rewrite gets a new modification time.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replace_file(srcPath, test_png(second).span());

    Vec<u8> after(default_allocator(), Tag::Test);
    bool const changed = wait_for_change(storeFile, before, after);
    cook::uninstall_provider(tc.ctx);
    if (!KILN_CHECK_MSG(changed, "the store file %s was not re-cooked", storeFile)) return;

    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(after.span());
    KILN_REQUIRE(v.ok());
    Vec<u8> const t0  = kiln::test::corpus::texels(*v, 0);
    Span<u8 const> l0 = t0.span();
    KILN_REQUIRE_EQ(l0.size, sizeof second);
    KILN_CHECK(std::memcmp(l0.data, second, sizeof second) == 0);
}

#if KILN_MESH
// A glb re-cook rewrites the mesh and the textures it embeds; for a glb without textures, only the mesh.
KILN_TEST(Provider, SourcePollerRecooksGlbAndTextures) {
    char root[1024], storeDir[1024], khronos[1024];
    scratch_dir("provider_watch_glb_src", root, sizeof root);
    scratch_dir("provider_watch_glb_store", storeDir, sizeof storeDir);
    gltf_khronos_dir(khronos, sizeof khronos);
    make_dir(root);

    char textured[1100], plain[1100], srcPath[1100], meshFile[1100], texFile[1100];
    format(textured, sizeof textured, "%s/BoxTextured.glb", khronos);
    format(plain, sizeof plain, "%s/Box.glb", khronos);
    format(srcPath, sizeof srcPath, "%s/box.glb", root);
    format(meshFile, sizeof meshFile, "%s/box.glb.mesh", storeDir);
    format(texFile, sizeof texFile, "%s/box.glb#image0.ktx2", storeDir); // BoxTextured's one unnamed image
    copy_file(textured, srcPath);

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "box.glb");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Ready);

    Vec<u8> texturedMesh(default_allocator(), Tag::Test);
    KILN_CHECK(read_file(meshFile, texturedMesh));
    KILN_CHECK_MSG(file_exists(texFile), "cook-on-miss did not write %s", texFile);

    // Box.glb has no textures: only the mesh file changes.
    copy_file(plain, srcPath);
    Vec<u8> plainMesh(default_allocator(), Tag::Test);
    bool const meshChanged = wait_for_change(meshFile, texturedMesh, plainMesh);

    // Back to BoxTextured.glb with its texture store file gone: the re-cook writes both again.
    std::remove(texFile);
    copy_file(textured, srcPath);
    Vec<u8> again(default_allocator(), Tag::Test);
    bool const meshBack = wait_for_change(meshFile, plainMesh, again);
    bool const texBack  = wait_for_file(texFile);
    cook::uninstall_provider(tc.ctx);

    KILN_CHECK_MSG(meshChanged, "%s was not re-cooked for Box.glb", meshFile);
    KILN_CHECK_MSG(meshBack, "%s was not re-cooked for BoxTextured.glb", meshFile);
    KILN_CHECK(same_bytes(again, texturedMesh));
    KILN_CHECK_MSG(texBack, "the re-cook did not rewrite %s", texFile);
}
#endif

// After uninstall_provider the poller is gone: a changed source rewrites nothing.
KILN_TEST(Provider, SourcePollerStopsOnUninstall) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_stop_src", root, sizeof root);
    scratch_dir("provider_watch_stop_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100], storeFile[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);
    format(storeFile, sizeof storeFile, "%s/tex.png.ktx2", storeDir);

    u8 first[4 * 4 * 4], second[4 * 4 * 4];
    test_pixels(first, 3);
    test_pixels(second, 200);
    replace_file(srcPath, test_png(first).span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex.png");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    Vec<u8> before(default_allocator(), Tag::Test);
    KILN_CHECK(read_file(storeFile, before));

    cook::uninstall_provider(tc.ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replace_file(srcPath, test_png(second).span());

    Vec<u8> after(default_allocator(), Tag::Test);
    KILN_CHECK(!wait_for_change(storeFile, before, after, 200));
}

// A named root: `lib:tex.png` cooks from the root's directory into `<store>/@lib/tex.png.ktx2`.
KILN_TEST(Provider, NamedRootCooksIntoItsStoreDirectory) {
    char root[1024], libRoot[1024], storeDir[1024];
    scratch_dir("provider_root_default", root, sizeof root);
    scratch_dir("provider_root_lib", libRoot, sizeof libRoot);
    scratch_dir("provider_root_store", storeDir, sizeof storeDir);
    make_dir(root);
    make_dir(libRoot);

    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 9);
    char path[1100];
    format(path, sizeof path, "%s/tex.png", libRoot);
    replace_file(path, test_png(rgba).span());

    Root const roots[] = {
        {{},    StrView(root)   },
        {"lib", StrView(libRoot)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 2), diags.sink())) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk}).ok());

    TextureHandle const tex = request_texture(tc.ctx, "lib:tex.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    char storeFile[1100];
    format(storeFile, sizeof storeFile, "%s/@lib/tex.png.ktx2", storeDir);
    KILN_CHECK_MSG(file_exists(storeFile), "cook-on-miss did not write %s", storeFile);

    // The same file is not in the default root.
    TextureHandle const missing = request_texture(tc.ctx, "tex.png");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, missing), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagStoreMiss));
}

KILN_TEST(Provider, UnknownMountFails) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_unknown_mount_src", root, sizeof root);
    scratch_dir("provider_unknown_mount_store", storeDir, sizeof storeDir);
    make_dir(root);

    Root const roots[] = {
        {{}, StrView(root)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), diags.sink())) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{}).ok());

    TextureHandle const tex = request_texture(tc.ctx, "nope:tex.png");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagCookOnMissFailed));
}

// The extension gives the kind: a mesh requested by a texture name, or the reverse, fails.
KILN_TEST(Provider, ExtensionMustMatchKind) {
    char storeDir[1024], gltfDir[1024];
    scratch_dir("provider_kind_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), diags.sink())) return;
    KILN_REQUIRE(
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory}).ok());

    TextureHandle const tex = request_texture(tc.ctx, "cube_basic.glb");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Failed);
    MeshHandle const mesh = request_mesh(tc.ctx, "pbr_textures.glb#hull_albedo");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Failed);
    MeshHandle const noExt = request_mesh(tc.ctx, "cube_basic");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, noExt), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagCookOnMissFailed));
    KILN_CHECK_EQ(diags.count, 3);
}

// A name in the wrong case never cooks: on Windows the file exists but differs in case (K5016),
// elsewhere it does not exist (K5001).
KILN_TEST(Provider, NameCaseMustMatchTheDisk) {
    char storeDir[1024], gltfDir[1024];
    scratch_dir("provider_case_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    Root const roots[] = {
        {{}, StrView(gltfDir)}
    };
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), diags.sink())) return;
    KILN_REQUIRE(
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory}).ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "Cube_Basic.glb");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Failed);
#if defined(KILN_OS_WINDOWS)
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagCookOnMissFailed));
    KILN_CHECK(!cook::source_case_matches(StrView(gltfDir), "Cube_Basic.glb"));
    KILN_CHECK(cook::source_case_matches(StrView(gltfDir), "cube_basic.glb"));
#else
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagStoreMiss));
#endif
}

namespace {

/// Turns mips off for every texture and refuses `big.png`.
Status no_mips_policy(void*, cook::CookAssetInfo const& asset, cook::TargetProfile const&,
                      cook::TextureCookSettings* s, DiagSink const*) noexcept {
    if (asset.name == "big.png") return make_status(Code::Unsupported);
    s->genMips = false;
    return kOk;
}

} // namespace

// The policy has the last word: it beats the sidecar, and a refusal fails the asset.
KILN_TEST(Provider, PolicyOverridesSidecarAndCanRefuse) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_policy_src", root, sizeof root);
    scratch_dir("provider_policy_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 4);
    char path[1100];
    for (char const* name : {"tex.png", "big.png"}) {
        format(path, sizeof path, "%s/%s", root, name);
        replace_file(path, test_png(rgba).span());
    }
    StrView const mips = "genMips = true\n";
    format(path, sizeof path, "%s/tex.png.kiln", root);
    replace_file(path, Span<u8 const>(reinterpret_cast<u8 const*>(mips.data), mips.size));

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    cook::ProviderDesc desc{.storeMode = cook::StoreMode::Memory};
    desc.policy.texture = &no_mips_policy;
    KILN_REQUIRE(cook::install_provider(tc.ctx, desc).ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    KILN_CHECK_EQ(texture_info(tc.ctx, tex).desc.levels, 1u);
    TextureHandle const big = request_texture(tc.ctx, "big.png");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, big), State::Failed);
}

// A project cook tool is cook_cli_main plus its policy.
KILN_TEST(Provider, CliMainAppliesThePolicy) {
    char root[1024], storeDir[1024];
    scratch_dir("cli_policy_src", root, sizeof root);
    scratch_dir("cli_policy_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 5);
    char src[1100];
    format(src, sizeof src, "%s/tex.png", root);
    replace_file(src, test_png(rgba).span());

    char arg0[] = "kiln-cook", argO[] = "-o", argQ[] = "-q";
    char* argv[] = {arg0, src, argO, storeDir, argQ};
    cook::CookPolicy policy;
    policy.texture = &no_mips_policy;
    KILN_REQUIRE_EQ(cook::cook_cli_main(5, argv, policy), 0);

    char storeFile[1100];
    format(storeFile, sizeof storeFile, "%s/tex.png.ktx2", storeDir);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(storeFile, bytes));
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(bytes.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->desc().levels, 1u);

    // Without the policy the same input gets its mips.
    char* argv2[] = {arg0, src, argO, storeDir, argQ};
    std::remove(storeFile);
    KILN_REQUIRE_EQ(cook::cook_cli_main(5, argv2), 0);
    KILN_REQUIRE(read_file(storeFile, bytes));
    Result<ktx2::Ktx2View> v2 = ktx2::Ktx2View::open(bytes.span());
    KILN_REQUIRE(v2.ok());
    KILN_CHECK(v2->desc().levels > 1u);
}

#if !KILN_MESH
// KILN_MESH=OFF: kiln-cook still cooks the textures it is given, fails the glb, and exits 3.
KILN_TEST(Provider, CliMainWithoutMeshCook) {
    char root[1024], storeDir[1024], khronos[1024];
    scratch_dir("cli_mesh_off_src", root, sizeof root);
    scratch_dir("cli_mesh_off_store", storeDir, sizeof storeDir);
    gltf_khronos_dir(khronos, sizeof khronos);
    make_dir(root);

    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 9);
    char png[1100], glb[1100];
    format(png, sizeof png, "%s/tex.png", root);
    format(glb, sizeof glb, "%s/Box.glb", khronos);
    replace_file(png, test_png(rgba).span());

    char arg0[] = "kiln-cook", argO[] = "-o", argQ[] = "-q";
    char* argv[] = {arg0, glb, png, argO, storeDir, argQ};
    KILN_CHECK_EQ(cook::cook_cli_main(6, argv), 3);

    char storeFile[1100];
    format(storeFile, sizeof storeFile, "%s/tex.png.ktx2", storeDir);
    KILN_CHECK_MSG(file_exists(storeFile), "kiln-cook did not write %s", storeFile);
    format(storeFile, sizeof storeFile, "%s/Box.glb.mesh", storeDir);
    KILN_CHECK(!file_exists(storeFile));
}
#endif

// A cube KTX2 cooks on miss and loads as a cube when the request expects one.
KILN_TEST(Provider, CubeKtx2CooksAndLoads) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_cube_src", root, sizeof root);
    scratch_dir("provider_cube_store", storeDir, sizeof storeDir);
    make_dir(root);
    char src[1100], dst[1100];
    format(src, sizeof src, "%s/generated/cube_rgba8_srgb_mip.ktx2", kiln::test::corpus_dir());
    format(dst, sizeof dst, "%s/sky.ktx2", root);
    copy_file(src, dst);

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk}).ok());

    TextureHandle const sky =
        request_texture(tc.ctx, "sky.ktx2", RequestOptions{.textureShape = TextureShape::Cube});
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, sky), State::Ready);
    TextureInfo const ti = texture_info(tc.ctx, sky);
    KILN_CHECK(!ti.isPlaceholder && ti.desc.isCube && ti.desc.faces == 6);
    char storeFile[1100];
    format(storeFile, sizeof storeFile, "%s/sky.ktx2.ktx2", storeDir);
    KILN_CHECK(file_exists(storeFile));
}

// A strip named `_cube` cooks on miss into a cube, and loads as one.
KILN_TEST(Provider, CubeStripFromName) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_strip_src", root, sizeof root);
    scratch_dir("provider_strip_store", storeDir, sizeof storeDir);
    make_dir(root);
    u8 rgba[4 * 24 * 4];
    for (usize i = 0; i < sizeof rgba; ++i)
        rgba[i] = u8(i * 3);
    Vec<u8> const png =
        kiln::test::png::encode({.width = 4, .height = 24, .colorType = 6, .depth = 8, .pixels = rgba});
    char path[1100];
    format(path, sizeof path, "%s/sky_cube.png", root);
    replace_file(path, png.span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory}).ok());
    TextureHandle const sky =
        request_texture(tc.ctx, "sky_cube.png", RequestOptions{.textureShape = TextureShape::Cube});
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, sky), State::Ready);
    TextureInfo const ti = texture_info(tc.ctx, sky);
    KILN_CHECK(ti.desc.isCube && ti.desc.width == 4 && ti.desc.height == 4);
}

// A .hdr source cooks on miss to RGBA16F with no settings at all.
KILN_TEST(Provider, HdrSourceCooksToBc6h) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_hdr_src", root, sizeof root);
    scratch_dir("provider_hdr_store", storeDir, sizeof storeDir);
    make_dir(root);
    u8 rgbe[4 * 24 * 4];
    kiln::test::hdr::pattern(rgbe, 4 * 24, 21);
    Vec<u8> const file = kiln::test::hdr::encode_flat(4, 24, Span<u8 const>(rgbe, sizeof rgbe));
    char path[1100];
    format(path, sizeof path, "%s/sky_cube.hdr", root);
    replace_file(path, file.span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory}).ok());
    TextureHandle const sky =
        request_texture(tc.ctx, "sky_cube.hdr", RequestOptions{.textureShape = TextureShape::Cube});
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, sky), State::Ready);
    TextureInfo const ti = texture_info(tc.ctx, sky);
    KILN_CHECK(ti.desc.isCube && ti.desc.format == Format::BC6H_UFLOAT);
}

// A disk store holds files of one profile: a provider with another profile writes nothing.
KILN_TEST(Provider, RefusesAStoreOfAnotherProfile) {
    char storeDir[1024];
    scratch_dir("provider_store_profile", storeDir, sizeof storeDir);
    Root const roots[] = {
        {{}, StrView(storeDir)}
    };
    {
        TestContext tc;
        if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
        KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{}).ok()); // writes compat
    }
    DiagCapture cap;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), cap.sink())) return;
    Status const st = cook::install_provider(tc.ctx, cook::ProviderDesc{.target = cook::kDesktopTarget});
    KILN_CHECK_EQ(st.code, Code::InvalidArgument);
    KILN_CHECK_EQ(cap.firstCode, u32(cook::kDiagStoreProfileMismatch));
    KILN_CHECK(cook_provider(tc.ctx).cook == nullptr);
    StoreProfile p;
    KILN_REQUIRE(read_store_profile(nullptr, StrView(storeDir), &p).ok());
    KILN_CHECK(StrView(p.name) == "compat");
}
