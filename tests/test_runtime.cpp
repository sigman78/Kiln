// tests/test_runtime.cpp — runtime (kiln/assets.h) through the null adapter.
// Golden-based tests use tests/golden as the store (`--golden <dir>`) and skip without it.
// RuntimePanic.* cases abort on purpose; they run only when selected by exact name (own CTest entries).
#include "kiln_test.h"

#include "ktx2_corpus.h" // corpus::read_file

#include "kiln/assets.h"
#include "kiln/null_adapter.h"
#include "kiln/placeholders.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif
#include <cerrno>

using namespace kiln;
using namespace kiln::literals;

namespace {

char const* const kGoldenMeshes[] = {
    "mesh/Box",           "mesh/BoxTextured",    "mesh/BoxVertexColors",  "mesh/MultiUVTest",
    "mesh/authored_lods", "mesh/cube_basic",     "mesh/external_uri",     "mesh/hierarchy_parts",
    "mesh/mounts_extras", "mesh/multi_material", "mesh/no_uv_no_normals", "mesh/non_triangle",
    "mesh/pbr_textures",  "mesh/two_uv_sets",    "mesh/u32_indices",
};
char const* const kGoldenTextures[] = {"ktx2/color_srgb", "ktx2/height16", "ktx2/normal"};

// Diagnostics collector (pump thread only).
struct DiagLog {
    u32 codes[64]  = {};
    u32 count      = 0;
    char last[512] = {};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLog*>(user);
        if (self->count < countof(self->codes)) self->codes[self->count++] = d.code;
        format(self->last, sizeof self->last, "K%u %.*s: %.*s", d.code, KILN_SV(d.asset), KILN_SV(d.message));
    }
    [[nodiscard]] bool has(u32 code) const {
        for (u32 i = 0; i < count; ++i)
            if (codes[i] == code) return true;
        return false;
    }
    [[nodiscard]] DiagSink sink() { return {&fn, this}; }
};

// Fixture: null adapter + context.
struct Rt {
    NullAdapter* na = nullptr;
    Adapter adapter;
    Context* ctx = nullptr;
    DiagLog diags;
    Vec<Event> events{default_allocator(), Tag::Test};

    Rt()                     = default;
    Rt(Rt const&)            = delete;
    Rt& operator=(Rt const&) = delete;
    ~Rt() { shutdown(); }

    bool init(NullAdapterDesc nd = {}, ContextDesc cd = {}) {
        Result<NullAdapter*> a = null_adapter_create(nd, &adapter);
        if (!KILN_CHECK(a.ok())) return false;
        na         = *a;
        cd.adapter = &adapter;
        cd.diag    = diags.sink();
        if (cd.storeDir.empty() && test::golden_dir()) cd.storeDir = test::golden_dir();
        Result<Context*> c = create(cd);
        if (!KILN_CHECK(c.ok())) return false;
        ctx = *c;
        return true;
    }
    void shutdown() {
        if (ctx) destroy(ctx);
        ctx = nullptr;
        if (na) null_adapter_destroy(na);
        na = nullptr;
    }

    PumpStats pump_once(PumpOptions const& opt = {}) {
        PumpStats const st = pump(ctx, opt);
        for (Event const& e : kiln::events(ctx))
            events.push_back(e);
        return st;
    }

