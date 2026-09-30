// tests/test_runtime.cpp — runtime (kiln/assets.h) through the null adapter.
// Tests that leave ContextDesc::storeDir empty use golden_store_dir(), the goldens as a catalog store.
// RuntimePanic.* cases abort on purpose; they run only when selected by exact name (own CTest entries).
#include "kiln_test.h"

#include "hand_store.h"
#include "ktx2_corpus.h" // corpus::read_file

#include "kiln/assets.h"
#include "kiln/manifest.h"
#include "kiln/null_adapter.h"
#include "kiln/placeholders.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

using namespace kiln;
using namespace kiln::literals;

namespace {

char const* const kGoldenMeshes[] = {
    "mesh/Box",           "mesh/BoxTextured",    "mesh/BoxVertexColors",  "mesh/MultiUVTest",
    "mesh/authored_lods", "mesh/cube_basic",     "mesh/external_uri",     "mesh/hierarchy_parts",
    "mesh/mounts_extras", "mesh/multi_material", "mesh/no_uv_no_normals", "mesh/non_triangle",
    "mesh/pbr_textures",  "mesh/two_uv_sets",    "mesh/u32_indices",
};
char const* const kGoldenTextures[] = {"ktx2/color_srgb", "ktx2/color_zstd", "ktx2/height16", "ktx2/normal"};

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
        if (cd.storeDir.empty()) cd.storeDir = test::golden_store_dir();
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

// A name is compared byte for byte: no normalization, and a name that breaks a rule has no id.
KILN_TEST(Runtime, AssetNameRules) {
    KILN_CHECK_EQ(asset_id("mesh/cube_basic.glb"), "mesh/cube_basic.glb"_h);
    KILN_CHECK(asset_id("mesh/cube_basic.glb") != asset_id("mesh/cube_basic"));
    KILN_CHECK_EQ(asset_id("pool:tex/wood.png"), "pool:tex/wood.png"_h);
    KILN_CHECK_EQ(asset_id("dir/.hidden"), "dir/.hidden"_h);

    for (char const* ok : {"a", "a.b/c", "props/chair.glb#wood", "pool:tex/wood.png", "m_2:x.glb#image0",
                           "d\xc3\xa9j\xc3\xa0/vu.png", "pool:@x/a.png", "tex/@2x/a.png"})
        KILN_CHECK_MSG(check_asset_name(StrView(ok)) == nullptr, "'%s' should be valid", ok);
    for (char const* bad :
         {"",         "/abs.png", "a//b.png",   "dir/",          "./a.png",        "a/../b.png",
          "a\\b.png", "x:",       "c:/tex.png", "Pool:a.png",    "a:b:c.png",      "a.glb#",
          "#sub",     "dir/#sub", "a.glb#x#y",  "a#b/c.png",     "a?.png",         "a*.png",
          "a<b.png",  "a|b.png",  "a\"b.png",   "tab\there.png", "@pool/wood.png", "@x.png"})
        KILN_CHECK_MSG(check_asset_name(StrView(bad)) != nullptr, "'%s' should be invalid", bad);
    char longName[300];
    std::memset(longName, 'a', sizeof longName);
    KILN_CHECK(check_asset_name(StrView(longName, kMaxAssetNameLen)) == nullptr);
    KILN_CHECK(check_asset_name(StrView(longName, kMaxAssetNameLen + 1)) != nullptr);
    KILN_CHECK_EQ(asset_id("./a.png"), AssetId(0));

    AssetNameParts const p = split_asset_name("pool:props/chair.glb#wood");
    KILN_CHECK(p.root == "pool" && p.path == "props/chair.glb" && p.sub == "wood");
    AssetNameParts const d = split_asset_name("chair.glb");
    KILN_CHECK(d.root.empty() && d.path == "chair.glb" && d.sub.empty());
}

KILN_TEST(Runtime, ResolveAssetName) {
    char buf[256];
    auto const resolve = [&buf](StrView owner, StrView uri) noexcept {
        return StrView(buf, resolve_asset_name(owner, uri, buf, sizeof buf));
    };
    KILN_CHECK(resolve("props/chair.glb", "wood.png") == "props/wood.png");
    KILN_CHECK(resolve("props/chair.glb", "./tex/wood.png") == "props/tex/wood.png");
    KILN_CHECK(resolve("props/chair.glb", "../tex/wood.png") == "tex/wood.png");
    KILN_CHECK(resolve("chair.glb", "wood.png") == "wood.png");
    KILN_CHECK(resolve("pool:props/chair.glb", "../wood.png") == "pool:wood.png");
    // Leaving the root, absolute URIs and invalid results give 0.
    KILN_CHECK(resolve("chair.glb", "../wood.png").empty());
    KILN_CHECK(resolve("pool:chair.glb", "../wood.png").empty());
    KILN_CHECK(resolve("chair.glb", "/wood.png").empty());
    KILN_CHECK(resolve("chair.glb", "C:/wood.png").empty());
    KILN_CHECK(resolve("chair.glb", "http://x/wood.png").empty());
    KILN_CHECK(resolve("chair.glb", "a//wood.png").empty());
    KILN_CHECK(resolve("chair.glb", "wood?.png").empty());
    KILN_CHECK(resolve("chair.glb", "").empty());
    KILN_CHECK(resolve("./chair.glb", "wood.png").empty());
    KILN_CHECK_EQ(resolve_asset_name("props/chair.glb", "wood.png", buf, 8), usize(0));
}

KILN_TEST(Runtime, InvalidNamesAndMountsAreRejected) {
    Rt rt;
    if (!rt.init()) return;
    KILN_CHECK(request_mesh(rt.ctx, "./mesh/cube_basic").is_null());
    KILN_CHECK(rt.diags.has(kDiagBadAssetName));
    KILN_CHECK(request_texture(rt.ctx, "tex\\a.png").is_null());
    u8 junk[16] = {};
    KILN_CHECK(register_mesh(rt.ctx, "a//b", Span<u8 const>(junk, sizeof junk)).is_null());
    KILN_CHECK_EQ(stats(rt.ctx).assets, 0u);

    for (StrView const name : {StrView("X"), StrView("Pool"), StrView("a-b")}) {
        Root const bad[] = {
            {name, "src"}
        };
        Rt r2;
        Result<NullAdapter*> a = null_adapter_create({}, &r2.adapter);
        KILN_REQUIRE(a.ok());
        r2.na = *a;
        ContextDesc cd{.diag = r2.diags.sink(), .adapter = &r2.adapter, .roots = Span<Root const>(bad, 1)};
        Result<Context*> c = create(cd);
        KILN_CHECK_MSG(c.failed(), "root name '%.*s' should be rejected", KILN_SV(name));
        if (c.ok()) destroy(*c);
        KILN_CHECK(r2.diags.has(kDiagBadAssetName));
    }
    Root const twice[] = {
        {"lib", "a"},
        {"lib", "b"}
    };
    Rt r3;
    Result<NullAdapter*> a = null_adapter_create({}, &r3.adapter);
    KILN_REQUIRE(a.ok());
    r3.na              = *a;
    Result<Context*> c = create(ContextDesc{.adapter = &r3.adapter, .roots = Span<Root const>(twice, 2)});
    KILN_CHECK(c.failed());
    if (c.ok()) destroy(*c);
}

KILN_TEST(Runtime, CreateDestroyPlaceholders) {
    for (bool dev : {true, false}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.bindlessSlots = 0;
        ContextDesc cd;
        cd.devPlaceholders = dev;
        cd.storeDir        = "does/not/exist";
        if (!rt.init(nd, cd)) return;
        NullAdapterStats st = null_adapter_stats(rt.na);
        // 4 kinds + the Failed checker, for each of Tex2D, Cube and Array.
        KILN_CHECK_EQ(st.beginUploads, dev ? 15u : 12u);
        KILN_CHECK_EQ(st.completes, dev ? 15u : 12u);
        KILN_CHECK_EQ(st.binds, 0u);

        // A fresh texture handle serves its kind placeholder (objects are created in kind order).
        RequestOptions ro;
        ro.textureKind  = TextureKind::Normal;
        TextureHandle t = request_texture(rt.ctx, "tex/missing", ro);
        KILN_REQUIRE(!t.is_null());
        KILN_CHECK_EQ(state(rt.ctx, t), State::Pending);
        GpuObject const g = gpu_object(rt.ctx, t);
        KILN_CHECK_EQ(g.native, u64(u32(TextureKind::Normal) + 1));
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(ti.isPlaceholder);
        KILN_CHECK_EQ(ti.desc.width, 1u);
        KILN_CHECK_EQ(ti.desc.format, Format::R8G8B8A8_UNORM);
        KILN_CHECK_EQ(null_adapter_payload(rt.na, g).size, usize(4));
        // placeholder_object() gives the same object without a handle, per kind and shape.
        KILN_CHECK_EQ(placeholder_object(rt.ctx, TextureKind::Normal).native, g.native);
        KILN_CHECK(placeholder_object(rt.ctx, TextureKind::Emissive, TextureShape::Cube).native !=
                   placeholder_object(rt.ctx, TextureKind::Emissive).native);
        KILN_CHECK(!placeholder_object(rt.ctx, TextureKind::Orm, TextureShape::Array).is_null());
        KILN_CHECK(placeholder_object(rt.ctx, TextureKind::Count).is_null());
        KILN_CHECK(placeholder_object(nullptr, TextureKind::BaseColor).is_null());
        // Stale handle: failed placeholder in dev mode, BaseColor otherwise.
        GpuObject const stale = gpu_object(rt.ctx, TextureHandle{0, 77});
        KILN_CHECK_EQ(stale.native, dev ? u64(5) : u64(1));
        KILN_CHECK(gpu_object(rt.ctx, MeshHandle{0, 77}).is_null());
        KILN_CHECK_EQ(state(rt.ctx, MeshHandle{}), State::Unloaded);
        release(rt.ctx, t);
        KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
        rt.shutdown();
    }
}

KILN_TEST(Runtime, HostPlaceholderOverride) {
    Rt rt;
    NullAdapterDesc nd;
    nd.bindlessSlots       = 0;
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
    Rt rt;
    if (!rt.init()) return;
    MeshHandle m = request_mesh(rt.ctx, "mesh/cube_basic");
    KILN_REQUIRE(!m.is_null());
    KILN_CHECK_EQ(state(rt.ctx, m), State::Pending);
    KILN_CHECK(mesh_view(rt.ctx, m) == nullptr);
    KILN_CHECK(gpu_object(rt.ctx, m).is_null());
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

    GpuObject const obj = gpu_object(rt.ctx, m);
    KILN_CHECK(!obj.is_null());
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/cube_basic", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, obj);
        KILN_CHECK_EQ(got.size, decoded.size());
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }

