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
#include "kiln/manifest.h"
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

bool read_file(char const* path, Vec<u8>& out);

/// The artifact of `name` in profile `compat` of the store's manifest on disk: false when the
/// manifest (or the entry) is missing. `out` (optional) gets its bytes, `key` its build key. The
/// provider writes the manifest on release, on each poller round, and at most once a second on a
/// request.
bool stored(char const* storeDir, AssetKind kind, StrView name, Vec<u8>* out = nullptr,
            Hash128* key = nullptr) {
    char path[1100];
    (void)manifest_file_path(StrView(storeDir), path, sizeof path);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    if (!read_file(path, bytes)) return false;
    Result<ManifestView> v = ManifestView::open(bytes.span());
    ManifestProfile p;
    ManifestEntry e;
    if (!v.ok() || !v->find_profile("compat", &p) || !p.find(kind, name, &e)) return false;
    if (key) *key = e.key;
    if (!out) return true;
    (void)artifact_file_path(StrView(storeDir), e.key, path, sizeof path);
    return read_file(path, *out);
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

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagCapture*>(user);
        if (self->firstCode == 0) {
            self->firstCode = d.code;
            format(self->firstMsg, sizeof self->firstMsg, "%.*s", KILN_SV(d.message));
        }
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

/// Owns a null adapter + a runtime Context; tears both down.
struct TestContext {
    Adapter adapter{};
    NullAdapter* na = nullptr;
    Context* ctx    = nullptr;

    TestContext(TestContext const&)            = delete;
    TestContext& operator=(TestContext const&) = delete;
    TestContext()                              = default;

    bool init(StrView storeDir, Span<Root const> roots, DiagSink diag = {}, HotReloadDesc hotReload = {}) {
        Result<NullAdapter*> na_ = null_adapter_create({}, &adapter);
        if (!KILN_CHECK_MSG(na_.ok(), "null_adapter_create failed")) return false;
        na = na_.value();

        ContextDesc desc{};
        desc.adapter   = &adapter;
        desc.storeDir  = storeDir;
        desc.roots     = roots;
        desc.diag      = diag;
        desc.hotReload = hotReload;

        Result<Context*> c = create(desc);
        if (!KILN_CHECK_MSG(c.ok(), "create() failed (%s)", code_name(c.code()))) return false;
        ctx = c.value();
        return true;
    }

    ~TestContext() {
        if (ctx) cook::uninstall_provider(ctx); // optional: destroy() releases it too
        if (ctx) destroy(ctx);
        if (na) null_adapter_destroy(na);
    }
};

/// Pumps until `h`'s state settles (Ready/Failed) or `maxIters` is reached.
template <class Handle> State pump_until_settled(Context* ctx, Handle h, int maxIters = 500) {
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

/// Reads the manifest every 10 ms for up to `ms` until `name`'s artifact exists and differs from
/// `old`; the new bytes land in `out`. False on timeout.
bool wait_for_change(char const* storeDir, AssetKind kind, StrView name, Vec<u8> const& old, Vec<u8>& out,
                     int ms = 3000) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (stored(storeDir, kind, name, &out) && !same_bytes(out, old)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// Waits up to `ms` for `name` to be in the manifest.
[[maybe_unused]] bool wait_for_entry(char const* storeDir, AssetKind kind, StrView name, int ms = 3000) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (stored(storeDir, kind, name)) return true;
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

namespace {

/// A host wrapper that is slow to reach the provider it wraps.
struct SlowWrapper {
    CookProvider inner;
    static Status prepare(void* user, AssetKind kind, StrView name, PrepareMode mode, Allocator const* alloc,
                          Vec<u8>* out, Hash128* key, DiagSink const* diag) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto const* self = static_cast<SlowWrapper const*>(user);
        return self->inner.prepare(self->inner.user, kind, name, mode, alloc, out, key, diag);
    }
};

} // namespace

// Uninstalling frees the provider only after the loads that may still call it are done.
KILN_TEST(Provider, UninstallWaitsForLoadsInFlight) {
    char root[1024], png[1100];
    scratch_dir("provider_uninstall_src", root, sizeof root);
    make_dir(root);
    format(png, sizeof png, "%s/tile.png", root);
    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 7);
    replace_file(png, test_png(rgba).span());
    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init({}, Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory,
                                                                   .target    = {.blockFormats = 0}})
                     .ok());
    SlowWrapper wrapper{cook_provider(tc.ctx)};
    set_cook_provider(tc.ctx, {&SlowWrapper::prepare, &wrapper, nullptr});
    TextureHandle const tex = request_texture(tc.ctx, "tile.png");
    pump(tc.ctx, {}); // dispatches the load, which holds the wrapper
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cook::uninstall_provider(tc.ctx); // the load is still in the wrapper's sleep
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    release(tc.ctx, tex);
}