    /// Pump (1 ms apart) until `done()` or ~10 s.
    template <class F> bool pump_until(F&& done, PumpOptions const& opt = {}) {
        for (int i = 0; i < 10000; ++i) {
            pump_once(opt);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    /// Index of the first event of `kind` for `bits` at or after `from`, or -1.
    [[nodiscard]] int find_event(EventKind kind, u64 bits, usize from = 0) const {
        for (usize i = from; i < events.size(); ++i)
            if (events[i].kind == kind && events[i].handle == bits) return int(i);
        return -1;
    }
};

bool read_golden(char const* rel, char const* ext, Vec<u8>& out) {
    char path[1024];
    format(path, sizeof path, "%s/%s%s", test::golden_dir(), rel, ext);
    return KILN_CHECK_MSG(test::corpus::read_file(path, out), "cannot read %s", path);
}

bool ensure_dir(char const* dir) {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(dir) == 0) return true;
#else
    if (mkdir(dir, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

bool bytes_equal(u8 const* a, u8 const* b, usize n) { return n == 0 || std::memcmp(a, b, n) == 0; }

/// The golden mesh's decoded payload, computed directly with the reader.
bool decoded_golden_mesh(char const* rel, Vec<u8>& file, Vec<u8>& decoded) {
    if (!read_golden(rel, ".mesh", file)) return false;
    Result<mesh::MeshView> v = mesh::MeshView::open(file.span());
    if (!KILN_CHECK(v.ok())) return false;
    decoded.resize(usize(v->decoded_size()));
    return KILN_CHECK(mesh::decode_payload(*v, v->encoded(), decoded.span()).ok());
}

void exit_on_panic(void*, char const* file, int line, char const* msg) {
    std::fprintf(stderr, "PANIC %s(%d): %s\n", file, line, msg);
    std::fflush(stderr);
    std::_Exit(3);
}

} // namespace

KILN_TEST(Runtime, AssetIdNormalization) {
    KILN_CHECK_EQ(asset_id("mesh/cube_basic"), "mesh/cube_basic"_h);
    KILN_CHECK_EQ(asset_id("mesh/cube_basic.mesh"), "mesh/cube_basic"_h);
    KILN_CHECK_EQ(asset_id("./mesh\\cube_basic.glb"), "mesh/cube_basic"_h);
    KILN_CHECK_EQ(asset_id("/mesh//./cube_basic"), "mesh/cube_basic"_h);
    KILN_CHECK_EQ(asset_id("a.b/c"), "a.b/c"_h);
    KILN_CHECK_EQ(asset_id("dir/.hidden"), "dir/.hidden"_h);
}

KILN_TEST(Runtime, CreateDestroyPlaceholders) {
    for (bool dev : {true, false}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.bindless = false;
        ContextDesc cd;
        cd.devPlaceholders = dev;
        cd.storeDir        = "does/not/exist";
        if (!rt.init(nd, cd)) return;
        NullAdapterStats st = null_adapter_stats(rt.na);
        KILN_CHECK_EQ(st.beginUploads, dev ? 5u : 4u);
        KILN_CHECK_EQ(st.completes, dev ? 5u : 4u);
        KILN_CHECK_EQ(st.publishes, dev ? 5u : 4u);

        // A fresh texture handle serves its kind placeholder (objects are created in kind order).
        RequestOptions ro;
        ro.textureKind  = TextureKind::Normal;
        TextureHandle t = request_texture(rt.ctx, "tex/missing", ro);
        KILN_REQUIRE(!t.is_null());
        KILN_CHECK_EQ(state(rt.ctx, t), State::Pending);
        GpuObject const g = gpu(rt.ctx, t);
        KILN_CHECK_EQ(g.native, u64(u32(TextureKind::Normal) + 1));
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(ti.isPlaceholder);
        KILN_CHECK_EQ(ti.desc.width, 1u);
        KILN_CHECK_EQ(ti.desc.format, Format::R8G8B8A8_UNORM);
        KILN_CHECK_EQ(null_adapter_payload(rt.na, g).size, usize(4));
        // Stale handle: failed placeholder in dev mode, BaseColor otherwise.
        GpuObject const stale = gpu(rt.ctx, TextureHandle{0, 77});
        KILN_CHECK_EQ(stale.native, dev ? u64(5) : u64(1));
        KILN_CHECK(gpu(rt.ctx, MeshHandle{0, 77}).is_null());
        KILN_CHECK_EQ(state(rt.ctx, MeshHandle{}), State::Unloaded);
        release(rt.ctx, t);
        KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
        rt.shutdown();
    }
}

KILN_TEST(Runtime, HostPlaceholderOverride) {
    Rt rt;
    NullAdapterDesc nd;
    nd.bindless            = false;
    nd.rowPitchAlign       = 256;
    u8 const px[2 * 2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    PlaceholderDesc pd;
    pd.kind   = TextureKind::Orm;
    pd.width  = 2;
    pd.height = 2;
    pd.pixels = Span<u8 const>(px, sizeof px);
    ContextDesc cd;
    cd.placeholders = Span<PlaceholderDesc const>(&pd, 1);
    if (!rt.init(nd, cd)) return;
    RequestOptions ro;
    ro.textureKind  = TextureKind::Orm;
    TextureHandle t = request_texture(rt.ctx, "tex/whatever", ro);
    TextureInfo ti  = texture_info(rt.ctx, t);
    KILN_CHECK_EQ(ti.desc.width, 2u);
    KILN_REQUIRE_EQ(ti.levelRowPitches.size, usize(1));
    KILN_CHECK_EQ(ti.levelRowPitches[0], u64(256));
    Span<u8 const> bytes = null_adapter_payload(rt.na, ti.gpu);
    KILN_REQUIRE_EQ(bytes.size, usize(512));
    KILN_CHECK(bytes_equal(bytes.data, px, 8));
    KILN_CHECK(bytes_equal(bytes.data + 256, px + 8, 8));
    release(rt.ctx, t);

    // A bad override (wrong pixel count) fails create() with K5009.
    NullAdapter* na2 = nullptr;
    Adapter a2;
    Result<NullAdapter*> r2 = null_adapter_create({}, &a2);
    KILN_REQUIRE(r2.ok());
    na2 = *r2;
    DiagLog dl;
    pd.pixels = Span<u8 const>(px, 3);
    ContextDesc bad;
    bad.adapter         = &a2;
    bad.diag            = dl.sink();
    bad.placeholders    = Span<PlaceholderDesc const>(&pd, 1);
    Result<Context*> c2 = create(bad);
    KILN_CHECK(c2.failed());
    KILN_CHECK(dl.has(kDiagPlaceholderFailed));
    null_adapter_destroy(na2);
}

KILN_TEST(Runtime, LoadMesh) {
    if (!test::golden_dir()) return;
    Rt rt;
    if (!rt.init()) return;
    MeshHandle m = request_mesh(rt.ctx, "mesh/cube_basic");
    KILN_REQUIRE(!m.is_null());
    KILN_CHECK_EQ(state(rt.ctx, m), State::Pending);
    KILN_CHECK(mesh_view(rt.ctx, m) == nullptr);
    KILN_CHECK(gpu(rt.ctx, m).is_null());
    KILN_CHECK_EQ(id_of(rt.ctx, m), "mesh/cube_basic"_h);

    bool sawMetaView = false;
    KILN_REQUIRE(rt.pump_until([&] {
        if (state(rt.ctx, m) == State::MetaReady)
            sawMetaView = sawMetaView || mesh_view(rt.ctx, m) != nullptr;
        return is_ready(rt.ctx, m);
    }));
    int const meta  = rt.find_event(EventKind::MetaReady, m.bits());
    int const ready = rt.find_event(EventKind::Ready, m.bits());
    KILN_CHECK(meta >= 0);
    KILN_CHECK(ready > meta);
    if (ready >= 0) KILN_CHECK_EQ(rt.events[usize(ready)].version, 1u);
    KILN_CHECK(has_meta(rt.ctx, m));
    mesh::MeshView const* v = mesh_view(rt.ctx, m);
    KILN_REQUIRE(v != nullptr);
    KILN_CHECK(v->parts().size() > 0);
    KILN_CHECK_EQ(version(rt.ctx, m), 1u);

    GpuObject const obj = gpu(rt.ctx, m);
    KILN_CHECK(!obj.is_null());
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/cube_basic", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, obj);
        KILN_CHECK_EQ(got.size, decoded.size());
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }

    // Refcount: same handle; one release keeps it loaded, the second unloads.
    MeshHandle m2 = request_mesh(rt.ctx, "mesh/cube_basic.mesh");
    KILN_CHECK(m2 == m);
    KILN_CHECK(find_mesh(rt.ctx, "mesh/cube_basic"_h) == m);
    KILN_CHECK(find_texture(rt.ctx, "mesh/cube_basic"_h).is_null());
    release(rt.ctx, m);
    KILN_CHECK_EQ(state(rt.ctx, m), State::Ready);
    u32 const destroysBefore = null_adapter_stats(rt.na).destroys;
    release(rt.ctx, m2);
    KILN_CHECK_EQ(state(rt.ctx, m), State::Unloaded);
    KILN_CHECK(mesh_view(rt.ctx, m) == nullptr);
    KILN_CHECK(find_mesh(rt.ctx, "mesh/cube_basic"_h).is_null());
    KILN_CHECK_EQ(null_adapter_flush_deferred(rt.na), 1u);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).destroys, destroysBefore + 1);

    // A new request gets a new generation (stale old handle stays Unloaded).
    MeshHandle m3 = request_mesh(rt.ctx, "mesh/cube_basic");
    KILN_CHECK(m3 != m);
    KILN_CHECK_EQ(state(rt.ctx, m), State::Unloaded);
    KILN_CHECK(rt.pump_until([&] { return is_ready(rt.ctx, m3); }));
    release(rt.ctx, m3);
}

// Tight rows and a 256-byte row pitch.
KILN_TEST(Runtime, LoadTexture) {
    if (!test::golden_dir()) return;
    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        nd.offsetAlign   = 64;
        if (!rt.init(nd)) return;
        TextureHandle t = request_texture(rt.ctx, "ktx2/color_srgb");
        KILN_REQUIRE(!t.is_null());
        KILN_CHECK(texture_info(rt.ctx, t).isPlaceholder);
        KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
        int const meta  = rt.find_event(EventKind::MetaReady, t.bits());
        int const ready = rt.find_event(EventKind::Ready, t.bits());
        KILN_CHECK(meta >= 0 && ready > meta);

        Vec<u8> file(default_allocator(), Tag::Test);
        if (!read_golden("ktx2/color_srgb", ".ktx2", file)) return;
        Result<ktx2::Ktx2View> kv = ktx2::Ktx2View::open(file.span());
        KILN_REQUIRE(kv.ok());
        ktx2::TextureDesc const want = kv->desc();

        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(!ti.isPlaceholder);
        KILN_CHECK_EQ(ti.version, 1u);
        KILN_CHECK_EQ(ti.desc.format, want.format);
        KILN_CHECK_EQ(ti.desc.width, want.width);
        KILN_CHECK_EQ(ti.desc.height, want.height);
        KILN_CHECK_EQ(ti.desc.levels, want.levels);
        KILN_REQUIRE_EQ(ti.levelOffsets.size, usize(want.levels));
        KILN_REQUIRE_EQ(ti.levelRowPitches.size, usize(want.levels));
        KILN_CHECK(want.levels > 1);

        Span<u8 const> payload = null_adapter_payload(rt.na, ti.gpu);
        KILN_REQUIRE(!payload.empty());
        for (u32 i = 0; i < want.levels; ++i) {
            u64 const off   = ti.levelOffsets[i];
            u64 const pitch = ti.levelRowPitches[i];
            KILN_CHECK_EQ(off % 64, u64(0));
            if (i > 0) KILN_CHECK(off > ti.levelOffsets[i - 1]);
            KILN_CHECK_EQ(pitch % pitchAlign, u64(0));
            u64 const rowBytes     = format_row_bytes(want.format, kv->level_width(i));
            u32 const rows         = kv->level_height(i);
            Span<u8 const> const L = kv->level_data(i);
            KILN_REQUIRE_EQ(L.size, usize(rowBytes * rows));
            KILN_REQUIRE(off + pitch * rows <= payload.size);
            bool same = true;
            for (u32 r = 0; r < rows; ++r)
                same = same &&
                       bytes_equal(payload.data + off + r * pitch, L.data + r * rowBytes, usize(rowBytes));
            KILN_CHECK_MSG(same, "level %u rows differ (pitch align %llu)", i,
                           static_cast<unsigned long long>(pitchAlign));
        }
        release(rt.ctx, t);
    }
}

