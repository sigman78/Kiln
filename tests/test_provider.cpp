// Tests for the cook-on-miss provider (kiln/cook/provider.h). Cook-only; needs a
// runtime Context (kiln/assets.h) with the null adapter (kiln/null_adapter.h).
//
// Needs both `--samples <dir>` (a scratch store directory) and `--corpus <dir>`
// (the KTX2 corpus root; the glTF corpus used here is its sibling
// `<dir>/../gltf/generated`, see tests/test_mesh_cook.cpp); every test no-ops
// when either flag is missing, same convention as the rest of the cook suite.
#include "kiln_test.h"

#include "kiln/assets.h"
#include "kiln/cook/provider.h"
#include "kiln/null_adapter.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace kiln;

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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

/// `<sample_dir()>/<suffix>`, or false if `--samples <dir>` wasn't given.
bool scratch_dir(char const* suffix, char* out, usize cap) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return false;
    format(out, cap, "%s/%s", dir, suffix);
    return true;
}

/// `<corpus_dir()>/../gltf/generated`, or false if `--corpus <dir>` wasn't given.
bool gltf_generated_dir(char* out, usize cap) {
    char const* dir = kiln::test::corpus_dir();
    if (!dir) return false;
    format(out, cap, "%s/../gltf/generated", dir);
    return true;
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

} // namespace

// ---------------------------------------------------------------------------
// install_provider validation
// ---------------------------------------------------------------------------

KILN_TEST(Provider, EmptySourceRootsIsInvalidArgument) {
    char storeDir[1024];
    if (!scratch_dir("provider_store_empty_roots", storeDir, sizeof storeDir)) return;

    TestContext tc;
    if (!tc.init(StrView(storeDir), {})) return;

    Status st = cook::install_provider(tc.ctx, cook::ProviderDesc{});
    KILN_CHECK(st.failed());
    KILN_CHECK_EQ(st.code, Code::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Disk mode: cook-on-miss writes the Named store layout; a second context
// without a provider then loads the same files straight from the store.
// ---------------------------------------------------------------------------

KILN_TEST(Provider, DiskModeCooksAndWritesNamedStoreFiles) {
    char storeDir[1024];
    char gltfDir[1024];
    if (!scratch_dir("provider_store", storeDir, sizeof storeDir)) return;
    if (!gltf_generated_dir(gltfDir, sizeof gltfDir)) return;

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

// ---------------------------------------------------------------------------
// Memory mode: cache-less, cooks every miss, never touches the store.
// ---------------------------------------------------------------------------

KILN_TEST(Provider, MemoryModeNeverWritesTheStore) {
    char storeDir[1024];
    char gltfDir[1024];
    if (!scratch_dir("provider_store_memory", storeDir, sizeof storeDir)) return;
    if (!gltf_generated_dir(gltfDir, sizeof gltfDir)) return;

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

// ---------------------------------------------------------------------------
// Missing source: the asset Fails with a K5001 store-miss diagnostic.
// ---------------------------------------------------------------------------

KILN_TEST(Provider, MissingSourceFailsWithStoreMiss) {
    char storeDir[1024];
    char gltfDir[1024];
    if (!scratch_dir("provider_store_missing", storeDir, sizeof storeDir)) return;
    if (!gltf_generated_dir(gltfDir, sizeof gltfDir)) return;

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