#if KILN_MESH
// Disk mode publishes into the manifest; a second context without a provider loads what it wrote.
KILN_TEST(Provider, DiskModeCooksIntoTheManifest) {
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

        // pbr_textures.glb#hull_albedo is embedded in pbr_textures.glb: cooking it cooks that
        // mesh, which publishes the mesh plus every embedded image it references.
        TextureHandle const tex = request_texture(tc.ctx, "pbr_textures.glb#hull_albedo");
        KILN_REQUIRE(tex);
        KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
        cook::uninstall_provider(tc.ctx); // writes the manifest

        KILN_CHECK(stored(storeDir, AssetKind::Mesh, "cube_basic.glb"));
        KILN_CHECK(stored(storeDir, AssetKind::Mesh, "pbr_textures.glb"));
        // Distinct *referenced* textures in generated/pbr_textures.glb (manifest.txt):
        // hull_albedo, hull_normal, hull_orm (bound twice, one file) and hull_emissive.
        // hull_height is an unreferenced images[] entry and is never cooked.
        static char const* const kTextures[] = {"hull_albedo", "hull_normal", "hull_orm", "hull_emissive"};
        for (char const* name : kTextures) {
            char full[256];
            format(full, sizeof full, "pbr_textures.glb#%s", name);
            KILN_CHECK_MSG(stored(storeDir, AssetKind::Texture, StrView(full)), "missing %s", full);
        }
        KILN_CHECK_MSG(!stored(storeDir, AssetKind::Texture, "pbr_textures.glb#hull_height"),
                       "hull_height should never be cooked (unreferenced)");
    }

    // A second context on the same store, without a provider: loads straight
    // from the manifest the first context just wrote (cache reuse).
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
    cook::uninstall_provider(tc.ctx);

    KILN_CHECK(stored(storeDir, AssetKind::Mesh, "jpeg_texture.glb"));
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "jpeg_texture.glb#albedo"));
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
    cook::uninstall_provider(tc.ctx);

    char path[1100];
    (void)manifest_file_path(StrView(storeDir), path, sizeof path);
    KILN_CHECK_MSG(!file_exists(path), "Memory mode must not write %s", path);
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

    cook::uninstall_provider(tc.ctx);
    KILN_CHECK(!stored(storeDir, AssetKind::Mesh, "cube_basic.glb"));
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

    char srcPath[1100];
    format(srcPath, sizeof srcPath, "%s/tex.jpg", root);
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
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "tex.jpg"));
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
    char srcPath[1100], sidecar[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);
    format(sidecar, sizeof sidecar, "%s/tex.png.kiln", root);
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
    KILN_REQUIRE(wait_for_change(storeDir, AssetKind::Texture, "tex.png", {}, before));

    StrView const noMips = "genMips = false\n";
    replace_file(sidecar, Span<u8 const>(reinterpret_cast<u8 const*>(noMips.data), noMips.size));
    Vec<u8> after(default_allocator(), Tag::Test);
    bool const changed = wait_for_change(storeDir, AssetKind::Texture, "tex.png", before, after);
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK_MSG(changed, "adding %s did not re-cook tex.png", sidecar);
}