KILN_TEST(Runtime, GoldenFolderGroupWait) {
    if (!test::golden_dir()) return;
    Rt rt;
    if (!rt.init()) return;
    Group g = group(rt.ctx);
    KILN_REQUIRE(!g.is_null());
    RequestOptions ro;
    ro.group = g;
    u32 n    = 0;
    MeshHandle meshes[countof(kGoldenMeshes)];
    TextureHandle textures[countof(kGoldenTextures)];
    for (usize i = 0; i < countof(kGoldenMeshes); ++i, ++n)
        meshes[i] = request_mesh(rt.ctx, kGoldenMeshes[i], ro);
    for (usize i = 0; i < countof(kGoldenTextures); ++i, ++n)
        textures[i] = request_texture(rt.ctx, kGoldenTextures[i], ro);

    GroupStatus st = progress(rt.ctx, g);
    KILN_CHECK_EQ(st.pending, n);
    KILN_CHECK_EQ(st.ready, 0u);
    KILN_CHECK(!st.settled());

    WaitOptions wo;
    wo.timeoutMs = 30000;
    st           = wait(rt.ctx, g, wo);
    KILN_CHECK(st.settled());
    KILN_CHECK_EQ(st.failed, 0u);
    KILN_CHECK_EQ(st.ready, n);
    KILN_CHECK(st.bytesTotal > 0);
    KILN_CHECK_EQ(st.bytesDone, st.bytesTotal);
    // wait()'s pumps accumulate events: every asset reported MetaReady and Ready.
    u32 readyEvents = 0;
    for (Event const& e : events(rt.ctx))
        readyEvents += e.kind == EventKind::Ready ? 1u : 0u;
    KILN_CHECK_EQ(readyEvents, n);
    for (MeshHandle m : meshes)
        KILN_CHECK(is_ready(rt.ctx, m));
    for (TextureHandle t : textures)
        KILN_CHECK(is_ready(rt.ctx, t));
    ContextStats cs = stats(rt.ctx);
    KILN_CHECK_EQ(cs.ready, n);
    KILN_CHECK_EQ(cs.groups, 1u);

    // Releasing a member leaves the group; releasing the group keeps the members.
    release(rt.ctx, meshes[0]);
    KILN_CHECK_EQ(progress(rt.ctx, g).ready, n - 1);
    release(rt.ctx, g);
    KILN_CHECK_EQ(progress(rt.ctx, g).ready, 0u);
    KILN_CHECK(is_ready(rt.ctx, meshes[1]));
    for (usize i = 1; i < countof(meshes); ++i)
        release(rt.ctx, meshes[i]);
    for (TextureHandle t : textures)
        release(rt.ctx, t);
    KILN_CHECK_EQ(stats(rt.ctx).assets, 0u);
}