    // Refcount: same handle; one release keeps it loaded, the second unloads.
    MeshHandle m2 = request_mesh(rt.ctx, "mesh/cube_basic");
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
    KILN_CHECK_EQ(null_adapter_stats(rt.na).destroys, destroysBefore + 1); // no frames reported: at once

    // A new request gets a new generation (stale old handle stays Unloaded).
    MeshHandle m3 = request_mesh(rt.ctx, "mesh/cube_basic");
    KILN_CHECK(m3 != m);
    KILN_CHECK_EQ(state(rt.ctx, m), State::Unloaded);
    KILN_CHECK(rt.pump_until([&] { return is_ready(rt.ctx, m3); }));
    release(rt.ctx, m3);
}

namespace {

/// The uploaded texture `t` holds the texels of `file`, rows padded to the adapter's pitch.
void check_uploaded(Rt& rt, TextureHandle t, Span<u8 const> file, u64 pitchAlign, char const* what) {
    Result<ktx2::Ktx2View> kv = ktx2::Ktx2View::open(file);
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
        Vec<u8> const texels   = test::corpus::texels(*kv, i);
        Span<u8 const> const L = texels.span();
        KILN_REQUIRE_EQ(L.size, usize(rowBytes * rows));
        KILN_REQUIRE(off + pitch * rows <= payload.size);
        bool same = true;
        for (u32 r = 0; r < rows; ++r)
            same =
                same && bytes_equal(payload.data + off + r * pitch, L.data + r * rowBytes, usize(rowBytes));
        KILN_CHECK_MSG(same, "%s: level %u rows differ (pitch align %llu)", what, i,
                       static_cast<unsigned long long>(pitchAlign));
    }
}

} // namespace

// Tight rows and a 256-byte row pitch; Zstd levels (the cooked golden, libktx's file) and plain ones,
// from the store and from memory.
KILN_TEST(Runtime, LoadTexture) {
    Vec<u8> golden(default_allocator(), Tag::Test), zstd(default_allocator(), Tag::Test),
        plain(default_allocator(), Tag::Test);
    if (!read_golden("ktx2/color_zstd", ".ktx2", golden)) return;
    char path[1024];
    format(path, sizeof path, "%s/generated/rgba8_srgb_mip_zstd.ktx2", test::corpus_dir());
    KILN_REQUIRE(test::corpus::read_file(path, zstd));
    format(path, sizeof path, "%s/generated/rgba8_unorm_mip.ktx2", test::corpus_dir());
    KILN_REQUIRE(test::corpus::read_file(path, plain));
    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        nd.offsetAlign   = 64;
        if (!rt.init(nd)) return;
        TextureHandle t = request_texture(rt.ctx, "ktx2/color_zstd");
        KILN_REQUIRE(!t.is_null());
        KILN_CHECK(texture_info(rt.ctx, t).isPlaceholder);
        TextureHandle tz = register_texture(rt.ctx, "gen/zstd", zstd.span());
        TextureHandle tp = register_texture(rt.ctx, "gen/plain", plain.span());
        KILN_REQUIRE(!tz.is_null() && !tp.is_null());
        KILN_REQUIRE(rt.pump_until(
            [&] { return is_ready(rt.ctx, t) && is_ready(rt.ctx, tz) && is_ready(rt.ctx, tp); }));
        int const meta  = rt.find_event(EventKind::MetaReady, t.bits());
        int const ready = rt.find_event(EventKind::Ready, t.bits());
        KILN_CHECK(meta >= 0 && ready > meta);

        Result<ktx2::Ktx2View> const gv = ktx2::Ktx2View::open(golden.span());
        KILN_CHECK(gv.ok() && gv->supercompressed());
        check_uploaded(rt, t, golden.span(), pitchAlign, "store golden");
        check_uploaded(rt, tz, zstd.span(), pitchAlign, "libktx zstd");
        check_uploaded(rt, tp, plain.span(), pitchAlign, "plain");
        release(rt.ctx, t);
        release(rt.ctx, tz);
        release(rt.ctx, tp);
    }
}

// A Zstd level that does not decode fails the asset at upload; the message carries the reason.
KILN_TEST(Runtime, LoadTextureBadZstdFrame) {
    Vec<u8> zstd(default_allocator(), Tag::Test);
    char path[1024];
    format(path, sizeof path, "%s/generated/rgba8_srgb_mip_zstd.ktx2", test::corpus_dir());
    KILN_REQUIRE(test::corpus::read_file(path, zstd));
    Result<ktx2::Ktx2View> kv = ktx2::Ktx2View::open(zstd.span());
    KILN_REQUIRE(kv.ok());
    zstd[usize(kv->levels()[0].byteOffset) + 5] ^= 0xFF; // inside level 0's frame header
    Rt rt;
    if (!rt.init()) return;
    TextureHandle t = register_texture(rt.ctx, "gen/bad", zstd.span());
    KILN_REQUIRE(!t.is_null());
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAssetLoadFailed));
    KILN_CHECK_MSG(std::strstr(rt.diags.last, "does not decode") != nullptr, "%s", rt.diags.last);
    release(rt.ctx, t);
}

KILN_TEST(Runtime, GoldenFolderGroupWait) {
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
        nd.bindlessSlots = 0;
        ContextDesc cd;
        cd.devPlaceholders = dev;
        if (!rt.init(nd, cd)) return;
        TextureHandle t = request_texture(rt.ctx, "tex/nope");
        KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
        KILN_CHECK(rt.diags.has(kDiagStoreMiss));
        int const f = rt.find_event(EventKind::Failed, t.bits());
        KILN_REQUIRE(f >= 0);
        KILN_CHECK_EQ(rt.events[usize(f)].status.code, Code::NotFound);
        KILN_CHECK(!has_meta(rt.ctx, t));
        GpuObject const g = gpu_object(rt.ctx, t);
        KILN_CHECK_EQ(g.native, dev ? u64(5) : u64(1)); // failed checker vs BaseColor placeholder
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(ti.isPlaceholder);
        KILN_CHECK_EQ(ti.desc.width, dev ? 8u : 1u);
        release(rt.ctx, t);
    }
}

