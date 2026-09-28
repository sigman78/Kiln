// tests/test_provider.cpp — cook-on-miss provider (kiln/cook/provider.h) on a null-adapter Context;
// cook-only. Scratch stores and sources live under sample_dir(); models come from
// `<corpus_dir>/../gltf/generated` and `<corpus_dir>/../gltf/khronos`.
#include "image_fixtures.h"
#include "kiln_test.h"
#include "png_writer.h"

#include "kiln/assets.h"
#include "kiln/cook/provider.h"
#include "kiln/ktx2.h"
#include "kiln/null_adapter.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
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

void scratch_dir(char const* suffix, char* out, usize cap) {
    format(out, cap, "%s/%s", kiln::test::sample_dir(), suffix);
}

void gltf_khronos_dir(char* out, usize cap) {
    format(out, cap, "%s/../gltf/khronos", kiln::test::corpus_dir());
}

void gltf_generated_dir(char* out, usize cap) {
    format(out, cap, "%s/../gltf/generated", kiln::test::corpus_dir());
}

struct DiagCapture {
    u32 firstCode = 0;
    int count     = 0;

    static void fn(void* user, Diagnostic const& d) noexcept {
        auto* self = static_cast<DiagCapture*>(user);
        if (self->firstCode == 0) self->firstCode = d.code;
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

    bool init(StrView storeDir, Span<StrView const> roots, DiagSink diag = {}) noexcept {
        Result<NullAdapter*> na_ = null_adapter_create({}, &adapter);
        if (!KILN_CHECK_MSG(na_.ok(), "null_adapter_create failed")) return false;
        na = na_.value();

        ContextDesc desc{};
        desc.adapter     = &adapter;
        desc.storeDir    = storeDir;
        desc.sourceRoots = roots;
        desc.diag        = diag;

        Result<Context*> c = create(desc);
        if (!KILN_CHECK_MSG(c.ok(), "create() failed (%s)", code_name(c.code()))) return false;
        ctx = c.value();
        return true;
    }

    ~TestContext() noexcept {
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
bool wait_for_file(char const* path, int ms = 3000) {
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

cook::ProviderDesc const kWatchDesc{.storeMode = cook::StoreMode::Disk, .watchSources = true, .pollMs = 20};

} // namespace

KILN_TEST(Provider, EmptySourceRootsIsInvalidArgument) {
    char storeDir[1024];
    scratch_dir("provider_store_empty_roots", storeDir, sizeof storeDir);

    TestContext tc;
    if (!tc.init(StrView(storeDir), {})) return;

    Status st = cook::install_provider(tc.ctx, cook::ProviderDesc{});
    KILN_CHECK(st.failed());
    KILN_CHECK_EQ(st.code, Code::InvalidArgument);
}

// Disk mode writes the Named store layout; a second context without a provider loads those files.
KILN_TEST(Provider, DiskModeCooksAndWritesNamedStoreFiles) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    StrView const roots[] = {StrView(gltfDir)};

    {
        TestContext tc;
        if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;

        Status const installed =
            cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
        KILN_REQUIRE(installed.ok());

        MeshHandle const mesh = request_mesh(tc.ctx, "cube_basic");
        KILN_REQUIRE(mesh);
        KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Ready);

        char meshPath[1024];
        store_path(storeDir, "cube_basic.mesh", meshPath, sizeof meshPath);
        KILN_CHECK(file_exists(meshPath));

        // pbr_textures/hull_albedo has no source of its own: cooking it cooks its
        // owning mesh, writing the mesh plus every texture it references.
        TextureHandle const tex = request_texture(tc.ctx, "pbr_textures/hull_albedo");
        KILN_REQUIRE(tex);
        KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);

        char pbrMeshPath[1024];
        store_path(storeDir, "pbr_textures.mesh", pbrMeshPath, sizeof pbrMeshPath);
        KILN_CHECK(file_exists(pbrMeshPath));

        // Distinct *referenced* textures in generated/pbr_textures.glb (manifest.txt):
        // hull_albedo, hull_normal, hull_orm (bound twice, one file) and hull_emissive.
        // hull_height is an unreferenced images[] entry and is never cooked.
        static char const* const kTextures[] = {"hull_albedo", "hull_normal", "hull_orm", "hull_emissive"};
        for (char const* name : kTextures) {
            char rel[256];
            format(rel, sizeof rel, "pbr_textures/%s.ktx2", name);
            char path[1024];
            store_path(storeDir, rel, path, sizeof path);
            KILN_CHECK_MSG(file_exists(path), "missing %s", path);
        }
        char heightPath[1024];
        store_path(storeDir, "pbr_textures/hull_height.ktx2", heightPath, sizeof heightPath);
        KILN_CHECK_MSG(!file_exists(heightPath), "hull_height should never be cooked (unreferenced)");
    }

    // A second context on the same store, without a provider: loads straight
    // from the files the first context just wrote (cache reuse).
    {
        TestContext tc2;
        if (!tc2.init(StrView(storeDir), {})) return;

        MeshHandle const mesh = request_mesh(tc2.ctx, "cube_basic");
        KILN_REQUIRE(mesh);
        KILN_CHECK_EQ(pump_until_settled(tc2.ctx, mesh), State::Ready);

        TextureHandle const tex = request_texture(tc2.ctx, "pbr_textures/hull_albedo");
        KILN_REQUIRE(tex);
        KILN_CHECK_EQ(pump_until_settled(tc2.ctx, tex), State::Ready);
    }
}