KILN_TEST(Runtime, WaitTimeoutAndMixedGroup) {
    if (!test::golden_dir()) return;
    Rt rt;
    if (!rt.init()) return;
    Group g = group(rt.ctx);
    RequestOptions ro;
    ro.group        = g;
    MeshHandle ok   = request_mesh(rt.ctx, "mesh/Box", ro);
    MeshHandle miss = request_mesh(rt.ctx, "mesh/not_there", ro);
    WaitOptions wo;
    wo.timeoutMs         = 30000;
    GroupStatus const st = wait(rt.ctx, g, wo);
    KILN_CHECK(st.settled());
    KILN_CHECK_EQ(st.ready, 1u);
    KILN_CHECK_EQ(st.failed, 1u);
    KILN_CHECK_EQ(state(rt.ctx, miss), State::Failed);
    // A stale group settles at once with zero counts.
    release(rt.ctx, g);
    GroupStatus const z = wait(rt.ctx, g, wo);
    KILN_CHECK(z.settled() && z.ready == 0);
    release(rt.ctx, ok);
    release(rt.ctx, miss);
}

KILN_TEST(Runtime, MissingAssetFails) {
    for (bool dev : {true, false}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.bindless = false;
        ContextDesc cd;
        cd.devPlaceholders = dev;
        cd.storeDir        = test::golden_dir() ? test::golden_dir() : "no/such/store";
        if (!rt.init(nd, cd)) return;
        TextureHandle t = request_texture(rt.ctx, "tex/nope");
        KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
        KILN_CHECK(rt.diags.has(kDiagStoreMiss));
        int const f = rt.find_event(EventKind::Failed, t.bits());
        KILN_REQUIRE(f >= 0);
        KILN_CHECK_EQ(rt.events[usize(f)].status.code, Code::NotFound);
        KILN_CHECK(!has_meta(rt.ctx, t));
        GpuObject const g = gpu(rt.ctx, t);
        KILN_CHECK_EQ(g.native, dev ? u64(5) : u64(1)); // failed checker vs BaseColor placeholder
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(ti.isPlaceholder);
        KILN_CHECK_EQ(ti.desc.width, dev ? 8u : 1u);
        release(rt.ctx, t);
    }
}