// The source poller re-cooks a PNG whose file changed and publishes it. This checks the store
// only; the runtime reloading from it is the runtime's own test.
KILN_TEST(Provider, SourcePollerRecooksPng) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_png_src", root, sizeof root);
    scratch_dir("provider_watch_png_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);

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
    KILN_CHECK(wait_for_change(storeDir, AssetKind::Texture, "tex.png", {}, before));

    // Past the file-time granularity, so the rewrite gets a new modification time.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replace_file(srcPath, test_png(second).span());

    Vec<u8> after(default_allocator(), Tag::Test);
    bool const changed = wait_for_change(storeDir, AssetKind::Texture, "tex.png", before, after);
    cook::uninstall_provider(tc.ctx);
    if (!KILN_CHECK_MSG(changed, "tex.png was not re-cooked")) return;

    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(after.span());
    KILN_REQUIRE(v.ok());
    Vec<u8> const t0  = kiln::test::corpus::texels(*v, 0);
    Span<u8 const> l0 = t0.span();
    KILN_REQUIRE_EQ(l0.size, sizeof second);
    KILN_CHECK(std::memcmp(l0.data, second, sizeof second) == 0);
}

#if KILN_MESH
// A glb re-cook publishes the mesh and the images it embeds; images it lost leave the manifest.
KILN_TEST(Provider, SourcePollerRecooksGlbAndTextures) {
    char root[1024], storeDir[1024], khronos[1024];
    scratch_dir("provider_watch_glb_src", root, sizeof root);
    scratch_dir("provider_watch_glb_store", storeDir, sizeof storeDir);
    gltf_khronos_dir(khronos, sizeof khronos);
    make_dir(root);

    char textured[1100], plain[1100], srcPath[1100];
    format(textured, sizeof textured, "%s/BoxTextured.glb", khronos);
    format(plain, sizeof plain, "%s/Box.glb", khronos);
    format(srcPath, sizeof srcPath, "%s/box.glb", root);
    StrView const texName = "box.glb#image0"; // BoxTextured's one unnamed image
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
    KILN_CHECK(wait_for_change(storeDir, AssetKind::Mesh, "box.glb", {}, texturedMesh));
    KILN_CHECK_MSG(stored(storeDir, AssetKind::Texture, texName),
                   "cook-on-miss did not publish box.glb#image0");

    // Box.glb has no textures: the mesh changes and the image leaves the manifest.
    copy_file(plain, srcPath);
    Vec<u8> plainMesh(default_allocator(), Tag::Test);
    bool const meshChanged = wait_for_change(storeDir, AssetKind::Mesh, "box.glb", texturedMesh, plainMesh);
    bool const texGone     = !stored(storeDir, AssetKind::Texture, texName);

    // Back to BoxTextured.glb: the re-cook publishes both again.
    copy_file(textured, srcPath);
    Vec<u8> again(default_allocator(), Tag::Test);
    bool const meshBack = wait_for_change(storeDir, AssetKind::Mesh, "box.glb", plainMesh, again);
    bool const texBack  = wait_for_entry(storeDir, AssetKind::Texture, texName);
    cook::uninstall_provider(tc.ctx);

    KILN_CHECK_MSG(meshChanged, "box.glb was not re-cooked for Box.glb");
    KILN_CHECK(texGone);
    KILN_CHECK_MSG(meshBack, "box.glb was not re-cooked for BoxTextured.glb");
    KILN_CHECK(same_bytes(again, texturedMesh));
    KILN_CHECK_MSG(texBack, "the re-cook did not publish box.glb#image0");
}
#endif

// After uninstall_provider the poller is gone: a changed source publishes nothing.
KILN_TEST(Provider, SourcePollerStopsOnUninstall) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_stop_src", root, sizeof root);
    scratch_dir("provider_watch_stop_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);

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
    cook::uninstall_provider(tc.ctx);
    Vec<u8> before(default_allocator(), Tag::Test);
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "tex.png", &before));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replace_file(srcPath, test_png(second).span());

    Vec<u8> after(default_allocator(), Tag::Test);
    KILN_CHECK(!wait_for_change(storeDir, AssetKind::Texture, "tex.png", before, after, 200));
}