KILN_TEST(Runtime, CorruptStoreFileFails) {
    test::HandStore store;
    if (!store.init("corrupt")) return;
    u8 garbage[300];
    for (usize i = 0; i < sizeof garbage; ++i)
        garbage[i] = u8(i * 37 + 11);
    KILN_REQUIRE(store.put("bad", AssetKind::Mesh, Span<u8 const>(garbage, sizeof garbage)));
    KILN_REQUIRE(
        store.put("short", AssetKind::Texture, Span<u8 const>(ktx2::kIdentifier, sizeof ktx2::kIdentifier)));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir();
    if (!rt.init({}, cd)) return;
    MeshHandle m    = request_mesh(rt.ctx, "bad");
    TextureHandle t = request_texture(rt.ctx, "short");
    KILN_REQUIRE(rt.pump_until(
        [&] { return state(rt.ctx, m) == State::Failed && state(rt.ctx, t) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAssetLoadFailed));
    KILN_CHECK(!rt.diags.has(kDiagStoreMiss));
    KILN_CHECK(gpu_object(rt.ctx, m).is_null());
    release(rt.ctx, m);
    release(rt.ctx, t);
}

KILN_TEST(Runtime, AdapterRejectFails) {
    Rt rt2;
    NullAdapterDesc nd2;
    nd2.failEveryN = 16; // the 15 placeholder uploads succeed, the 16th (the mesh) fails
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

    /// Cooks `virtual/cube`, has no source for `virtual/none`, and leaves every other name to the
    /// catalog.
    static Status prepare(void* user, AssetKind kind, StrView path, Allocator const* alloc, Vec<u8>* out,
                          Hash128*, DiagSink const* diag) {
        auto* self = static_cast<FakeProvider*>(user);
        self->calls.fetch_add(1);
        if (path == "virtual/none")
            return diagf(diag, make_status(Code::NotFound), 1001, Severity::Error, path, "provider",
                         "no source");
        if (kind != AssetKind::Mesh || path != "virtual/cube") return kOk;
        Vec<u8> v(alloc, Tag::Payload);
        v.append(self->bytes.span());
        *out = std::move(v);
        return kOk;
    }
};

KILN_TEST(Runtime, CookProviderOnMiss) {
    FakeProvider fp;
    if (!read_golden("mesh/cube_basic", ".mesh", fp.bytes)) return;
    Rt rt;
    if (!rt.init()) return;
    set_cook_provider(rt.ctx, CookProvider{&FakeProvider::prepare, &fp});
    MeshHandle m    = request_mesh(rt.ctx, "virtual/cube");
    MeshHandle miss = request_mesh(rt.ctx, "virtual/none");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m) && state(rt.ctx, miss) == State::Failed; }));
    KILN_CHECK_EQ(fp.calls.load(), 2u);
    KILN_CHECK(rt.diags.has(kDiagStoreMiss));
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/cube_basic", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, gpu_object(rt.ctx, m));
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }
    // A catalog hit asks the provider too; with no answer, the catalog entry is used.
    MeshHandle hit = request_mesh(rt.ctx, "mesh/Box");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, hit); }));
    KILN_CHECK_EQ(fp.calls.load(), 3u);
    release(rt.ctx, m);
    release(rt.ctx, miss);
    release(rt.ctx, hit);
}

KILN_TEST(Runtime, RegisterInMemory) {
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
    KILN_CHECK(find_texture(rt.ctx, asset_id("gen/tex")) == t);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m) && is_ready(rt.ctx, t); }));
    KILN_CHECK(rt.find_event(EventKind::MetaReady, m.bits()) >= 0);
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/multi_material", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, gpu_object(rt.ctx, m));
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

KILN_TEST(Runtime, BindlessSlots) {
    Rt rt;
    if (!rt.init()) return;
    RequestOptions ro;
    ro.textureKind          = TextureKind::Normal;
    TextureHandle t         = request_texture(rt.ctx, "ktx2/normal", ro);
    GpuObject const pending = gpu_object(rt.ctx, t);
    u32 const slot          = pending.slot;
    KILN_REQUIRE(slot != kInvalid);
    // The slot shows the Normal placeholder from the request on.
    KILN_CHECK_EQ(null_adapter_slot(rt.na, slot).native, u64(u32(TextureKind::Normal) + 1));
    KILN_CHECK_EQ(pending.native, u64(u32(TextureKind::Normal) + 1));
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    GpuObject const real = gpu_object(rt.ctx, t);
    KILN_CHECK_EQ(real.slot, slot);
    KILN_CHECK_EQ(null_adapter_slot(rt.na, slot).native, real.native);

    TextureHandle t2 = request_texture(rt.ctx, "ktx2/color_srgb");
    KILN_CHECK(gpu_object(rt.ctx, t2).slot != slot);
    release(rt.ctx, t2);
    // No frames reported: the number comes back at once and the next request reuses it.
    release(rt.ctx, t);
    TextureHandle t3 = request_texture(rt.ctx, "ktx2/normal");
    KILN_CHECK_EQ(gpu_object(rt.ctx, t3).slot, slot);
    release(rt.ctx, t3);
}

// With frames reported, a dropped object and its slot number wait for completedFrame.
KILN_TEST(Runtime, FramesDelayRelease) {
    Rt rt;
    if (!rt.init()) return;
    PumpOptions po;
    po.frame          = 1;
    po.completedFrame = 0;
    rt.pump_once(po);
    TextureHandle t = request_texture(rt.ctx, "ktx2/normal");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }, po));
    u32 const slot           = gpu_object(rt.ctx, t).slot;
    GpuObject const obj      = gpu_object(rt.ctx, t);
    u32 const destroysBefore = null_adapter_stats(rt.na).destroys;
    po.frame                 = 5;
    po.completedFrame        = 3;
    rt.pump_once(po);
    release(rt.ctx, t); // dropped after frame 5 was announced
    TextureHandle t2 = request_texture(rt.ctx, "ktx2/color_srgb");
    KILN_CHECK(gpu_object(rt.ctx, t2).slot != slot); // the number waits too
    po.frame          = 6;
    po.completedFrame = 4;
    rt.pump_once(po);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).destroys, destroysBefore);
    KILN_CHECK_EQ(null_adapter_payload(rt.na, obj).size != 0, true);
    po.frame          = 0; // 0 keeps the last frame
    po.completedFrame = 5;
    rt.pump_once(po);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).destroys, destroysBefore + 1);
    KILN_CHECK(null_adapter_payload(rt.na, obj).empty());
    TextureHandle t3 = request_texture(rt.ctx, "ktx2/normal");
    KILN_CHECK_EQ(gpu_object(rt.ctx, t3).slot, slot);
    release(rt.ctx, t2);
    release(rt.ctx, t3);
}

// Assets dropped at every stage of a load leave no object behind: kiln finishes the abandoned
// uploads and destroys their objects.
KILN_TEST(Runtime, DropMidLoadReleasesObjects) {
    Rt rt;
    if (!rt.init()) return;
    u32 const baseline = null_adapter_stats(rt.na).liveObjects; // the placeholders
    for (int round = 0; round < 8; ++round) {
        TextureHandle t = request_texture(rt.ctx, "ktx2/normal");
        MeshHandle m    = request_mesh(rt.ctx, kGoldenMeshes[0]);
        for (int i = 0; i < round; ++i)
            rt.pump_once();
        release(rt.ctx, t);
        release(rt.ctx, m);
    }
    KILN_REQUIRE(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
    for (int i = 0; i < 3; ++i)
        rt.pump_once();
    KILN_CHECK_EQ(null_adapter_stats(rt.na).liveObjects, baseline);
}

// A request beyond Adapter::bindlessSlots fails with K5004.
KILN_TEST(Runtime, BindlessSlotsExhausted) {
    Rt rt;
    NullAdapterDesc nd;
    nd.bindlessSlots = 1;
    if (!rt.init(nd)) return;
    TextureHandle a = request_texture(rt.ctx, "ktx2/normal");
    TextureHandle b = request_texture(rt.ctx, "ktx2/color_srgb");
    KILN_CHECK_EQ(gpu_object(rt.ctx, a).slot, 0u);
    KILN_CHECK_EQ(gpu_object(rt.ctx, b).slot, kInvalid);
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, b) == State::Failed && is_ready(rt.ctx, a); }));
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    release(rt.ctx, a);
    release(rt.ctx, b);
}