KILN_TEST(Runtime, CorruptStoreFileFails) {
    if (!test::sample_dir()) return;
    char dir[1024], path[1024];
    format(dir, sizeof dir, "%s/store", test::sample_dir());
    KILN_REQUIRE(ensure_dir(dir));
    format(path, sizeof path, "%s/bad.mesh", dir);
    std::FILE* f = std::fopen(path, "wb");
    KILN_REQUIRE(f != nullptr);
    u8 garbage[300];
    for (usize i = 0; i < sizeof garbage; ++i)
        garbage[i] = u8(i * 37 + 11);
    std::fwrite(garbage, 1, sizeof garbage, f);
    std::fclose(f);
    format(path, sizeof path, "%s/short.ktx2", dir);
    f = std::fopen(path, "wb");
    KILN_REQUIRE(f != nullptr);
    std::fwrite(ktx2::kIdentifier, 1, sizeof ktx2::kIdentifier, f);
    std::fclose(f);

    Rt rt;
    ContextDesc cd;
    cd.storeDir = dir;
    if (!rt.init({}, cd)) return;
    MeshHandle m    = request_mesh(rt.ctx, "bad");
    TextureHandle t = request_texture(rt.ctx, "short");
    KILN_REQUIRE(rt.pump_until(
        [&] { return state(rt.ctx, m) == State::Failed && state(rt.ctx, t) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAssetLoadFailed));
    KILN_CHECK(!rt.diags.has(kDiagStoreMiss));
    KILN_CHECK(gpu(rt.ctx, m).is_null());
    release(rt.ctx, m);
    release(rt.ctx, t);
}

KILN_TEST(Runtime, AdapterRejectFails) {
    if (!test::golden_dir()) return;
    Rt rt2;
    NullAdapterDesc nd2;
    nd2.failEveryN = 6; // the 5 placeholder uploads succeed, the 6th (the mesh) fails
    ContextDesc cd;
    cd.devPlaceholders = true;
    if (!rt2.init(nd2, cd)) return;
    MeshHandle m = request_mesh(rt2.ctx, "mesh/Box");
    KILN_REQUIRE(rt2.pump_until([&] { return state(rt2.ctx, m) == State::Failed; }));
    KILN_CHECK(rt2.diags.has(kDiagAdapterRejected));
    KILN_CHECK(rt2.find_event(EventKind::MetaReady, m.bits()) >= 0);
    release(rt2.ctx, m);
}

KILN_TEST(Runtime, BusyBackPressure) {
    if (!test::golden_dir()) return;
    Rt rt;
    NullAdapterDesc nd;
    nd.busyEveryN = 2;
    if (!rt.init(nd)) return;
    MeshHandle meshes[countof(kGoldenMeshes)];
    for (usize i = 0; i < countof(kGoldenMeshes); ++i)
        meshes[i] = request_mesh(rt.ctx, kGoldenMeshes[i]);
    TextureHandle t = request_texture(rt.ctx, "ktx2/normal");
    KILN_REQUIRE(rt.pump_until([&] {
        for (MeshHandle m : meshes)
            if (!is_ready(rt.ctx, m)) return false;
        return is_ready(rt.ctx, t);
    }));
    KILN_CHECK(null_adapter_stats(rt.na).busyReturned > 0);
    for (MeshHandle m : meshes)
        release(rt.ctx, m);
    release(rt.ctx, t);

    // PumpStats::busyRetries: collected per pump.
    Rt rt2;
    if (!rt2.init(nd)) return;
    MeshHandle a = request_mesh(rt2.ctx, "mesh/Box");
    MeshHandle b = request_mesh(rt2.ctx, "mesh/cube_basic");
    u32 retries  = 0;
    u32 started  = 0;
    u64 bytes    = 0;
    for (int i = 0; i < 10000 && !(is_ready(rt2.ctx, a) && is_ready(rt2.ctx, b)); ++i) {
        PumpStats const ps = rt2.pump_once();
        retries += ps.busyRetries;
        started += ps.uploadsStarted;
        bytes += ps.uploadBytes;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    KILN_CHECK(is_ready(rt2.ctx, a) && is_ready(rt2.ctx, b));
    KILN_CHECK(retries > 0);
    // Retries consume budget but are not counted as new uploads: one per mesh, payload bytes once.
    KILN_CHECK_EQ(started, 2u);
    KILN_CHECK_EQ(bytes, mesh_view(rt2.ctx, a)->decoded_size() + mesh_view(rt2.ctx, b)->decoded_size());
    release(rt2.ctx, a);
    release(rt2.ctx, b);
}

KILN_TEST(Runtime, UploadBudget) {
    if (!test::golden_dir()) return;
    Rt rt;
    if (!rt.init()) return;
    MeshHandle meshes[6];
    for (usize i = 0; i < countof(meshes); ++i)
        meshes[i] = request_mesh(rt.ctx, kGoldenMeshes[i]);
    PumpOptions po;
    po.uploadBytes = 1;
    u32 pumps = 0, started = 0;
    bool within = true;
    for (int i = 0; i < 10000; ++i) {
        PumpStats const ps = rt.pump_once(po);
        ++pumps;
        started += ps.uploadsStarted;
        within   = within && ps.uploadsStarted <= 1;
        bool all = true;
        for (MeshHandle m : meshes)
            all = all && is_ready(rt.ctx, m);
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    KILN_CHECK(within);
    KILN_CHECK_EQ(started, u32(countof(meshes)));
    KILN_CHECK(pumps > countof(meshes));
    for (MeshHandle m : meshes)
        KILN_CHECK(is_ready(rt.ctx, m));
    for (MeshHandle m : meshes)
        release(rt.ctx, m);
}

struct FakeProvider {
    std::atomic<u32> calls{0};
    Vec<u8> bytes{default_allocator(), Tag::Test};

    static Status cook(void* user, AssetKind kind, StrView path, Allocator const* alloc, Vec<u8>* out,
                       DiagSink const* diag) {
        auto* self = static_cast<FakeProvider*>(user);
        self->calls.fetch_add(1);
        if (kind != AssetKind::Mesh || path != "virtual/cube")
            return diagf(diag, make_status(Code::NotFound), 1001, Severity::Error, path, "provider",
                         "no source");
        Vec<u8> v(alloc, Tag::Payload);
        v.append(self->bytes.span());
        *out = std::move(v);
        return kOk;
    }
};

KILN_TEST(Runtime, CookProviderOnMiss) {
    if (!test::golden_dir()) return;
    FakeProvider fp;
    if (!read_golden("mesh/cube_basic", ".mesh", fp.bytes)) return;
    Rt rt;
    if (!rt.init()) return;
    set_cook_provider(rt.ctx, CookProvider{&FakeProvider::cook, &fp});
    MeshHandle m    = request_mesh(rt.ctx, "virtual/cube");
    MeshHandle miss = request_mesh(rt.ctx, "virtual/none");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m) && state(rt.ctx, miss) == State::Failed; }));
    KILN_CHECK_EQ(fp.calls.load(), 2u);
    KILN_CHECK(rt.diags.has(kDiagStoreMiss));
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/cube_basic", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, gpu(rt.ctx, m));
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }
    // A store hit never calls the provider.
    MeshHandle hit = request_mesh(rt.ctx, "mesh/Box");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, hit); }));
    KILN_CHECK_EQ(fp.calls.load(), 2u);
    release(rt.ctx, m);
    release(rt.ctx, miss);
    release(rt.ctx, hit);
}