// A named root: `lib:tex.png` cooks from the root's directory and keeps its name in the manifest.
KILN_TEST(Provider, NamedRootCooksFromItsDirectory) {
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

    // The same file is not in the default root.
    TextureHandle const missing = request_texture(tc.ctx, "tex.png");
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, missing), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagStoreMiss));
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "lib:tex.png"));
    KILN_CHECK(!stored(storeDir, AssetKind::Texture, "tex.png"));
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
                      cook::TextureCookSettings* s, DiagSink const*) {
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

namespace {

void write_text_file(char const* path, char const* text) {
    replace_file(path, Span<u8 const>(reinterpret_cast<u8 const*>(text), std::strlen(text)));
}

} // namespace

// ProviderDesc::projectFile: kiln.toml's rules apply to what the provider cooks; an error in the file
// fails the install.
KILN_TEST(Provider, ProjectFileSetsSettings) {
    char root[1024], storeDir[1024], dir[1024], project[1100], path[1100];
    scratch_dir("provider_project_src", root, sizeof root);
    scratch_dir("provider_project_store", storeDir, sizeof storeDir);
    scratch_dir("provider_project_dir", dir, sizeof dir);
    make_dir(root);
    make_dir(dir);
    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 9);
    for (char const* name : {"ui/button.png", "rock.png"}) {
        format(path, sizeof path, "%s/%s", root, name);
        if (std::strchr(name, '/')) {
            char sub[1100];
            format(sub, sizeof sub, "%s/ui", root);
            make_dir(sub);
        }
        replace_file(path, test_png(rgba).span());
    }
    format(project, sizeof project, "%s/kiln.toml", dir);
    write_text_file(project, "[[texture.rule]]\nmatch = [\"ui/**\"]\ngenMips = false\n");

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(
        cook::install_provider(tc.ctx, {.storeMode = cook::StoreMode::Memory, .projectFile = project}).ok());
    TextureHandle const ui   = request_texture(tc.ctx, "ui/button.png");
    TextureHandle const rock = request_texture(tc.ctx, "rock.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, ui), State::Ready);
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, rock), State::Ready);
    KILN_CHECK_EQ(texture_info(tc.ctx, ui).desc.levels, 1u);
    KILN_CHECK_EQ(texture_info(tc.ctx, rock).desc.levels, 3u);
    cook::uninstall_provider(tc.ctx);

    write_text_file(project, "[texture]\nnoSuchKey = 1\n");
    KILN_CHECK(cook::install_provider(tc.ctx, {.storeMode = cook::StoreMode::Memory, .projectFile = project})
                   .failed());
}

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
// With both pollers on, an edit to kiln.toml cooks the assets it changes again and the runtime
// reloads them; an edit with errors keeps the previous project.
KILN_TEST(Provider, ProjectEditReloads) {
    char root[1024], storeDir[1024], dir[1024], project[1100], path[1100];
    scratch_dir("provider_project_watch_src", root, sizeof root);
    scratch_dir("provider_project_watch_store", storeDir, sizeof storeDir);
    scratch_dir("provider_project_watch_dir", dir, sizeof dir);
    make_dir(root);
    make_dir(dir);
    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 11);
    format(path, sizeof path, "%s/tile.png", root);
    replace_file(path, test_png(rgba).span());
    format(project, sizeof project, "%s/kiln.toml", dir);
    write_text_file(project, "[texture]\ngenMips = false\n");

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), {}, {.watchStore = true, .pollMs = 20}))
        return;
    cook::ProviderDesc desc = kWatchDesc;
    desc.projectFile        = project;
    KILN_REQUIRE(cook::install_provider(tc.ctx, desc).ok());
    TextureHandle const tex = request_texture(tc.ctx, "tile.png");
    KILN_REQUIRE_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    KILN_CHECK_EQ(texture_info(tc.ctx, tex).desc.levels, 1u);

    auto pump_for_ms = [&](int ms) {
        for (int i = 0; i < ms / 10; ++i) {
            pump(tc.ctx, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // past file-time granularity
    write_text_file(project, "[texture]\ngenMips = maybe\n");   // an error: nothing changes
    pump_for_ms(300);
    KILN_CHECK_EQ(texture_info(tc.ctx, tex).version, 1u);

    write_text_file(project, "[texture]\ngenMips = true\n");
    bool reloaded = false;
    for (int i = 0; i < 500 && !reloaded; ++i) {
        pump(tc.ctx, {});
        reloaded = texture_info(tc.ctx, tex).version > 1;
        if (!reloaded) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cook::uninstall_provider(tc.ctx);
    if (!KILN_CHECK_MSG(reloaded, "the texture did not reload after kiln.toml changed")) return;
    KILN_CHECK_EQ(texture_info(tc.ctx, tex).desc.levels, 3u);
    release(tc.ctx, tex);
}

namespace {

/// Pumps until `tex` is Ready, for at most 5 s.
bool pump_until_ready(Context* ctx, TextureHandle tex) {
    for (int i = 0; i < 500; ++i) {
        pump(ctx, {});
        if (state(ctx, tex) == State::Ready) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// A watching provider over <scratch>/<name>_src/tile.png (`png`, else "not a png") and, if `toml` is
/// set, <scratch>/<name>_src/kiln.toml. The first request of tile.png fails.
struct FailingFirstLoad {
    char root[1024], storeDir[1024], png[1100], project[1100];
    TestContext tc;
    TextureHandle tex;

    bool init(char const* name, bool validPng, char const* toml) {
        char dir[256];
        format(dir, sizeof dir, "%s_src", name);
        scratch_dir(dir, root, sizeof root);
        format(dir, sizeof dir, "%s_store", name);
        scratch_dir(dir, storeDir, sizeof storeDir);
        make_dir(root);
        format(png, sizeof png, "%s/tile.png", root);
        format(project, sizeof project, "%s/kiln.toml", root);
        if (validPng)
            write_png(11);
        else
            write_text_file(png, "not a png");
        if (toml) write_text_file(project, toml);
        Root const roots[] = {
            {{}, StrView(root)}
        };
        if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), {}, {.watchStore = true, .pollMs = 20}))
            return false;
        cook::ProviderDesc desc = kWatchDesc;
        if (toml) desc.projectFile = project;
        if (!KILN_CHECK(cook::install_provider(tc.ctx, desc).ok())) return false;
        tex = request_texture(tc.ctx, "tile.png");
        return KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Failed);
    }
    void write_png(u8 seed) {
        u8 rgba[4 * 4 * 4];
        test_pixels(rgba, seed);
        replace_file(png, test_png(rgba).span());
    }
};

} // namespace

// A source that fails its first cook has no record to watch; fixing it still reloads it.
KILN_TEST(Provider, FailedFirstLoadReloadsWhenTheSourceIsFixed) {
    FailingFirstLoad f;
    if (!f.init("provider_failed_source", false, nullptr)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // past file-time granularity
    f.write_png(11);
    bool const ready = pump_until_ready(f.tc.ctx, f.tex);
    cook::uninstall_provider(f.tc.ctx);
    KILN_CHECK_MSG(ready, "tile.png did not reload after its source was fixed");
    release(f.tc.ctx, f.tex);
}

// A request that fails on a kiln.toml setting reloads when the project is fixed.
KILN_TEST(Provider, FailedFirstLoadReloadsWhenTheProjectIsFixed) {
    FailingFirstLoad f;
    if (!f.init("provider_failed_project", true, "[texture]\nencoding = \"bc6h\"\n")) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_text_file(f.project, "[texture]\nencoding = \"uncompressed\"\n");
    bool const ready = pump_until_ready(f.tc.ctx, f.tex);
    cook::uninstall_provider(f.tc.ctx);
    KILN_CHECK_MSG(ready, "tile.png did not reload after kiln.toml was fixed");
    release(f.tc.ctx, f.tex);
}
#endif

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
    KILN_REQUIRE_EQ(cook::cook_cli_main(5, argv, policy, 1), 0); // version 1: this policy

    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(stored(storeDir, AssetKind::Texture, "tex.png", &bytes));
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(bytes.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->desc().levels, 1u);

    // Without the policy (version 0) the same input gets its mips: the new policy version checks
    // the key again, and the key differs, so the texture cooks again.
    char* argv2[] = {arg0, src, argO, storeDir, argQ};
    KILN_REQUIRE_EQ(cook::cook_cli_main(5, argv2), 0);
    KILN_REQUIRE(stored(storeDir, AssetKind::Texture, "tex.png", &bytes));
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

    KILN_CHECK_MSG(stored(storeDir, AssetKind::Texture, "tex.png"), "kiln-cook did not publish tex.png");
    KILN_CHECK(!stored(storeDir, AssetKind::Mesh, "Box.glb"));
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
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "sky.ktx2"));
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