// No allocation in pump() or the queries.
KILN_TEST(Runtime, SteadyStateNoAllocation) {
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
            sink += u64(state(rt.ctx, m)) + gpu_object(rt.ctx, m).native + (mesh_view(rt.ctx, m) ? 1 : 0);
            sink += find_mesh(rt.ctx, id_of(rt.ctx, m)).bits();
        }
        sink += texture_info(rt.ctx, t).levelOffsets.size + gpu_object(rt.ctx, t).native;
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

// ---------------------------------------------------------------------------
// Hot reload (docs/design/hot-reload.md)
// ---------------------------------------------------------------------------

namespace {

/// A scratch catalog store `<samples>/reload_<name>`; the tests replace the files of `mesh/thing`
/// and `ktx2/thing` in it.
struct ReloadStore {
    test::HandStore hand;
    char const* dir = nullptr;

    bool init(char const* name) {
        char sub[64];
        format(sub, sizeof sub, "reload_%s", name);
        if (!hand.init(sub)) return false;
        dir = hand.dir();
        return true;
    }
};

/// Golden `rel` + `ext` becomes `mesh/thing` (.mesh) or `ktx2/thing` (.ktx2).
bool put_golden(ReloadStore& store, char const* rel, char const* ext) {
    Vec<u8> bytes(default_allocator(), Tag::Test);
    bool const mesh = std::strcmp(ext, ".mesh") == 0;
    return read_golden(rel, ext, bytes) &&
           store.hand.put(mesh ? "mesh/thing" : "ktx2/thing", mesh ? AssetKind::Mesh : AssetKind::Texture,
                          bytes.span());
}

bool put_garbage(ReloadStore& store) {
    u8 garbage[300];
    for (usize i = 0; i < sizeof garbage; ++i)
        garbage[i] = u8(i * 37 + 11);
    return store.hand.put("mesh/thing", AssetKind::Mesh, Span<u8 const>(garbage, sizeof garbage));
}

u32 count_code(DiagLog const& d, u32 code) {
    u32 n = 0;
    for (u32 i = 0; i < d.count; ++i)
        n += d.codes[i] == code ? 1u : 0u;
    return n;
}

u32 count_events(Rt const& rt, EventKind kind, u64 bits, usize from = 0) {
    u32 n = 0;
    for (usize i = from; i < rt.events.size(); ++i)
        n += (rt.events[i].kind == kind && rt.events[i].handle == bits) ? 1u : 0u;
    return n;
}

struct MeshCounts {
    u32 parts = 0, lods = 0, submeshes = 0;
    u64 decoded                              = 0;
    bool operator==(MeshCounts const&) const = default;
};

MeshCounts counts_of(mesh::MeshView const& v) {
    return {u32(v.parts().size()), u32(v.lods().size()), u32(v.submeshes().size()), v.decoded_size()};
}

bool golden_counts(char const* rel, MeshCounts& out) {
    Vec<u8> bytes(default_allocator(), Tag::Test);
    if (!read_golden(rel, ".mesh", bytes)) return false;
    Result<mesh::MeshView> v = mesh::MeshView::open(bytes.span());
    if (!KILN_CHECK(v.ok())) return false;
    out = counts_of(*v);
    return true;
}

} // namespace

KILN_TEST(Runtime, ReloadSwapsVersion) {
    ReloadStore store;
    if (!store.init("swap")) return;
    MeshCounts boxCounts, multiCounts;
    if (!golden_counts("mesh/Box", boxCounts) || !golden_counts("mesh/MultiUVTest", multiCounts)) return;
    KILN_REQUIRE(!(boxCounts == multiCounts));
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir;
    if (!rt.init({}, cd)) return;
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m); }));
    KILN_CHECK_EQ(version(rt.ctx, m), 1u);
    mesh::MeshView const* v1 = mesh_view(rt.ctx, m);
    KILN_REQUIRE(v1 != nullptr);
    KILN_CHECK(counts_of(*v1) == boxCounts);
    GpuObject const oldObj = gpu_object(rt.ctx, m);

    NullAdapterStats const st0 = null_adapter_stats(rt.na);
    usize const ev0            = rt.events.size();
    KILN_REQUIRE(put_golden(store, "mesh/MultiUVTest", ".mesh"));
    request_reload(rt.ctx, m);

    bool alwaysReady = true, oldViewUntilSwap = true;
    KILN_REQUIRE(rt.pump_until([&] {
        alwaysReady        = alwaysReady && is_ready(rt.ctx, m);
        bool const changed = rt.find_event(EventKind::Changed, m.bits(), ev0) >= 0;
        if (!changed) { // the old version is served until the swap
            mesh::MeshView const* v = mesh_view(rt.ctx, m);
            oldViewUntilSwap        = oldViewUntilSwap && v && counts_of(*v) == boxCounts &&
                               version(rt.ctx, m) == 1 && gpu_object(rt.ctx, m).native == oldObj.native;
        }
        return changed;
    }));
    KILN_CHECK(alwaysReady);
    KILN_CHECK(oldViewUntilSwap);
    KILN_CHECK_EQ(version(rt.ctx, m), 2u);
    int const ch = rt.find_event(EventKind::Changed, m.bits(), ev0);
    if (ch >= 0) KILN_CHECK_EQ(rt.events[usize(ch)].version, 2u);
    KILN_CHECK_EQ(count_events(rt, EventKind::MetaReady, m.bits(), ev0), 0u);
    KILN_CHECK_EQ(count_events(rt, EventKind::Ready, m.bits(), ev0), 0u);
    mesh::MeshView const* v2 = mesh_view(rt.ctx, m);
    KILN_REQUIRE(v2 != nullptr);
    KILN_CHECK(counts_of(*v2) == multiCounts);

    GpuObject const newObj = gpu_object(rt.ctx, m);
    KILN_CHECK(newObj.native != oldObj.native);
    Vec<u8> file(default_allocator(), Tag::Test), decoded(default_allocator(), Tag::Test);
    if (decoded_golden_mesh("mesh/MultiUVTest", file, decoded)) {
        Span<u8 const> got = null_adapter_payload(rt.na, newObj);
        KILN_CHECK(got.size == decoded.size() && bytes_equal(got.data, decoded.data(), got.size));
    }
    NullAdapterStats const st1 = null_adapter_stats(rt.na);
    KILN_CHECK_EQ(st1.destroys, st0.destroys + 1); // the old object
    KILN_CHECK(null_adapter_payload(rt.na, oldObj).size == 0);
    KILN_CHECK_EQ(rt.diags.count, 0u);
    release(rt.ctx, m);
}

// An upload the adapter fails after commit_upload (UploadStatus::Failed) fails the asset with K5004;
// its object is destroyed, and a later reload that succeeds recovers it with Ready.
KILN_TEST(Runtime, UploadFailedFailsAsset) {
    Rt rt;
    if (!rt.init()) return;
    u32 const baseline = null_adapter_stats(rt.na).liveObjects; // the placeholders
    Group g            = group(rt.ctx);
    RequestOptions ro;
    ro.group = g;
    null_adapter_fail_uploads(rt.na, true);
    TextureHandle t = request_texture(rt.ctx, "ktx2/normal", ro);
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
    null_adapter_fail_uploads(rt.na, false);
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    int const failed = rt.find_event(EventKind::Failed, t.bits());
    KILN_REQUIRE(failed >= 0);
    KILN_CHECK_EQ(rt.events[usize(failed)].status.code, Code::OutOfMemory); // the adapter's reason
    KILN_CHECK_EQ(progress(rt.ctx, g).failed, 1u);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).uploadsFailed, 1u);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).liveObjects, baseline); // the failed object is gone
    KILN_CHECK(gpu_object(rt.ctx, t).native <= 15u);                // a placeholder, not the failed object

    usize const ev0 = rt.events.size();
    request_reload(rt.ctx, t);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    int const ready = rt.find_event(EventKind::Ready, t.bits(), ev0);
    KILN_REQUIRE(ready >= 0);
    KILN_CHECK_EQ(rt.events[usize(ready)].version, 2u);
    KILN_CHECK_EQ(progress(rt.ctx, g).ready, 1u);
    release(rt.ctx, t);
    release(rt.ctx, g);
}