KILN_TEST(Runtime, RegisterInMemory) {
    if (!test::golden_dir()) return;
    Vec<u8> meshBytes(default_allocator(), Tag::Test), texBytes(default_allocator(), Tag::Test);
    if (!read_golden("mesh/multi_material", ".mesh", meshBytes)) return;
    if (!read_golden("ktx2/color_srgb", ".ktx2", texBytes)) return;
    Rt rt;
    NullAdapterDesc nd;
    nd.rowPitchAlign = 256;
    if (!rt.init(nd)) return;
    MeshHandle m    = register_mesh(rt.ctx, "gen/mesh", meshBytes.span());
    TextureHandle t = register_texture(rt.ctx, "gen/tex", texBytes.span());
    KILN_REQUIRE(!m.is_null() && !t.is_null());
    KILN_CHECK(find_mesh(rt.ctx, asset_id("gen/mesh")) == m);
    KILN_CHECK(find_texture(rt.ctx, asset_id("gen/tex.ktx2")) == t);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m) && is_ready(rt.ctx, t); }));
    KILN_CHECK(rt.find_event(EventKind::MetaReady, m.bits()) >= 0);
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/multi_material", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, gpu(rt.ctx, m));
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }
    TextureInfo const ti = texture_info(rt.ctx, t);
    KILN_CHECK(!ti.isPlaceholder);
    KILN_CHECK(ti.levelRowPitches.size > 0 && ti.levelRowPitches[0] % 256 == 0);

    // Duplicates and garbage are rejected with a diagnostic.
    KILN_CHECK(register_mesh(rt.ctx, "gen/mesh", meshBytes.span()).is_null());
    KILN_CHECK(rt.diags.has(kDiagDuplicateRegister));
    u8 junk[64] = {};
    KILN_CHECK(register_mesh(rt.ctx, "gen/junk", Span<u8 const>(junk, sizeof junk)).is_null());
    KILN_CHECK(register_texture(rt.ctx, "gen/junk", Span<u8 const>(junk, sizeof junk)).is_null());
    KILN_CHECK(find_mesh(rt.ctx, asset_id("gen/junk")).is_null());
    // request_* of a registered path shares the handle.
    KILN_CHECK(request_mesh(rt.ctx, "gen/mesh") == m);
    release(rt.ctx, m);
    release(rt.ctx, m);
    release(rt.ctx, t);
    KILN_CHECK(find_mesh(rt.ctx, asset_id("gen/mesh")).is_null());
}