// The provider's quality cap applies to its own cooks: an entry cooked without the cap (as by
// kiln-cook) stays in use, and a changed source cooks again under the cap.
KILN_TEST(Provider, QualityCapKeepsBetterEntries) {
    char root[1024], storeDir[1024], refDir[1024];
    scratch_dir("provider_cap_src", root, sizeof root);
    scratch_dir("provider_cap_store", storeDir, sizeof storeDir);
    scratch_dir("provider_cap_ref", refDir, sizeof refDir);
    make_dir(root);
    char path[1100];
    format(path, sizeof path, "%s/tile.png", root);
    u8 rgba[4 * 4 * 4];
    test_pixels(rgba, 3);
    replace_file(path, test_png(rgba).span());
    Root const roots[] = {
        {{}, StrView(root)}
    };
    auto cook_once = [&](char const* store, cook::EncodeQuality cap, Hash128* key) {
        TestContext tc;
        if (!tc.init(StrView(store), Span<Root const>(roots, 1))) return false;
        cook::ProviderDesc const pd{.storeMode = cook::StoreMode::Disk, .maxQuality = cap};
        if (!KILN_CHECK(cook::install_provider(tc.ctx, pd).ok())) return false;
        TextureHandle const t = request_texture(tc.ctx, "tile.png");
        bool const ready      = pump_until_settled(tc.ctx, t) == State::Ready;
        release(tc.ctx, t);
        cook::uninstall_provider(tc.ctx);
        return KILN_CHECK(ready) && KILN_CHECK(stored(store, AssetKind::Texture, "tile.png", nullptr, key));
    };
    Hash128 high, kept, capped, reference;
    if (!cook_once(storeDir, cook::EncodeQuality::High, &high)) return;
    if (!cook_once(storeDir, cook::EncodeQuality::Fast, &kept)) return;
    KILN_CHECK(kept == high);

    test_pixels(rgba, 91);
    replace_file(path, test_png(rgba).span());
    if (!cook_once(storeDir, cook::EncodeQuality::Fast, &capped)) return;
    if (!cook_once(refDir, cook::EncodeQuality::High, &reference)) return;
    KILN_CHECK(!(capped == high));
    KILN_CHECK(!(capped == reference));
}