// kiln's write fails after a successful begin_upload (here: no destination memory). With
// discard_upload the adapter frees the ticket and object at once, nothing is committed, and kiln
// neither polls nor destroys; without it kiln commits and destroys the result. Either way nothing
// is left, and the asset recovers on reload.
KILN_TEST(Runtime, CpuFailureDiscardsUpload) {
    for (bool withDiscard : {true, false}) {
        Rt rt;
        Result<NullAdapter*> na = null_adapter_create({}, &rt.adapter);
        KILN_REQUIRE(na.ok());
        rt.na = *na;
        if (!withDiscard) rt.adapter.discard_upload = nullptr;
        Result<Context*> c = create(ContextDesc{
            .diag = rt.diags.sink(), .adapter = &rt.adapter, .storeDir = test::golden_store_dir()});
        KILN_REQUIRE(c.ok());
        rt.ctx                    = *c;
        NullAdapterStats const s0 = null_adapter_stats(rt.na);
        null_adapter_break_targets(rt.na, true);
        TextureHandle t = request_texture(rt.ctx, "ktx2/normal");
        KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
        null_adapter_break_targets(rt.na, false);
        for (int i = 0; i < 3; ++i) // an orphaned commit is destroyed on a later pump
            rt.pump_once();
        NullAdapterStats const s1 = null_adapter_stats(rt.na);
        KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
        KILN_CHECK_EQ(s1.discards, s0.discards + (withDiscard ? 1u : 0u));
        KILN_CHECK_EQ(s1.commits, s0.commits + (withDiscard ? 0u : 1u));
        KILN_CHECK_EQ(s1.liveObjects, s0.liveObjects); // nothing left behind
        request_reload(rt.ctx, t);
        KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
        release(rt.ctx, t);
    }
}

// A reload whose upload the adapter fails keeps the current version (K5010), like any failed reload.
KILN_TEST(Runtime, UploadFailedReloadKeepsOld) {
    Rt rt;
    if (!rt.init()) return;
    TextureHandle t = request_texture(rt.ctx, "ktx2/normal");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    GpuObject const obj = gpu_object(rt.ctx, t);
    u32 const live      = null_adapter_stats(rt.na).liveObjects;
    usize const ev0     = rt.events.size();
    null_adapter_fail_uploads(rt.na, true);
    request_reload(rt.ctx, t);
    KILN_REQUIRE(rt.pump_until([&] { return rt.diags.has(kDiagReloadFailed); }));
    null_adapter_fail_uploads(rt.na, false);
    KILN_CHECK_EQ(state(rt.ctx, t), State::Ready);
    KILN_CHECK_EQ(version(rt.ctx, t), 1u);
    KILN_CHECK_EQ(gpu_object(rt.ctx, t).native, obj.native);
    KILN_CHECK_EQ(rt.events.size(), ev0);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).liveObjects, live);
    release(rt.ctx, t);
}

// A placeholder upload the adapter fails makes create() fail with K5009.
KILN_TEST(Runtime, UploadFailedPlaceholderFailsCreate) {
    Rt rt;
    Result<NullAdapter*> a = null_adapter_create({}, &rt.adapter);
    KILN_REQUIRE(a.ok());
    rt.na = *a;
    null_adapter_fail_uploads(rt.na, true);
    Result<Context*> c = create(ContextDesc{.diag = rt.diags.sink(), .adapter = &rt.adapter});
    KILN_CHECK(c.failed());
    if (c.ok()) destroy(*c);
    KILN_CHECK(rt.diags.has(kDiagPlaceholderFailed));
    KILN_CHECK_EQ(null_adapter_stats(rt.na).liveObjects, 0u); // every placeholder object destroyed
}

KILN_TEST(Runtime, ReloadFailureKeepsOld) {
    ReloadStore store;
    if (!store.init("fail")) return;
    MeshCounts boxCounts;
    if (!golden_counts("mesh/Box", boxCounts)) return;
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir;
    if (!rt.init({}, cd)) return;
    Group g = group(rt.ctx);
    RequestOptions ro;
    ro.group     = g;
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing", ro);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m); }));
    mesh::MeshView const* v1   = mesh_view(rt.ctx, m);
    GpuObject const obj        = gpu_object(rt.ctx, m);
    GroupStatus const gs0      = progress(rt.ctx, g);
    NullAdapterStats const st0 = null_adapter_stats(rt.na);
    usize const ev0            = rt.events.size();

    KILN_REQUIRE(put_garbage(store));
    request_reload(rt.ctx, m);
    bool alwaysReady = true;
    KILN_REQUIRE(rt.pump_until([&] {
        alwaysReady = alwaysReady && is_ready(rt.ctx, m);
        return rt.diags.has(kDiagReloadFailed);
    }));
    for (int i = 0; i < 5; ++i)
        rt.pump_once();
    KILN_CHECK(alwaysReady);
    KILN_CHECK_EQ(state(rt.ctx, m), State::Ready);
    KILN_CHECK_EQ(version(rt.ctx, m), 1u);
    KILN_CHECK(mesh_view(rt.ctx, m) == v1);
    KILN_CHECK(counts_of(*v1) == boxCounts); // still readable: the old view stays valid
    KILN_CHECK_EQ(gpu_object(rt.ctx, m).native, obj.native);
    KILN_CHECK_EQ(count_code(rt.diags, kDiagReloadFailed), 1u);
    KILN_CHECK_EQ(rt.diags.count, 1u);
    KILN_CHECK_EQ(rt.events.size(), ev0);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).binds, st0.binds);
    GroupStatus const gs1 = progress(rt.ctx, g);
    KILN_CHECK_EQ(gs1.ready, gs0.ready);
    KILN_CHECK_EQ(gs1.failed, gs0.failed);
    KILN_CHECK_EQ(gs1.bytesDone, gs0.bytesDone);
    release(rt.ctx, m);
    release(rt.ctx, g);
}