// Zombie slots, then registry reuse.
KILN_TEST(Runtime, ReleaseWhileLoading) {
    if (!test::golden_dir()) return;
    Rt rt;
    ContextDesc cd;
    cd.maxAssets = 4;
    if (!rt.init({}, cd)) return;
    for (int round = 0; round < 20; ++round) {
        MeshHandle m    = request_mesh(rt.ctx, "mesh/authored_lods");
        TextureHandle t = request_texture(rt.ctx, "ktx2/color_srgb");
        for (int i = 0; i < round % 4; ++i)
            rt.pump_once(); // stop at various pipeline stages
        release(rt.ctx, m);
        release(rt.ctx, t);
        KILN_CHECK_EQ(state(rt.ctx, m), State::Unloaded);
        KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
    }
    // Everything drains; the registry is reusable.
    KILN_CHECK(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
    MeshHandle m = request_mesh(rt.ctx, "mesh/Box");
    KILN_REQUIRE(!m.is_null());
    KILN_CHECK(rt.pump_until([&] { return is_ready(rt.ctx, m); }));
    // Registry full -> null handle + K5005.
    MeshHandle extra[4];
    u32 got = 0;
    for (usize i = 0; i < countof(extra); ++i) {
        extra[i] = request_mesh(rt.ctx, kGoldenMeshes[i + 5]);
        got += extra[i].is_null() ? 0u : 1u;
    }
    KILN_CHECK(got < countof(extra));
    KILN_CHECK(rt.diags.has(kDiagRegistryFull));
    for (MeshHandle e : extra)
        if (!e.is_null()) release(rt.ctx, e);
    release(rt.ctx, m);
}

KILN_TEST(Runtime, EventOverflowDropsOldest) {
    if (!test::golden_dir()) return;
    Rt rt;
    ContextDesc cd;
    cd.maxEvents = 2;
    if (!rt.init({}, cd)) return;
    Group g = group(rt.ctx);
    RequestOptions ro;
    ro.group = g;
    MeshHandle m[3];
    for (usize i = 0; i < countof(m); ++i)
        m[i] = request_mesh(rt.ctx, kGoldenMeshes[i], ro);
    WaitOptions wo;
    wo.timeoutMs         = 30000;
    GroupStatus const st = wait(rt.ctx, g, wo); // 6 events accumulate into a 2-event buffer
    KILN_CHECK_EQ(st.ready, 3u);
    KILN_CHECK_EQ(events(rt.ctx).size, usize(2));
    KILN_CHECK(rt.diags.has(kDiagEventsDropped));
    for (MeshHandle h : m)
        release(rt.ctx, h);
}

KILN_TEST(Runtime, BindlessPublish) {
    if (!test::golden_dir()) return;
    Rt rt;
    NullAdapterDesc nd;
    nd.bindless = true;
    if (!rt.init(nd)) return;
    TextureHandle t          = request_texture(rt.ctx, "ktx2/normal");
    GpuObject const acquired = gpu(rt.ctx, t);
    KILN_REQUIRE(acquired.slot != kInvalid);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    GpuObject const real = gpu(rt.ctx, t);
    KILN_CHECK_EQ(null_adapter_slot(rt.na, acquired.slot).native, real.native);
    release(rt.ctx, t);
    KILN_CHECK(null_adapter_slot(rt.na, acquired.slot).is_null()); // publish(id, null) at unload
}

// No allocation in pump() or the queries.
KILN_TEST(Runtime, SteadyStateNoAllocation) {
    if (!test::golden_dir()) return;
    Rt rt;
    if (!rt.init()) return;
    MeshHandle meshes[4];
    for (usize i = 0; i < countof(meshes); ++i)
        meshes[i] = request_mesh(rt.ctx, kGoldenMeshes[i]);
    TextureHandle t = request_texture(rt.ctx, "ktx2/height16");
    KILN_REQUIRE(rt.pump_until([&] {
        for (MeshHandle m : meshes)
            if (!is_ready(rt.ctx, m)) return false;
        return is_ready(rt.ctx, t);
    }));
    rt.events.reserve(rt.events.size() + 64);
    AllocStats const reg0 = default_alloc_stats(Tag::Registry);
    AllocStats const pay0 = default_alloc_stats(Tag::Payload);
    AllocStats const io0  = default_alloc_stats(Tag::Io);
    u64 sink              = 0;
    for (int i = 0; i < 100; ++i) {
        PumpStats const ps = pump(rt.ctx);
        sink += ps.completed + events(rt.ctx).size;
        for (MeshHandle m : meshes) {
            sink += u64(state(rt.ctx, m)) + gpu(rt.ctx, m).native + (mesh_view(rt.ctx, m) ? 1 : 0);
            sink += find_mesh(rt.ctx, id_of(rt.ctx, m)).bits();
        }
        sink += texture_info(rt.ctx, t).levelOffsets.size + gpu(rt.ctx, t).native;
        sink += progress(rt.ctx, Group{}).pending + stats(rt.ctx).ready;
    }
    AllocStats const reg1 = default_alloc_stats(Tag::Registry);
    AllocStats const pay1 = default_alloc_stats(Tag::Payload);
    AllocStats const io1  = default_alloc_stats(Tag::Io);
    KILN_CHECK_EQ(reg1.allocCount, reg0.allocCount);
    KILN_CHECK_EQ(pay1.allocCount, pay0.allocCount);
    KILN_CHECK_EQ(io1.allocCount, io0.allocCount);
    KILN_CHECK(sink > 0);
    std::printf("  steady state: Registry allocs %llu (%llu bytes live), Payload allocs %llu, 100 pumps: "
                "+%llu/+%llu\n",
                static_cast<unsigned long long>(reg1.allocCount),
                static_cast<unsigned long long>(reg1.bytesCurrent),
                static_cast<unsigned long long>(pay1.allocCount),
                static_cast<unsigned long long>(reg1.allocCount - reg0.allocCount),
                static_cast<unsigned long long>(pay1.allocCount - pay0.allocCount));
    for (MeshHandle m : meshes)
        release(rt.ctx, m);
    release(rt.ctx, t);
}

KILN_TEST(RuntimePanic, WaitOffThread) {
    if (!test::selected_exactly("RuntimePanic.WaitOffThread")) return;
    set_panic_handler(&exit_on_panic, nullptr);
    Rt rt;
    ContextDesc cd;
    cd.storeDir = "nowhere";
    if (!rt.init({}, cd)) return;
    Group g = group(rt.ctx);
    RequestOptions ro;
    ro.group        = g;
    TextureHandle t = request_texture(rt.ctx, "tex/a", ro);
    (void)t;
    pump(rt.ctx); // binds the pump thread
    std::thread other([&] { (void)wait(rt.ctx, g); });
    other.join();
    KILN_CHECK_MSG(false, "wait() off the pump thread did not panic");
}

KILN_TEST(RuntimePanic, WaitNotSelfSubmitting) {
    if (!test::selected_exactly("RuntimePanic.WaitNotSelfSubmitting")) return;
    set_panic_handler(&exit_on_panic, nullptr);
    Adapter a;
    Result<NullAdapter*> na = null_adapter_create({}, &a);
    KILN_REQUIRE(na.ok());
    a.caps = 0; // uploads "need a frame"
    ContextDesc cd;
    cd.adapter         = &a;
    cd.storeDir        = "nowhere";
    Result<Context*> c = create(cd);
    KILN_REQUIRE(c.ok());
    Group g = group(*c);
    RequestOptions ro;
    ro.group = g;
    (void)request_texture(*c, "tex/a", ro);
    (void)wait(*c, g);
    KILN_CHECK_MSG(false, "wait() without kSelfSubmitting did not panic");
    destroy(*c);
    null_adapter_destroy(*na);
}