// generated/jpeg_texture.glb has no source of its own for its "albedo" image (embedded
// JPEG): cooking it cooks the owning mesh, same path as pbr_textures/hull_albedo (PNG).
KILN_TEST(Provider, DiskModeCooksEmbeddedJpegTexture) {
    char storeDir[1024], gltfDir[1024];
    scratch_dir("provider_jpeg_embedded_store", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    StrView const roots[] = {StrView(gltfDir)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;
    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    TextureHandle const tex = request_texture(tc.ctx, "jpeg_texture/albedo");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);

    char meshPath[1024], texPath[1024];
    store_path(storeDir, "jpeg_texture.mesh", meshPath, sizeof meshPath);
    store_path(storeDir, "jpeg_texture/albedo.ktx2", texPath, sizeof texPath);
    KILN_CHECK(file_exists(meshPath));
    KILN_CHECK_MSG(file_exists(texPath), "cook-on-miss did not write %s", texPath);
}

// Memory mode: cache-less, cooks every miss, never touches the store.
KILN_TEST(Provider, MemoryModeNeverWritesTheStore) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store_memory", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    StrView const roots[] = {StrView(gltfDir)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;

    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Memory});
    KILN_REQUIRE(installed.ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "cube_basic");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Ready);

    char meshPath[1024];
    store_path(storeDir, "cube_basic.mesh", meshPath, sizeof meshPath);
    KILN_CHECK_MSG(!file_exists(meshPath), "Memory mode must not write %s", meshPath);
}

// Missing source: the asset Fails with a K5001 store-miss diagnostic.
KILN_TEST(Provider, MissingSourceFailsWithStoreMiss) {
    char storeDir[1024];
    char gltfDir[1024];
    scratch_dir("provider_store_missing", storeDir, sizeof storeDir);
    gltf_generated_dir(gltfDir, sizeof gltfDir);

    StrView const roots[] = {StrView(gltfDir)};
    DiagCapture diags;
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1), diags.sink())) return;

    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "does_not_exist_in_the_corpus");
    KILN_REQUIRE(mesh);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, mesh), State::Failed);
    KILN_CHECK_EQ(diags.firstCode, u32(kDiagStoreMiss));
}

// A texture whose only source is `<root>/tex.jpg` cooks like any other source extension
// (provider_cook's texExts tries png, jpg, jpeg, webp, ktx2 in order).
KILN_TEST(Provider, DiskModeCooksJpegSource) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_jpeg_src", root, sizeof root);
    scratch_dir("provider_jpeg_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100], texPath[1100];
    format(srcPath, sizeof srcPath, "%s/tex.jpg", root);
    format(texPath, sizeof texPath, "%s/tex.ktx2", storeDir);
    std::remove(texPath);
    replace_file(srcPath, kiln::test::img::kJpegGradientRgbBytes);

    StrView const roots[] = {StrView(root)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;
    Status const installed =
        cook::install_provider(tc.ctx, cook::ProviderDesc{.storeMode = cook::StoreMode::Disk});
    KILN_REQUIRE(installed.ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex");
    KILN_REQUIRE(tex);
    KILN_CHECK_EQ(pump_until_settled(tc.ctx, tex), State::Ready);
    KILN_CHECK_MSG(file_exists(texPath), "cook-on-miss did not write %s", texPath);
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
    format(storeFile, sizeof storeFile, "%s/tex.ktx2", storeDir);
    std::remove(storeFile); // left by an earlier run, it would load without a cook

    u8 first[4 * 4 * 4], second[4 * 4 * 4];
    test_pixels(first, 1);
    test_pixels(second, 100);
    replace_file(srcPath, test_png(first).span());

    StrView const roots[] = {StrView(root)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex");
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
    Span<u8 const> l0 = v->level_data(0);
    KILN_REQUIRE_EQ(l0.size, sizeof second);
    KILN_CHECK(std::memcmp(l0.data, second, sizeof second) == 0);
}

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
    format(meshFile, sizeof meshFile, "%s/box.mesh", storeDir);
    format(texFile, sizeof texFile, "%s/box/image_0.ktx2", storeDir); // BoxTextured's one unnamed image
    std::remove(meshFile);
    std::remove(texFile);
    copy_file(textured, srcPath);

    StrView const roots[] = {StrView(root)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    MeshHandle const mesh = request_mesh(tc.ctx, "box");
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

// After uninstall_provider the poller is gone: a changed source rewrites nothing.
KILN_TEST(Provider, SourcePollerStopsOnUninstall) {
    char root[1024], storeDir[1024];
    scratch_dir("provider_watch_stop_src", root, sizeof root);
    scratch_dir("provider_watch_stop_store", storeDir, sizeof storeDir);
    make_dir(root);

    char srcPath[1100], storeFile[1100];
    format(srcPath, sizeof srcPath, "%s/tex.png", root);
    format(storeFile, sizeof storeFile, "%s/tex.ktx2", storeDir);
    std::remove(storeFile);

    u8 first[4 * 4 * 4], second[4 * 4 * 4];
    test_pixels(first, 3);
    test_pixels(second, 200);
    replace_file(srcPath, test_png(first).span());

    StrView const roots[] = {StrView(root)};
    TestContext tc;
    if (!tc.init(StrView(storeDir), Span<StrView const>(roots, 1))) return;
    KILN_REQUIRE(cook::install_provider(tc.ctx, kWatchDesc).ok());

    TextureHandle const tex = request_texture(tc.ctx, "tex");
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