KILN_TEST(Runtime, ReloadFromFailed) {
    ReloadStore store;
    if (!store.init("from_failed")) return;
    MeshCounts multiCounts;
    if (!golden_counts("mesh/MultiUVTest", multiCounts)) return;
    KILN_REQUIRE(put_garbage(store));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir;
    if (!rt.init({}, cd)) return;
    Group g = group(rt.ctx);
    RequestOptions ro;
    ro.group     = g;
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing", ro);
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, m) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAssetLoadFailed));
    KILN_CHECK_EQ(version(rt.ctx, m), 1u);
    GroupStatus const gs0 = progress(rt.ctx, g);
    KILN_CHECK_EQ(gs0.failed, 1u);
    KILN_CHECK_EQ(gs0.ready, 0u);
    KILN_CHECK_EQ(gs0.bytesTotal, 0u);

    // Fails again: stays Failed, a Failed event with the new status, no K5010.
    usize ev0 = rt.events.size();
    request_reload(rt.ctx, m);
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Failed, m.bits(), ev0) >= 0; }));
    KILN_CHECK_EQ(state(rt.ctx, m), State::Failed);
    KILN_CHECK(!rt.diags.has(kDiagReloadFailed));
    KILN_CHECK_EQ(progress(rt.ctx, g).failed, 1u); // failing again is not counted twice
    KILN_CHECK_EQ(progress(rt.ctx, g).ready, 0u);

    // Succeeds: Ready (not Changed), content version 2.
    KILN_REQUIRE(put_golden(store, "mesh/MultiUVTest", ".mesh"));
    ev0 = rt.events.size();
    request_reload(rt.ctx, m);
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Ready, m.bits(), ev0) >= 0; }));
    KILN_CHECK(is_ready(rt.ctx, m));
    KILN_CHECK_EQ(version(rt.ctx, m), 2u);
    KILN_CHECK_EQ(rt.events[usize(rt.find_event(EventKind::Ready, m.bits(), ev0))].version, 2u);
    KILN_CHECK_EQ(count_events(rt, EventKind::Changed, m.bits(), ev0), 0u);
    KILN_CHECK_EQ(count_events(rt, EventKind::MetaReady, m.bits(), ev0), 0u);
    mesh::MeshView const* v = mesh_view(rt.ctx, m);
    KILN_REQUIRE(v != nullptr);
    KILN_CHECK(counts_of(*v) == multiCounts);
    // Failed -> Ready is a first success: the group moves the member from failed to
    // ready and counts its bytes.
    GroupStatus const gs1 = progress(rt.ctx, g);
    KILN_CHECK_EQ(gs1.failed, 0u);
    KILN_CHECK_EQ(gs1.ready, 1u);
    KILN_CHECK_EQ(gs1.pending, 0u);
    KILN_CHECK(gs1.bytesDone > 0);
    KILN_CHECK_EQ(gs1.bytesDone, gs1.bytesTotal);
    KILN_CHECK_EQ(gs1.bytesDone, v->decoded_size());
    release(rt.ctx, m);
    GroupStatus const gs2 = progress(rt.ctx, g); // leaving undoes what was counted
    KILN_CHECK_EQ(gs2.ready, 0u);
    KILN_CHECK_EQ(gs2.failed, 0u);
    KILN_CHECK_EQ(gs2.bytesDone, 0u);
    KILN_CHECK_EQ(gs2.bytesTotal, 0u);
    release(rt.ctx, g);
}

KILN_TEST(Runtime, ReloadTexture) {
    ReloadStore store;
    if (!store.init("texture")) return;
    KILN_REQUIRE(put_golden(store, "ktx2/color_srgb", ".ktx2"));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir;
    if (!rt.init({}, cd)) return;
    TextureHandle t       = request_texture(rt.ctx, "ktx2/thing");
    GpuObject const first = gpu_object(rt.ctx, t);
    KILN_REQUIRE(first.slot != kInvalid);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    TextureInfo const ti1 = texture_info(rt.ctx, t);
    GpuObject const obj1  = gpu_object(rt.ctx, t);
    KILN_CHECK_EQ(null_adapter_slot(rt.na, first.slot).native, obj1.native);

    usize const ev0 = rt.events.size();
    KILN_REQUIRE(put_golden(store, "ktx2/height16", ".ktx2"));
    request_reload(rt.ctx, t);
    bool oldInfo = true;
    KILN_REQUIRE(rt.pump_until([&] {
        bool const changed = rt.find_event(EventKind::Changed, t.bits(), ev0) >= 0;
        if (!changed) {
            TextureInfo const ti = texture_info(rt.ctx, t);
            oldInfo = oldInfo && !ti.isPlaceholder && ti.version == 1 && ti.desc.format == ti1.desc.format &&
                      ti.desc.width == ti1.desc.width;
        }
        return changed;
    }));
    KILN_CHECK(oldInfo);
    TextureInfo const ti2 = texture_info(rt.ctx, t);
    KILN_CHECK(!ti2.isPlaceholder);
    KILN_CHECK_EQ(ti2.version, 2u);
    KILN_CHECK(ti2.desc.format != ti1.desc.format || ti2.desc.width != ti1.desc.width ||
               ti2.desc.height != ti1.desc.height);
    KILN_CHECK_EQ(ti2.levelOffsets.size, usize(ti2.desc.levels));
    GpuObject const obj2 = gpu_object(rt.ctx, t);
    KILN_CHECK(obj2.native != obj1.native);
    KILN_CHECK_EQ(ti2.gpu.native, obj2.native);
    KILN_CHECK_EQ(null_adapter_slot(rt.na, first.slot).native, obj2.native);
    KILN_CHECK(null_adapter_payload(rt.na, obj1).empty());
    release(rt.ctx, t);
}

KILN_TEST(Runtime, ReloadWhileLoading) {
    ReloadStore store;
    if (!store.init("while_loading")) return;
    MeshCounts boxCounts, multiCounts;
    if (!golden_counts("mesh/Box", boxCounts) || !golden_counts("mesh/MultiUVTest", multiCounts)) return;
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));

    Rt rt;
    ContextDesc cd;
    cd.storeDir = store.dir;
    if (!rt.init({}, cd)) return;

    // Queued: the reload waits for the first load to settle, then runs once.
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing");
    request_reload(rt.ctx, m);
    KILN_REQUIRE(put_golden(store, "mesh/MultiUVTest", ".mesh"));
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, m.bits()) >= 0; }));
    int const meta    = rt.find_event(EventKind::MetaReady, m.bits());
    int const ready   = rt.find_event(EventKind::Ready, m.bits());
    int const changed = rt.find_event(EventKind::Changed, m.bits());
    KILN_CHECK(meta >= 0 && ready > meta && changed > ready);
    if (ready >= 0) KILN_CHECK_EQ(rt.events[usize(ready)].version, 1u);
    if (changed >= 0) KILN_CHECK_EQ(rt.events[usize(changed)].version, 2u);
    KILN_CHECK_EQ(count_events(rt, EventKind::MetaReady, m.bits()), 1u);
    KILN_CHECK_EQ(version(rt.ctx, m), 2u);
    KILN_CHECK(counts_of(*mesh_view(rt.ctx, m)) == multiCounts);

    // Job in flight: a reload requested during a reload runs after it.
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));
    usize const ev0 = rt.events.size();
    request_reload(rt.ctx, m);
    rt.pump_once();                               // dispatches the meta stage
    KILN_CHECK(stats(rt.ctx).ioJobsInFlight > 0); // popped only by the next pump
    request_reload(rt.ctx, m);
    KILN_REQUIRE(rt.pump_until([&] { return count_events(rt, EventKind::Changed, m.bits(), ev0) == 2; }));
    for (int i = 0; i < 5; ++i)
        rt.pump_once();
    KILN_CHECK_EQ(count_events(rt, EventKind::Changed, m.bits(), ev0), 2u);
    KILN_CHECK_EQ(version(rt.ctx, m), 4u);
    KILN_CHECK(counts_of(*mesh_view(rt.ctx, m)) == boxCounts);
    KILN_CHECK_EQ(rt.diags.count, 0u);
    release(rt.ctx, m);
}

KILN_TEST(Runtime, ReloadMemorySourceWarns) {
    Vec<u8> meshBytes(default_allocator(), Tag::Test);
    if (!read_golden("mesh/Box", ".mesh", meshBytes)) return;
    Rt rt;
    if (!rt.init()) return;
    MeshHandle m = register_mesh(rt.ctx, "gen/box", meshBytes.span());
    KILN_REQUIRE(!m.is_null());
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m); }));
    usize const ev0            = rt.events.size();
    NullAdapterStats const st0 = null_adapter_stats(rt.na);
    request_reload(rt.ctx, m);
    for (int i = 0; i < 5; ++i)
        rt.pump_once();
    KILN_CHECK_EQ(rt.diags.count, 1u);
    KILN_CHECK_EQ(count_code(rt.diags, kDiagReloadMemorySource), 1u);
    KILN_CHECK_EQ(rt.events.size(), ev0);
    KILN_CHECK_EQ(version(rt.ctx, m), 1u);
    KILN_CHECK_EQ(null_adapter_stats(rt.na).beginUploads, st0.beginUploads);
    release(rt.ctx, m);
}