// A provider cooks for the context's profile only: another target writes nothing.
KILN_TEST(Provider, RefusesAnotherProfile) {
    char storeDir[1024];
    scratch_dir("provider_store_profile", storeDir, sizeof storeDir);
    Root const roots[] = {
        {{}, StrView(storeDir)}
    };
    DiagCapture cap;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), cap.sink())) return;
    Status const st = cook::install_provider(tc.ctx, cook::ProviderDesc{.target = cook::kDesktopTarget});
    KILN_CHECK_EQ(st.code, Code::InvalidArgument);
    KILN_CHECK_EQ(cap.firstCode, u32(cook::kDiagStoreProfileMismatch));
    KILN_CHECK(cook_provider(tc.ctx).prepare == nullptr);
    char path[1100];
    (void)manifest_file_path(StrView(storeDir), path, sizeof path);
    KILN_CHECK(!file_exists(path));
}

// Two source PNGs cook on miss into one array (docs/design/runtime-texture-arrays.md); its layers
// are not textures of their own.
KILN_TEST(Provider, ArrayCookedOnMiss) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_array_src", root, sizeof root);
    scratch_dir("provider_array_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgbaA[4 * 4 * 4], rgbaB[4 * 4 * 4];
    test_pixels(rgbaA, 10);
    test_pixels(rgbaB, 20);
    char path[1100];
    format(path, sizeof path, "%s/tileA.png", root);
    replace_file(path, test_png(rgbaA).span());
    format(path, sizeof path, "%s/tileB.png", root);
    replace_file(path, test_png(rgbaB).span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk,
                                                                   .target    = {.blockFormats = 0}})
                     .ok());

    StrView const layers[] = {"tileA.png", "tileB.png"};
    TextureHandle const arr =
        request_texture_array(tc.ctx, {.name = "arr/tiles", .layers = Span<StrView const>(layers, 2)});
    KILN_REQUIRE(arr);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, arr), State::Ready);
    TextureInfo const ti = texture_info(tc.ctx, arr);
    KILN_CHECK(ti.desc.isArray && ti.desc.layers == 2);
    cook::uninstall_provider(tc.ctx);

    KILN_CHECK(stored(storeDir, AssetKind::Texture, "tileA.png"));
    KILN_CHECK(stored(storeDir, AssetKind::Texture, "tileB.png"));
    KILN_CHECK(find_texture(tc.ctx, asset_id("tileA.png")).is_null()); // a layer is not a texture of its own
    release(tc.ctx, arr);
}

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
// With the cook provider's source poller and the runtime's store poller both on, editing a source
// PNG re-cooks it and the array gets a Changed event, the array's side of hot-reload.md.
KILN_TEST(Provider, ArrayReloadsWhenASourceChanges) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_array_watch_src", root, sizeof root);
    scratch_dir("provider_array_watch_store", storeDir, sizeof storeDir);
    make_dir(root);

    u8 rgbaA[4 * 4 * 4], rgbaB[4 * 4 * 4], rgbaB2[4 * 4 * 4];
    test_pixels(rgbaA, 30);
    test_pixels(rgbaB, 40);
    test_pixels(rgbaB2, 50);
    char pathA[1100], pathB[1100];
    format(pathA, sizeof pathA, "%s/tileA.png", root);
    format(pathB, sizeof pathB, "%s/tileB.png", root);
    replace_file(pathA, test_png(rgbaA).span());
    replace_file(pathB, test_png(rgbaB).span());

    Root const roots[] = {
        {{}, StrView(root)}
    };
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<Root const>(roots, 1), {}, {.watchStore = true, .pollMs = 20}))
        return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    StrView const layers[] = {"tileA.png", "tileB.png"};
    TextureHandle const arr =
        request_texture_array(tc.ctx, {.name = "arr/tiles", .layers = Span<StrView const>(layers, 2)});
    KILN_REQUIRE(arr);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, arr), State::Ready);

    Vec<Event> seen(default_allocator(), Tag::Test);
    auto pump_and_collect = [&] {
        pump(tc.ctx, {});
        for (Event const& e : kiln::events(tc.ctx))
            seen.push_back(e);
    };
    for (int i = 0; i < 5; ++i)
        pump_and_collect();
    usize const ev0 = seen.size();

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // past file-time granularity
    replace_file(pathB, test_png(rgbaB2).span());

    bool changed = false;
    for (int i = 0; i < 500 && !changed; ++i) {
        pump_and_collect();
        for (usize k = ev0; k < seen.size() && !changed; ++k)
            changed = seen[k].kind == EventKind::Changed && seen[k].handle == arr.bits();
        if (!changed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cook::uninstall_provider(tc.ctx);
    KILN_CHECK_MSG(changed, "the array did not get a Changed event after tileB.png changed");
    release(tc.ctx, arr);
}
#endif