KILN_TEST(Runtime, HotReloadUnavailableWarns) {
    IoBackend noStat = *compat_io_backend();
    noStat.stat      = nullptr;
    Rt rt;
    ContextDesc cd;
    cd.storeDir  = "nowhere";
    cd.io        = &noStat;
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    KILN_CHECK_EQ(count_code(rt.diags, kDiagHotReloadUnavailable), 1u);
    rt.shutdown();

#if !(defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD)
    Rt rt2;
    ContextDesc cd2;
    cd2.storeDir  = "nowhere";
    cd2.hotReload = {.watchStore = true};
    if (!rt2.init({}, cd2)) return;
    KILN_CHECK_EQ(count_code(rt2.diags, kDiagHotReloadUnavailable), 1u);
#endif
}

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
KILN_TEST(Runtime, StorePollerDetectsChange) {
    ReloadStore store;
    if (!store.init("poller")) return;
    MeshCounts multiCounts;
    if (!golden_counts("mesh/MultiUVTest", multiCounts)) return;
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));

    Rt rt;
    ContextDesc cd;
    cd.storeDir  = store.dir;
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    KILN_CHECK(!rt.diags.has(kDiagHotReloadUnavailable));
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m); }));
    usize const ev0 = rt.events.size();

    // Unchanged file: nothing happens for a few poll rounds.
    for (int i = 0; i < 20; ++i) {
        rt.pump_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    KILN_CHECK_EQ(rt.events.size(), ev0);

    // A different size changes the stat even where mtime resolution is coarse.
    KILN_REQUIRE(put_golden(store, "mesh/MultiUVTest", ".mesh"));
    bool changed = false;
    for (int i = 0; i < 1000 && !changed; ++i) { // ~5 s
        rt.pump_once();
        changed = rt.find_event(EventKind::Changed, m.bits(), ev0) >= 0;
        if (!changed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    KILN_REQUIRE(changed);
    KILN_CHECK_EQ(version(rt.ctx, m), 2u);
    KILN_CHECK(counts_of(*mesh_view(rt.ctx, m)) == multiCounts);
    KILN_CHECK_EQ(rt.diags.count, 0u);
    release(rt.ctx, m);
}
#endif

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

// ---------------------------------------------------------------------------
// Texture shapes (docs/design/texture-shapes.md)
// ---------------------------------------------------------------------------

namespace {

/// A scratch catalog store with KTX2 corpus files under short names: sky (cube), layers (array
/// of 7), vol (volume) and flat (2D).
struct ShapeStore {
    test::HandStore hand;
    char const* dir = nullptr;

    bool init() {
        if (!hand.init("shapes")) return false;
        dir = hand.dir();
        struct Copy {
            char const* from;
            char const* to;
        };
        Copy const files[] = {
            {"generated/cube_rgba8_srgb_mip.ktx2",     "sky"   },
            {"khronos/r8g8b8a8_srgb_array_7_mip.ktx2", "layers"},
            {"khronos/r8g8b8a8_srgb_3d_7.ktx2",        "vol"   },
            {"khronos/r8g8b8a8_srgb.ktx2",             "flat"  },
        };
        for (Copy const& c : files) {
            char src[1024];
            format(src, sizeof src, "%s/%s", test::corpus_dir(), c.from);
            if (!hand.put_file(StrView(c.to), AssetKind::Texture, src)) return false;
        }
        return true;
    }
};

TextureHandle request_shape(Rt& rt, char const* name, TextureShape shape) {
    RequestOptions ro;
    ro.textureShape = shape;
    return request_texture(rt.ctx, name, ro);
}

} // namespace

KILN_TEST(Runtime, ShapePlaceholders) {
    Rt rt;
    ContextDesc cd;
    cd.storeDir = "does/not/exist";
    if (!rt.init({}, cd)) return;
    TextureHandle const cube = request_shape(rt, "tex/cube", TextureShape::Cube);
    TextureInfo ti           = texture_info(rt.ctx, cube);
    KILN_CHECK(ti.isPlaceholder);
    KILN_CHECK(ti.desc.isCube && ti.desc.faces == 6);
    TextureHandle const arr = request_shape(rt, "tex/array", TextureShape::Array);
    ti                      = texture_info(rt.ctx, arr);
    KILN_CHECK(ti.desc.isArray && !ti.desc.isCube);
    release(rt.ctx, cube);
    release(rt.ctx, arr);
}

KILN_TEST(Runtime, CubeAndArrayLoad) {
    ShapeStore store;
    if (!store.init()) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.dir);
    if (!rt.init({}, cd)) return;
    TextureHandle const sky    = request_shape(rt, "sky", TextureShape::Cube);
    TextureHandle const layers = request_shape(rt, "layers", TextureShape::Array);
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, sky) && is_ready(rt.ctx, layers); }));
    TextureInfo const cube = texture_info(rt.ctx, sky);
    KILN_CHECK(!cube.isPlaceholder && cube.desc.isCube && cube.desc.faces == 6);
    TextureInfo const arr = texture_info(rt.ctx, layers);
    KILN_CHECK(!arr.isPlaceholder && arr.desc.isArray && arr.desc.layers == 7);
    release(rt.ctx, sky);
    release(rt.ctx, layers);
}

// The cooked shape must be the requested one. A volume never loads: the KTX2 reader rejects 3D.
KILN_TEST(Runtime, TextureShapeMismatchFails) {
    ShapeStore store;
    if (!store.init()) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.dir);
    if (!rt.init({}, cd)) return;
    TextureHandle const cubeAs2D   = request_shape(rt, "sky", TextureShape::Tex2D);
    TextureHandle const flatAsCube = request_shape(rt, "flat", TextureShape::Cube);
    TextureHandle const vol        = request_shape(rt, "vol", TextureShape::Tex2D);
    KILN_REQUIRE(rt.pump_until([&] {
        return state(rt.ctx, cubeAs2D) == State::Failed && state(rt.ctx, flatAsCube) == State::Failed &&
               state(rt.ctx, vol) == State::Failed;
    }));
    KILN_CHECK_EQ(count_code(rt.diags, kDiagTextureShapeMismatch), 2u);
    KILN_CHECK_EQ(count_code(rt.diags, kDiagAssetLoadFailed), 1u);
    release(rt.ctx, cubeAs2D);
    release(rt.ctx, flatAsCube);
    release(rt.ctx, vol);
}

// An adapter without kCubeTextures / kArrayTextures gets 2D placeholders only, and a request for
// another shape fails as adapter-rejected.
KILN_TEST(Runtime, AdapterWithoutShapeCaps) {
    Rt rt;
    Result<NullAdapter*> a = null_adapter_create({.bindlessSlots = 0}, &rt.adapter);
    KILN_REQUIRE(a.ok());
    rt.na = *a;
    rt.adapter.caps &= ~u32(kCubeTextures | kArrayTextures);
    Result<Context*> c =
        create(ContextDesc{.diag = rt.diags.sink(), .adapter = &rt.adapter, .storeDir = "none"});
    KILN_REQUIRE(c.ok());
    rt.ctx = *c;
    KILN_CHECK_EQ(null_adapter_stats(rt.na).beginUploads, KILN_DEBUG ? 5u : 4u);
    KILN_CHECK(placeholder_object(rt.ctx, TextureKind::BaseColor, TextureShape::Cube).is_null());
    KILN_CHECK(!placeholder_object(rt.ctx, TextureKind::BaseColor).is_null());
    TextureHandle const cube = request_shape(rt, "tex/cube", TextureShape::Cube);
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, cube) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    release(rt.ctx, cube);
}

// A texture-only adapter (no kMeshes) is never asked to upload a mesh, and a
// requested or registered mesh fails as adapter-rejected (docs/design/texture-only.md).
KILN_TEST(Runtime, AdapterWithoutMeshes) {
    Rt rt;
    Result<NullAdapter*> a = null_adapter_create({.bindlessSlots = 0}, &rt.adapter);
    KILN_REQUIRE(a.ok());
    rt.na = *a;
    rt.adapter.caps &= ~u32(kMeshes);
    Result<Context*> c = create(
        ContextDesc{.diag = rt.diags.sink(), .adapter = &rt.adapter, .storeDir = test::golden_store_dir()});
    KILN_REQUIRE(c.ok());
    rt.ctx                 = *c;
    u32 const placeholders = null_adapter_stats(rt.na).beginUploads;

    MeshHandle const mesh = request_mesh(rt.ctx, "mesh/Box");
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, mesh) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    KILN_CHECK_EQ(null_adapter_stats(rt.na).beginUploads, placeholders);
    release(rt.ctx, mesh);
}

// texture_level_layout: levels at the offset alignment, rows padded, layers inside a level; and it
// matches the layout kiln used for a loaded texture.
KILN_TEST(Runtime, TextureLevelLayout) {
    TextureDesc const cube{.format     = Format::R8G8B8A8_UNORM,
                           .width      = 8,
                           .height     = 8,
                           .depth      = 1,
                           .layers     = 6,
                           .levels     = 3,
                           .shape      = TextureShape::Cube,
                           .firstLevel = 0};
    CopyConstraints const cc{
        .optimalRowPitchAlign = 256, .optimalOffsetAlign = 512, .bufferOffsetAlign = 256};
    u64 offsets[3], pitches[3];
    KILN_CHECK_EQ(texture_level_layout(cube, cc, offsets, pitches),
                  u64(256 * 8 * 6 + 256 * 4 * 6 + 256 * 2 * 6));
    KILN_CHECK_EQ(offsets[0], u64(0));
    KILN_CHECK_EQ(offsets[1], u64(256 * 8 * 6));
    KILN_CHECK_EQ(offsets[2], u64(256 * 8 * 6 + 256 * 4 * 6));
    KILN_CHECK_EQ(pitches[2], u64(256));
    KILN_CHECK_EQ(texture_level_layout(cube, cc, nullptr, nullptr), u64(21504));

    // Invalid input: unknown format, alignments that are 0 or not powers of two, levels past 31,
    // sizes past u64.
    TextureDesc bad = cube;
    bad.format      = Format(12345);
    KILN_CHECK_EQ(texture_level_layout(bad, cc, nullptr, nullptr), u64(0));
    TextureDesc const small{.format     = Format::R8G8B8A8_UNORM,
                            .width      = 3,
                            .height     = 1,
                            .depth      = 1,
                            .layers     = 1,
                            .levels     = 40,
                            .shape      = TextureShape::Tex2D,
                            .firstLevel = 0};
    u64 manyOffsets[40], manyPitches[40];
    CopyConstraints const odd{.optimalRowPitchAlign = 5, .optimalOffsetAlign = 0, .bufferOffsetAlign = 1};
    KILN_CHECK_EQ(texture_level_layout(small, odd, manyOffsets, manyPitches), u64(16 + 39 * 8));
    KILN_CHECK_EQ(manyPitches[0], u64(16)); // 12 bytes padded to 8-byte rows
    KILN_CHECK_EQ(manyPitches[39], u64(8));
    KILN_CHECK_EQ(manyOffsets[39], u64(16 + 38 * 8));
    TextureDesc const huge{.format     = Format::R32G32B32A32_SFLOAT,
                           .width      = ~0u,
                           .height     = ~0u,
                           .depth      = 1,
                           .layers     = ~0u,
                           .levels     = 1,
                           .shape      = TextureShape::Array,
                           .firstLevel = 0};
    KILN_CHECK_EQ(texture_level_layout(huge, cc, nullptr, nullptr), u64(0));

    Rt rt;
    if (!rt.init(NullAdapterDesc{.rowPitchAlign = 64, .offsetAlign = 128})) return;
    TextureHandle const t = request_texture(rt.ctx, "ktx2/normal");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    TextureInfo const ti = texture_info(rt.ctx, t);
    TextureDesc const d{.format     = ti.desc.format,
                        .width      = ti.desc.width,
                        .height     = ti.desc.height,
                        .depth      = ti.desc.depth,
                        .layers     = ti.desc.layers * ti.desc.faces,
                        .levels     = ti.desc.levels,
                        .shape      = TextureShape::Tex2D,
                        .firstLevel = 0};
    CopyConstraints c{};
    rt.adapter.copy_constraints(rt.adapter.user, &c);
    u64 mine[16], myPitches[16];
    KILN_REQUIRE(d.levels <= 16 && ti.levelOffsets.size == d.levels);
    (void)texture_level_layout(d, c, mine, myPitches);
    for (u32 i = 0; i < d.levels; ++i) {
        KILN_CHECK_EQ(mine[i], ti.levelOffsets[i]);
        KILN_CHECK_EQ(myPitches[i], ti.levelRowPitches[i]);
    }
    release(rt.ctx, t);
}

namespace {
u32 g_flushes = 0;
void count_flush(void*) { ++g_flushes; }
} // namespace

// An adapter with flush but without kSelfSubmitting: create() waits for the placeholders through
// flush, pump() calls it every time, and wait() works.
KILN_TEST(Runtime, AdapterFlush) {
    Rt rt;
    Result<NullAdapter*> a = null_adapter_create({}, &rt.adapter);
    KILN_REQUIRE(a.ok());
    rt.na = *a;
    rt.adapter.caps &= ~u32(kSelfSubmitting);
    rt.adapter.flush   = &count_flush;
    g_flushes          = 0;
    Result<Context*> c = create(
        ContextDesc{.diag = rt.diags.sink(), .adapter = &rt.adapter, .storeDir = test::golden_store_dir()});
    KILN_REQUIRE(c.ok());
    rt.ctx = *c;
    KILN_CHECK(g_flushes >= 1);
    u32 const afterCreate = g_flushes;
    (void)pump(rt.ctx);
    KILN_CHECK_EQ(g_flushes, afterCreate + 1);

    Group const g        = group(rt.ctx);
    MeshHandle const m   = request_mesh(rt.ctx, "mesh/Box", RequestOptions{.group = g});
    GroupStatus const st = wait(rt.ctx, g, WaitOptions{.timeoutMs = 10000});
    KILN_CHECK(st.settled());
    KILN_CHECK_EQ(st.ready, 1u);
    KILN_CHECK(g_flushes > afterCreate + 1);
    release(rt.ctx, m);
    release(rt.ctx, g);
}

namespace {
bool no_bc5(void*, Format f, FormatUsage) { return f != Format::BC5_UNORM; }
} // namespace

// create() checks the catalog's profile against the adapter once (docs/design/target-profiles.md).
KILN_TEST(Runtime, CreateChecksStoreProfile) {
    test::HandStore store;
    if (!store.init("runtime_store_profile")) return;
    store.set_block_formats(block_format_bit(Format::BC5_UNORM) | block_format_bit(Format::BC7_SRGB));
    Vec<u8> tex(default_allocator(), Tag::Test);
    if (!read_golden("ktx2/normal", ".ktx2", tex)) return;
    KILN_REQUIRE(store.put("normal", AssetKind::Texture, tex.span()));

    Adapter adapter{};
    Result<NullAdapter*> na = null_adapter_create({}, &adapter);
    KILN_REQUIRE(na.ok());
    DiagLog log;
    ContextDesc cd{};
    cd.adapter          = &adapter;
    cd.storeDir         = StrView(store.dir());
    cd.diag             = log.sink();
    Result<Context*> ok = create(cd); // the null adapter samples everything
    KILN_REQUIRE(ok.ok());
    destroy(*ok);

    adapter.supports_format  = &no_bc5;
    Result<Context*> refused = create(cd);
    KILN_CHECK_EQ(refused.code(), Code::Unsupported);
    KILN_CHECK(log.has(kDiagStoreProfileUnsampled));
    KILN_CHECK_MSG(std::strstr(log.last, "BC5_UNORM") != nullptr, "%s", log.last);

    cd.allowUnsampledFormats = true; // tools and debugging
    Result<Context*> allowed = create(cd);
    KILN_CHECK(allowed.ok());
    if (allowed.ok()) destroy(*allowed);
    null_adapter_destroy(*na);
}

// The goldens as a store (hand_store.h), also the ctest fixture for kiln-headless.
KILN_TEST(GoldenStore, Build) {
    char path[1200];
    (void)manifest_file_path(StrView(test::golden_store_dir()), path, sizeof path);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(test::corpus::read_file(path, bytes));
    Result<ManifestView> v = ManifestView::open(bytes.span());
    KILN_REQUIRE(v.ok());
    ManifestProfile p;
    KILN_REQUIRE(v->find_profile("compat", &p));
    ManifestEntry e;
    for (char const* name : kGoldenMeshes)
        KILN_CHECK_MSG(p.find(AssetKind::Mesh, StrView(name), &e), "%s is not in the golden store", name);
    for (char const* name : kGoldenTextures)
        KILN_CHECK_MSG(p.find(AssetKind::Texture, StrView(name), &e), "%s is not in the golden store", name);
}
