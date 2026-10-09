// tests/test_runtime.cpp — runtime (kiln/assets.h) through the null adapter.
// Tests that leave ContextDesc::storeDir empty use golden_store_dir(), the goldens as a manifest store.
// RuntimePanic.* cases abort on purpose; they run only when selected by exact name (own CTest entries).
#include "kiln_test.h"

#include "hand_store.h"
#include "ktx2_corpus.h" // corpus::read_file
#include "profile_log.h"

#include "kiln/assets.h"
#include "kiln/manifest.h"
#include "kiln/null_adapter.h"
#include "kiln/placeholders.h"

#if defined(KILN_TEST_HAS_COOK) && KILN_TEST_HAS_COOK
#include "kiln/cook/ktx2_writer.h" // TextureArrayBcLayers, TextureArrayMixedSupercompression
#endif

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
    DiagSink sink() { return {&fn, this}; }
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
    int find_event(EventKind kind, u64 bits, usize from = 0) const {
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
    auto const resolve = [&buf](StrView owner, StrView uri) {
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

/// The uploaded texture `t` holds the texels of `file` from level `first` on, rows padded to the
/// adapter's pitch.
void check_uploaded(Rt& rt, TextureHandle t, Span<u8 const> file, u64 pitchAlign, char const* what,
                    u32 first = 0, u32 version = 1) {
    Result<ktx2::Ktx2View> kv = ktx2::Ktx2View::open(file);
    KILN_REQUIRE(kv.ok());
    ktx2::TextureDesc const want = kv->desc();

    TextureInfo const ti = texture_info(rt.ctx, t);
    KILN_CHECK(!ti.isPlaceholder);
    KILN_CHECK_EQ(ti.version, version);
    KILN_CHECK_EQ(ti.desc.format, want.format);
    KILN_CHECK_EQ(ti.desc.width, want.width);
    KILN_CHECK_EQ(ti.desc.height, want.height);
    KILN_CHECK_EQ(ti.desc.levels, want.levels);
    KILN_CHECK_EQ(ti.firstLevel, first);
    KILN_REQUIRE(first < want.levels);
    u32 const resident = want.levels - first;
    KILN_REQUIRE_EQ(ti.levelOffsets.size, usize(resident));
    KILN_REQUIRE_EQ(ti.levelRowPitches.size, usize(resident));
    KILN_CHECK(want.levels > 1);

    Span<u8 const> payload = null_adapter_payload(rt.na, ti.gpu);
    KILN_REQUIRE(!payload.empty());
    for (u32 i = 0; i < resident; ++i) {
        u32 const level = first + i;
        u64 const off   = ti.levelOffsets[i];
        u64 const pitch = ti.levelRowPitches[i];
        KILN_CHECK_EQ(off % 64, u64(0));
        if (i > 0) KILN_CHECK(off > ti.levelOffsets[i - 1]);
        KILN_CHECK_EQ(pitch % pitchAlign, u64(0));
        u64 const rowBytes     = format_row_bytes(want.format, kv->level_width(level));
        u32 const rows         = kv->level_height(level);
        u32 const images       = want.layers * want.faces; // each at a stride of pitch * rows
        Vec<u8> const texels   = test::corpus::texels(*kv, level);
        Span<u8 const> const L = texels.span();
        KILN_REQUIRE_EQ(L.size, usize(rowBytes * rows * images));
        KILN_REQUIRE(off + pitch * rows * images <= payload.size);
        bool same = true;
        for (u32 j = 0; j < images; ++j)
            for (u32 r = 0; r < rows; ++r)
                same = same && bytes_equal(payload.data + off + (j * rows + r) * pitch,
                                           L.data + (j * rows + r) * rowBytes, usize(rowBytes));
        KILN_CHECK_MSG(same, "%s: level %u rows differ (pitch align %llu)", what, level,
                       static_cast<unsigned long long>(pitchAlign));
    }
    // The adapter saw the smaller texture: its object is the resident levels' layout.
    TextureDesc const d{.format     = want.format,
                        .width      = max(want.width >> first, 1u),
                        .height     = max(want.height >> first, 1u),
                        .depth      = max(want.depth >> first, 1u),
                        .layers     = want.layers * want.faces,
                        .levels     = resident,
                        .shape      = want.isCube ? TextureShape::Cube : TextureShape::Tex2D,
                        .firstLevel = first};
    CopyConstraints c{};
    rt.adapter.copy_constraints(rt.adapter.user, &c);
    KILN_CHECK_EQ(payload.size, usize(texture_level_layout(d, c, nullptr, nullptr)));
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

// RequestOptions::maxExtent loads the levels from the first one that fits, as a complete, smaller
// texture (docs/design/streaming.md): from the store and from memory, Zstd and plain, a cube; an
// extent above the texture's size and one below its last level; a second request of the live path
// keeps the extent; a reload loads at it.
KILN_TEST(Runtime, LoadTextureLevelLimited) {
    Vec<u8> golden(default_allocator(), Tag::Test), plain(default_allocator(), Tag::Test),
        flipped(default_allocator(), Tag::Test);
    if (!read_golden("ktx2/color_zstd", ".ktx2", golden)) return; // 64x64, 7 Zstd levels
    char path[1024];
    format(path, sizeof path, "%s/generated/rgba8_unorm_mip.ktx2", test::corpus_dir()); // 16x16, 5 levels
    KILN_REQUIRE(test::corpus::read_file(path, plain));
    flipped.resize(plain.size());
    std::memcpy(flipped.data(), plain.data(), plain.size());
    {
        Result<ktx2::Ktx2View> const v = ktx2::Ktx2View::open(plain.span());
        KILN_REQUIRE(v.ok());
        for (ktx2::LevelIndex const& li : v->levels())
            for (u64 b = 0; b < li.byteLength; ++b)
                flipped[usize(li.byteOffset + b)] ^= u8(0x5A + b);
    }
    test::HandStore hand;
    if (!hand.init("level_limited")) return;
    format(path, sizeof path, "%s/generated/cube_rgba8_srgb_mip.ktx2", test::corpus_dir()); // 8x8, 4 levels
    KILN_REQUIRE(hand.put_file("tex/cube", AssetKind::Texture, path));
    KILN_REQUIRE(hand.put("tex/a", AssetKind::Texture, plain.span()));
    Vec<u8> cube(default_allocator(), Tag::Test);
    KILN_REQUIRE(test::corpus::read_file(path, cube));

    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        nd.offsetAlign   = 64;
        ContextDesc cd;
        cd.storeDir = StrView(hand.dir());
        if (!rt.init(nd, cd)) return;
        TextureHandle const t  = request_texture(rt.ctx, "tex/a", {.maxExtent = 4});
        TextureHandle const tz = register_texture(rt.ctx, "gen/zstd", golden.span(), {.maxExtent = 16});
        TextureHandle const tb = register_texture(rt.ctx, "gen/big", plain.span(), {.maxExtent = 1000});
        TextureHandle const tl = register_texture(rt.ctx, "gen/last", plain.span(), {.maxExtent = 1});
        TextureHandle const tc =
            request_texture(rt.ctx, "tex/cube", {.textureShape = TextureShape::Cube, .maxExtent = 2});
        KILN_REQUIRE(!t.is_null() && !tz.is_null() && !tb.is_null() && !tl.is_null() && !tc.is_null());
        KILN_REQUIRE(rt.pump_until([&] {
            return is_ready(rt.ctx, t) && is_ready(rt.ctx, tz) && is_ready(rt.ctx, tb) &&
                   is_ready(rt.ctx, tl) && is_ready(rt.ctx, tc);
        }));
        KILN_CHECK_MSG(rt.diags.count == 0, "%s", rt.diags.last);
        check_uploaded(rt, t, plain.span(), pitchAlign, "store at 4", 2);
        check_uploaded(rt, tz, golden.span(), pitchAlign, "zstd from memory at 16", 2);
        check_uploaded(rt, tb, plain.span(), pitchAlign, "plain at 1000", 0);
        check_uploaded(rt, tl, plain.span(), pitchAlign, "plain at 1: the last level", 4);
        check_uploaded(rt, tc, cube.span(), pitchAlign, "cube at 2", 2);
        KILN_CHECK(texture_info(rt.ctx, tc).desc.isCube);

        TextureHandle const again = request_texture(rt.ctx, "tex/a", {.maxExtent = 0});
        KILN_CHECK_EQ(again.bits(), t.bits());
        KILN_CHECK_EQ(texture_info(rt.ctx, t).firstLevel, 2u);
        release(rt.ctx, again);

        usize const ev0 = rt.events.size();
        KILN_REQUIRE(hand.put("tex/a", AssetKind::Texture, flipped.span()));
        request_reload(rt.ctx, t);
        KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, t.bits(), ev0) >= 0; }));
        check_uploaded(rt, t, flipped.span(), pitchAlign, "store at 4 after reload", 2, 2);
        KILN_REQUIRE(hand.put("tex/a", AssetKind::Texture, plain.span()));

        release(rt.ctx, t);
        release(rt.ctx, tz);
        release(rt.ctx, tb);
        release(rt.ctx, tl);
        release(rt.ctx, tc);
    }
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
    /// manifest.
    static Status prepare(void* user, AssetKind kind, StrView path, PrepareMode, Allocator const* alloc,
                          Vec<u8>* out, Hash128*, DiagSink const* diag) {
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
    // A manifest hit asks the provider too; with no answer, the manifest entry is used.
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

/// A scratch manifest store `<samples>/reload_<name>`; the tests replace the files of `mesh/thing`
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

/// Pumps for about `ms` milliseconds while the store poller runs.
void pump_for(Rt& rt, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        rt.pump_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// A loaded asset whose entry leaves the manifest stays Ready with one K5022, until its entry is back.
KILN_TEST(Runtime, StorePollerWarnsWhenAnEntryLeaves) {
    ReloadStore store;
    if (!store.init("entry_gone")) return;
    Vec<u8> a(default_allocator(), Tag::Test), b(default_allocator(), Tag::Test);
    if (!read_golden("ktx2/normal", ".ktx2", a) || !read_golden("ktx2/color_srgb", ".ktx2", b)) return;
    KILN_REQUIRE(store.hand.put("a", AssetKind::Texture, a.span()));
    KILN_REQUIRE(store.hand.put("b", AssetKind::Texture, b.span()));

    Rt rt;
    ContextDesc cd;
    cd.storeDir  = store.dir;
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_texture(rt.ctx, "a");
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Ready; }));
    auto gone = [&] { return count_code(rt.diags, kDiagEntryRemoved); };

    KILN_REQUIRE(store.hand.remove("a", AssetKind::Texture));
    KILN_REQUIRE(rt.pump_until([&] { return gone() > 0; }));
    KILN_CHECK_EQ(state(rt.ctx, t), State::Ready);
    KILN_CHECK_EQ(version(rt.ctx, t), 1u);

    KILN_REQUIRE(store.hand.remove("b", AssetKind::Texture)); // another rewrite: no second warning
    pump_for(rt, 300);
    KILN_CHECK_EQ(gone(), 1u);

    KILN_REQUIRE(store.hand.put("a", AssetKind::Texture, a.span())); // back, same artifact: no reload
    pump_for(rt, 300);
    KILN_REQUIRE(store.hand.remove("a", AssetKind::Texture));
    KILN_CHECK(rt.pump_until([&] { return gone() == 2; }));
    KILN_CHECK_EQ(version(rt.ctx, t), 1u);
    release(rt.ctx, t);
}

// After a failed reload, a change to another asset's entry does not reload this one again
// (open-questions R13); a new entry for it does.
KILN_TEST(Runtime, StorePollerSkipsFailedKey) {
    ReloadStore store;
    if (!store.init("poller_failed")) return;
    KILN_REQUIRE(put_golden(store, "mesh/Box", ".mesh"));
    Vec<u8> other(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_golden("mesh/MultiUVTest", ".mesh", other));

    Rt rt;
    ContextDesc cd;
    cd.storeDir  = store.dir;
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    MeshHandle m = request_mesh(rt.ctx, "mesh/thing");
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, m); }));

    KILN_REQUIRE(put_garbage(store));
    KILN_REQUIRE(rt.pump_until([&] { return rt.diags.has(kDiagReloadFailed); }));
    KILN_REQUIRE(store.hand.put("mesh/other", AssetKind::Mesh, other.span()));
    pump_for(rt, 200);
    KILN_CHECK_EQ(count_code(rt.diags, kDiagReloadFailed), 1u);

    usize const ev0 = rt.events.size();
    KILN_REQUIRE(put_golden(store, "mesh/MultiUVTest", ".mesh"));
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, m.bits(), ev0) >= 0; }));
    KILN_CHECK_EQ(version(rt.ctx, m), 2u);
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

/// A scratch manifest store with KTX2 corpus files under short names: sky (cube), layers (array
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

/// A host JobSystem that keeps each job until the test runs it, on the test thread.
struct HeldJobs {
    struct Job {
        void (*fn)(void*);
        void* arg;
    };
    Vec<Job> jobs{default_allocator(), Tag::Test};
    u32 next = 0;

    JobSystem system() { return {.submit = &submit, .user = this}; }
    u32 held() const { return u32(jobs.size()) - next; }
    void run_all() {
        while (next < jobs.size()) {
            Job const j = jobs[next++];
            j.fn(j.arg);
        }
    }
    static void submit(void* user, void (*fn)(void*), void* arg) {
        static_cast<HeldJobs*>(user)->jobs.push_back({fn, arg});
    }
};

// Workers take the prepared jobs by priority, and a job that ends starts the next one without a pump:
// a High request made after the Normal jobs were prepared still runs first.
KILN_TEST(Runtime, PreparedJobsRunByPriority) {
    HeldJobs held;
    JobSystem const js = held.system();
    Rt rt;
    ContextDesc cd;
    cd.jobs      = &js;
    cd.maxIoJobs = 1;
    if (!rt.init({}, cd)) return;
    TextureHandle const a = request_texture(rt.ctx, "ktx2/bc1_high");
    TextureHandle const b = request_texture(rt.ctx, "ktx2/bc5_normal");
    rt.pump_once();
    KILN_CHECK(held.held() == 1); // maxIoJobs = 1: one job runs them all
    TextureHandle const c = request_texture(rt.ctx, "ktx2/bc7_color_srgb", {.priority = Priority::High});
    rt.pump_once();
    KILN_CHECK(held.held() == 1);
    held.run_all(); // the three meta jobs, with no pump between them
    rt.pump_once();
    if (KILN_CHECK(rt.events.size() == 3)) {
        KILN_CHECK(rt.events[0].kind == EventKind::MetaReady && rt.events[0].handle == c.bits());
        KILN_CHECK(rt.events[1].handle == a.bits());
        KILN_CHECK(rt.events[2].handle == b.bits());
    }
    for (int i = 0; i < 8 && !(is_ready(rt.ctx, a) && is_ready(rt.ctx, b) && is_ready(rt.ctx, c)); ++i) {
        held.run_all();
        rt.pump_once();
    }
    KILN_CHECK(is_ready(rt.ctx, a) && is_ready(rt.ctx, b) && is_ready(rt.ctx, c));
    // The uploads ran in the same order: the High one first.
    u32 ready = 0;
    for (Event const& e : rt.events) {
        if (e.kind != EventKind::Ready) continue;
        if (ready++ == 0) KILN_CHECK(e.handle == c.bits());
    }
    KILN_CHECK(ready == 3);
    held.run_all(); // destroy() waits for every submitted job
}

// A prepared job that no worker took yet goes away with its asset: no job runs for it.
KILN_TEST(Runtime, ReleaseDropsPreparedJob) {
    HeldJobs held;
    JobSystem const js = held.system();
    Rt rt;
    ContextDesc cd;
    cd.jobs      = &js;
    cd.maxIoJobs = 1;
    if (!rt.init({}, cd)) return;
    TextureHandle const a = request_texture(rt.ctx, "ktx2/bc1_high");
    TextureHandle const b = request_texture(rt.ctx, "ktx2/bc5_normal");
    rt.pump_once();
    KILN_CHECK(stats(rt.ctx).ioJobsInFlight == 2);
    release(rt.ctx, b);
    KILN_CHECK(stats(rt.ctx).ioJobsInFlight == 1);
    KILN_CHECK(stats(rt.ctx).assets == 1);
    // The slot is free at once: a new request of the name is a new load.
    TextureHandle const b2 = request_texture(rt.ctx, "ktx2/bc5_normal");
    KILN_CHECK(b2 != b && state(rt.ctx, b) == State::Unloaded);
    for (int i = 0; i < 8 && !(is_ready(rt.ctx, a) && is_ready(rt.ctx, b2)); ++i) {
        held.run_all();
        rt.pump_once();
    }
    KILN_CHECK(is_ready(rt.ctx, a) && is_ready(rt.ctx, b2));
    u32 metas = 0;
    for (Event const& e : rt.events)
        metas += e.kind == EventKind::MetaReady;
    KILN_CHECK(metas == 2);
    KILN_CHECK(rt.diags.count == 0);
    held.run_all();
}

// ContextDesc::profiler: zones for the pump and each job, intervals for the waits, the GPU copy and
// the whole load, each with the asset's name; every zone closes on its own thread.
KILN_TEST(Runtime, ProfilerHooks) {
    test::ProfileLog log;
    {
        Rt rt;
        ContextDesc cd;
        cd.profiler = log.hooks();
        if (!rt.init({}, cd)) return;
        KILN_CHECK(profile_hooks(rt.ctx) != nullptr);
        TextureHandle const t = request_texture(rt.ctx, "ktx2/height16");
        MeshHandle const m    = request_mesh(rt.ctx, "mesh/Box");
        KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t) && is_ready(rt.ctx, m); }));
        release(rt.ctx, t);
        release(rt.ctx, m);
    }
    KILN_CHECK(log.well_formed());
    KILN_CHECK(log.count('B', "kiln.pump") > 0);
    static char const* const kAssets[]    = {"ktx2/height16", "mesh/Box"};
    static char const* const kIntervals[] = {"kiln.wait.meta", "kiln.wait.upload", "kiln.gpu", "kiln.load"};
    for (char const* asset : kAssets) {
        KILN_CHECK_MSG(log.count('B', "kiln.meta", asset) == 1, "%s", asset);
        KILN_CHECK_MSG(log.count('B', "kiln.upload", asset) == 1, "%s", asset);
        for (char const* interval : kIntervals)
            KILN_CHECK_MSG(log.count('I', interval, asset) == 1, "%s %s", asset, interval);
        KILN_CHECK_MSG(log.count('I', "kiln.wait.pool", asset) == 2, "%s", asset);
        KILN_CHECK_MSG(log.count('B', "kiln.open", asset) == 2, "%s", asset); // meta, then upload
        KILN_CHECK_MSG(log.count('B', "kiln.read", asset) >= 2, "%s", asset);
    }

    Rt off; // no hooks: nothing to report to
    if (!off.init()) return;
    KILN_CHECK(profile_hooks(off.ctx) == nullptr);
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

// create() checks the manifest's profile against the adapter once (docs/design/target-profiles.md).
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

// A profile that a cook writes after create() is checked when it appears, as a warning.
KILN_TEST(Runtime, ProfileThatAppearsLaterIsChecked) {
    test::HandStore store;
    if (!store.init("runtime_store_profile_later")) return;
    Adapter adapter{};
    Result<NullAdapter*> na = null_adapter_create({}, &adapter);
    KILN_REQUIRE(na.ok());
    adapter.supports_format = &no_bc5;
    DiagLog log;
    ContextDesc cd{};
    cd.adapter         = &adapter;
    cd.storeDir        = StrView(store.dir()); // no manifest yet
    cd.diag            = log.sink();
    Result<Context*> c = create(cd);
    KILN_REQUIRE(c.ok());
    Context* ctx = *c;
    auto settle  = [&](TextureHandle t) {
        for (int i = 0; i < 2000 && state(ctx, t) == State::Pending; ++i) {
            pump(ctx, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    TextureHandle const t = request_texture(ctx, "normal");
    settle(t);
    KILN_CHECK_EQ(state(ctx, t), State::Failed);
    store.set_block_formats(block_format_bit(Format::BC5_UNORM));
    Vec<u8> tex(default_allocator(), Tag::Test);
    if (read_golden("ktx2/normal", ".ktx2", tex) &&
        KILN_CHECK(store.put("normal", AssetKind::Texture, tex.span()))) {
        request_reload(ctx, t); // reads the manifest again: the profile appears
        for (int i = 0; i < 200; ++i) {
            pump(ctx, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        request_reload(ctx, t);
        for (int i = 0; i < 200; ++i) {
            pump(ctx, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        KILN_CHECK_EQ(count_code(log, kDiagStoreProfileUnsampled), 1u);
    }
    release(ctx, t);
    destroy(ctx);
    null_adapter_destroy(*na);
}

// A manifest without the context's profile is reported once, at create(), with the profiles it has.
KILN_TEST(Runtime, MissingProfileWarnsAtCreate) {
    Rt rt;
    ContextDesc cd;
    cd.profile = "desktop"; // the golden store has compat only
    if (!rt.init({}, cd)) return;
    KILN_CHECK_EQ(count_code(rt.diags, kDiagManifestMissing), 1u);
    KILN_CHECK_MSG(std::strstr(rt.diags.last, "'compat'") != nullptr, "%s", rt.diags.last);
}

// A storeDir too long for its manifest path fails create() with a diagnostic.
KILN_TEST(Runtime, TooLongStoreDirIsDiagnosed) {
    char longDir[1100];
    std::memset(longDir, 'a', sizeof longDir - 1);
    longDir[sizeof longDir - 1] = '\0';
    Adapter adapter{};
    Result<NullAdapter*> na = null_adapter_create({}, &adapter);
    KILN_REQUIRE(na.ok());
    DiagLog log;
    ContextDesc cd{};
    cd.adapter         = &adapter;
    cd.storeDir        = StrView(longDir);
    cd.diag            = log.sink();
    Result<Context*> c = create(cd);
    KILN_CHECK(c.failed());
    KILN_CHECK(log.has(kDiagManifestMissing));
    if (c.ok()) destroy(*c);
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

// ---------------------------------------------------------------------------
// Texture arrays from separate 2D textures (docs/design/runtime-texture-arrays.md)
// ---------------------------------------------------------------------------

namespace {

struct ArrayStore {
    test::HandStore hand;
    Vec<u8> plain{default_allocator(), Tag::Test};   ///< RGBA8 UNORM 16x16, 5 levels
    Vec<u8> flipped{default_allocator(), Tag::Test}; ///< `plain` with other texels
    Vec<u8> zstd{default_allocator(), Tag::Test};    ///< RGBA8 sRGB 16x16, 5 Zstd levels

    bool init(char const* name) {
        if (!hand.init(name)) return false;
        char path[1024];
        format(path, sizeof path, "%s/generated/rgba8_unorm_mip.ktx2", test::corpus_dir());
        if (!KILN_CHECK(test::corpus::read_file(path, plain))) return false;
        format(path, sizeof path, "%s/generated/rgba8_srgb_mip_zstd.ktx2", test::corpus_dir());
        if (!KILN_CHECK(test::corpus::read_file(path, zstd))) return false;
        flipped.resize(plain.size());
        std::memcpy(flipped.data(), plain.data(), plain.size());
        Result<ktx2::Ktx2View> const v = ktx2::Ktx2View::open(plain.span());
        if (!KILN_CHECK(v.ok())) return false;
        for (ktx2::LevelIndex const& li : v->levels())
            for (u64 b = 0; b < li.byteLength; ++b)
                flipped[usize(li.byteOffset + b)] ^= u8(0x5A + b);
        Copy const files[] = {
            {"generated/rgba8_unorm_npot_mip.ktx2", "tex/npot"},
            {"generated/cube_rgba8_srgb_mip.ktx2",  "tex/cube"},
        };
        for (Copy const& c : files) {
            format(path, sizeof path, "%s/%s", test::corpus_dir(), c.from);
            if (!hand.put_file(StrView(c.to), AssetKind::Texture, path)) return false;
        }
        return hand.put("tex/a", AssetKind::Texture, plain.span()) &&
               hand.put("tex/b", AssetKind::Texture, flipped.span()) &&
               hand.put("tex/z", AssetKind::Texture, zstd.span());
    }

private:
    struct Copy {
        char const* from;
        char const* to;
    };
};

TextureHandle request_array(Rt& rt, char const* name, std::initializer_list<StrView> layers) {
    return request_texture_array(
        rt.ctx, {.name = StrView(name), .layers = Span<StrView const>(layers.begin(), layers.size())});
}

bool settled_state(Rt& rt, TextureHandle t) {
    State const st = state(rt.ctx, t);
    return st == State::Ready || st == State::Failed;
}

/// Layer `layer` of the array `t` holds the texels of `file`, level by level. Rows and the layer
/// stride are counted in block rows for a compressed format, so a level under one block rounds up.
void check_layer_uploaded(Rt& rt, TextureHandle t, u32 layer, Span<u8 const> file, char const* what,
                          u32 first = 0) {
    Result<ktx2::Ktx2View> kv = ktx2::Ktx2View::open(file);
    KILN_REQUIRE(kv.ok());
    TextureInfo const ti   = texture_info(rt.ctx, t);
    Span<u8 const> payload = null_adapter_payload(rt.na, ti.gpu);
    KILN_REQUIRE(!payload.empty());
    KILN_CHECK_EQ(ti.firstLevel, first);
    KILN_REQUIRE(first < kv->desc().levels);
    KILN_REQUIRE_EQ(ti.levelOffsets.size, usize(kv->desc().levels - first));
    FormatInfo const* fi = format_info(kv->desc().format);
    KILN_REQUIRE(fi != nullptr);
    for (u32 i = 0; i + first < kv->desc().levels; ++i) {
        u32 const level      = first + i;
        u64 const pitch      = ti.levelRowPitches[i];
        u64 const rowBytes   = format_row_bytes(kv->desc().format, kv->level_width(level));
        u32 const rows       = (kv->level_height(level) + fi->blockHeight - 1) / fi->blockHeight;
        u64 const off        = ti.levelOffsets[i] + layer * pitch * rows;
        Vec<u8> const texels = test::corpus::texels(*kv, level);
        KILN_REQUIRE(off + pitch * rows <= payload.size);
        bool same = true;
        for (u32 r = 0; r < rows; ++r)
            same = same &&
                   bytes_equal(payload.data + off + r * pitch, texels.data() + r * rowBytes, usize(rowBytes));
        KILN_CHECK_MSG(same, "%s: layer %u level %u differs", what, layer, level);
    }
}

} // namespace

// Three layers (one repeated) in one upload: each layer lands in its place, with tight and padded rows.
KILN_TEST(Runtime, TextureArrayLoadsLayers) {
    ArrayStore store;
    if (!store.init("arrays")) return;
    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        nd.offsetAlign   = 64;
        ContextDesc cd;
        cd.storeDir = StrView(store.hand.dir());
        if (!rt.init(nd, cd)) return;
        u32 const uploads0    = null_adapter_stats(rt.na).beginUploads;
        TextureHandle const t = request_array(rt, "arr/abc", {"tex/a", "tex/b", "tex/a"});
        KILN_REQUIRE(!t.is_null());
        KILN_CHECK(texture_info(rt.ctx, t).isPlaceholder);
        KILN_CHECK(texture_info(rt.ctx, t).desc.isArray);
        KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
        if (!KILN_CHECK_MSG(is_ready(rt.ctx, t), "%s", rt.diags.last)) return;
        KILN_CHECK(rt.find_event(EventKind::MetaReady, t.bits()) >= 0);
        KILN_CHECK_EQ(null_adapter_stats(rt.na).beginUploads - uploads0, 1u);
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK(ti.desc.isArray && ti.desc.layers == 3 && ti.desc.levels == 5);
        KILN_CHECK_EQ(ti.desc.format, Format::R8G8B8A8_UNORM);
        check_layer_uploaded(rt, t, 0, store.plain.span(), "a");
        check_layer_uploaded(rt, t, 1, store.flipped.span(), "b");
        check_layer_uploaded(rt, t, 2, store.plain.span(), "a again");
        KILN_CHECK_EQ(find_texture(rt.ctx, asset_id("arr/abc")).bits(), t.bits());
        KILN_CHECK(find_texture(rt.ctx, asset_id("tex/a")).is_null()); // layers are not textures of their own
        KILN_CHECK_EQ(rt.diags.count, 0u);
        release(rt.ctx, t);
    }
}

// Zstd layers are decoded into their place.
KILN_TEST(Runtime, TextureArrayZstdLayers) {
    ArrayStore store;
    if (!store.init("arrays_zstd")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({.rowPitchAlign = 256}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/zz", {"tex/z", "tex/z"});
    KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
    if (!KILN_CHECK_MSG(is_ready(rt.ctx, t), "%s", rt.diags.last)) return;
    check_layer_uploaded(rt, t, 0, store.zstd.span(), "z0");
    check_layer_uploaded(rt, t, 1, store.zstd.span(), "z1");
    release(rt.ctx, t);
}

// TextureArrayDesc::maxExtent: every layer starts at the same level, and the object is the smaller
// array. The same declaration with another extent is the same handle and keeps its extent.
KILN_TEST(Runtime, TextureArrayLevelLimited) {
    ArrayStore store;
    if (!store.init("arrays_limited")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({.rowPitchAlign = 256, .offsetAlign = 64}, cd)) return;
    StrView const layers[] = {"tex/a", "tex/b", "tex/a"};
    TextureHandle const t  = request_texture_array(
        rt.ctx, {.name = "arr/small", .layers = Span<StrView const>(layers), .maxExtent = 4});
    KILN_REQUIRE(!t.is_null());
    KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
    if (!KILN_CHECK_MSG(is_ready(rt.ctx, t), "%s", rt.diags.last)) return;
    TextureInfo const ti = texture_info(rt.ctx, t);
    KILN_CHECK(ti.desc.isArray && ti.desc.layers == 3 && ti.desc.levels == 5 && ti.desc.width == 16);
    KILN_CHECK_EQ(ti.firstLevel, 2u);
    KILN_CHECK_EQ(ti.levelOffsets.size, usize(3));
    check_layer_uploaded(rt, t, 0, store.plain.span(), "a", 2);
    check_layer_uploaded(rt, t, 1, store.flipped.span(), "b", 2);
    check_layer_uploaded(rt, t, 2, store.plain.span(), "a again", 2);
    TextureDesc const d{.format     = ti.desc.format,
                        .width      = 4,
                        .height     = 4,
                        .depth      = 1,
                        .layers     = 3,
                        .levels     = 3,
                        .shape      = TextureShape::Array,
                        .firstLevel = 2};
    CopyConstraints c{};
    rt.adapter.copy_constraints(rt.adapter.user, &c);
    KILN_CHECK_EQ(null_adapter_payload(rt.na, ti.gpu).size,
                  usize(texture_level_layout(d, c, nullptr, nullptr)));

    TextureHandle const again = request_texture_array(
        rt.ctx, {.name = "arr/small", .layers = Span<StrView const>(layers), .maxExtent = 0});
    KILN_CHECK_EQ(again.bits(), t.bits());
    KILN_CHECK_EQ(texture_info(rt.ctx, t).firstLevel, 2u);
    release(rt.ctx, again);
    release(rt.ctx, t);
}

// A layer that differs from layer 0, is not 2D, or is missing fails the array and names the layer.
KILN_TEST(Runtime, TextureArrayLayerFailures) {
    ArrayStore store;
    if (!store.init("arrays_bad")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({}, cd)) return;
    struct Case {
        char const* name;
        StrView layers[2];
        u32 code;
        char const* says;
    };
    Case const cases[] = {
        {"arr/size",
         {"tex/a", "tex/npot"},
         kDiagArrayLayerMismatch,                                      "layer 1 (tex/npot) is R8G8B8A8_UNORM 7x5"},
        {"arr/format", {"tex/a", "tex/z"},    kDiagArrayLayerMismatch, "layer 1 (tex/z) is R8G8B8A8_SRGB"        },
        {"arr/cube",   {"tex/cube", "tex/a"}, kDiagArrayLayerMismatch, "layer 0 (tex/cube) is cube"              },
        {"arr/miss",   {"tex/a", "tex/none"}, kDiagStoreMiss,          "layer 1 (tex/none)"                      },
    };
    for (Case const& c : cases) {
        u32 const before      = count_code(rt.diags, c.code);
        TextureHandle const t = request_texture_array(
            rt.ctx, {.name = StrView(c.name), .layers = Span<StrView const>(c.layers, 2)});
        KILN_REQUIRE(!t.is_null());
        KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
        KILN_CHECK_MSG(state(rt.ctx, t) == State::Failed, "%s loaded", c.name);
        KILN_CHECK_MSG(count_code(rt.diags, c.code) > before, "%s: %s", c.name, rt.diags.last);
        KILN_CHECK_MSG(std::strstr(rt.diags.last, c.says) != nullptr, "%s: %s", c.name, rt.diags.last);
        KILN_CHECK(rt.find_event(EventKind::Failed, t.bits()) >= 0);
        release(rt.ctx, t);
    }
}

// The same declaration shares one handle; a name that another list or another texture uses, and a bad
// declaration, are K5020 and a null handle.
KILN_TEST(Runtime, TextureArrayDeclarations) {
    ArrayStore store;
    if (!store.init("arrays_decl")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(!t.is_null());
    TextureHandle const again = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_CHECK_EQ(again.bits(), t.bits());
    KILN_CHECK_EQ(rt.diags.count, 0u);

    TextureHandle const a = request_texture(rt.ctx, "tex/a");
    KILN_CHECK(request_array(rt, "arr/ab", {"tex/b", "tex/a"}).is_null()); // another list
    KILN_CHECK(request_texture(rt.ctx, "arr/ab").is_null());               // not a plain texture
    KILN_CHECK(request_array(rt, "tex/a", {"tex/b"}).is_null());           // a texture's name
    KILN_CHECK(request_texture_array(rt.ctx, {.name = "arr/none"}).is_null());
    KILN_CHECK(request_array(rt, "arr/bad", {"tex/a", "../x"}).is_null());
    KILN_CHECK(request_array(rt, "arr/self", {"arr/self"}).is_null());
    KILN_CHECK_EQ(count_code(rt.diags, kDiagArrayDeclaration), 6u);
    static StrView many[kMaxTextureArrayLayers + 1];
    for (StrView& m : many)
        m = "tex/a";
    KILN_CHECK(request_texture_array(rt.ctx,
                                     {.name = "arr/many", .layers = Span<StrView const>(many, countof(many))})
                   .is_null());
    KILN_CHECK_EQ(count_code(rt.diags, kDiagArrayDeclaration), 7u);

    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t) && is_ready(rt.ctx, a); }));
    release(rt.ctx, again);
    KILN_CHECK(is_ready(rt.ctx, t)); // one reference left
    release(rt.ctx, t);
    KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
    release(rt.ctx, a);
}

// destroy() frees the declarations of arrays the host never released.
KILN_TEST(Runtime, TextureArrayDestroyWithLiveArray) {
    ArrayStore store;
    if (!store.init("arrays_destroy")) return;
    u64 const reg0 = default_alloc_stats(Tag::Registry).bytesCurrent;
    {
        Rt rt;
        ContextDesc cd;
        cd.storeDir = StrView(store.hand.dir());
        if (!rt.init({}, cd)) return;
        TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
        KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
        KILN_REQUIRE(!request_array(rt, "arr/loading", {"tex/b", "tex/a", "tex/z"}).is_null());
        rt.pump_once();
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Registry).bytesCurrent, reg0);
}

// Without kArrayTextures an array fails like any array request (K5004).
KILN_TEST(Runtime, TextureArrayNeedsArrayCaps) {
    ArrayStore store;
    if (!store.init("arrays_caps")) return;
    Rt rt;
    Result<NullAdapter*> na = null_adapter_create({}, &rt.adapter);
    KILN_REQUIRE(na.ok());
    rt.na = *na;
    rt.adapter.caps &= ~u32(kArrayTextures);
    Result<Context*> c = create(
        ContextDesc{.diag = rt.diags.sink(), .adapter = &rt.adapter, .storeDir = StrView(store.hand.dir())});
    KILN_REQUIRE(c.ok());
    rt.ctx                = *c;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    release(rt.ctx, t);
}

// request_reload() loads the whole array again; a layer that no longer matches keeps the old array.
KILN_TEST(Runtime, TextureArrayReload) {
    ArrayStore store;
    if (!store.init("arrays_reload")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    usize const ev0 = rt.events.size();

    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.plain.span()));
    request_reload(rt.ctx, t);
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, t.bits(), ev0) >= 0; }));
    KILN_CHECK_EQ(version(rt.ctx, t), 2u);
    check_layer_uploaded(rt, t, 1, store.plain.span(), "b after reload");

    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.zstd.span()));
    request_reload(rt.ctx, t);
    KILN_REQUIRE(rt.pump_until([&] { return rt.diags.has(kDiagReloadFailed); }));
    KILN_CHECK(is_ready(rt.ctx, t));
    KILN_CHECK_EQ(version(rt.ctx, t), 2u);
    release(rt.ctx, t);
}

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
// The store poller reloads an array when a layer's entry changes, and only then.
KILN_TEST(Runtime, TextureArrayStorePoller) {
    ArrayStore store;
    if (!store.init("arrays_poller")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir  = StrView(store.hand.dir());
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    usize const ev0 = rt.events.size();
    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.plain.span()));
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, t.bits(), ev0) >= 0; }));
    check_layer_uploaded(rt, t, 1, store.plain.span(), "b after the poller");

    usize const ev1 = rt.events.size();
    KILN_REQUIRE(store.hand.put("tex/other", AssetKind::Texture, store.zstd.span()));
    for (int i = 0; i < 40; ++i) {
        rt.pump_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    KILN_CHECK_EQ(count_events(rt, EventKind::Changed, t.bits(), ev1), 0u);
    release(rt.ctx, t);
}

// After a failed reload, an array reloads only for a layer entry that neither the loaded array nor
// the failed attempt used (open-questions R13).
KILN_TEST(Runtime, TextureArrayStorePollerSkipsFailedKey) {
    ArrayStore store;
    if (!store.init("arrays_poller_failed")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir  = StrView(store.hand.dir());
    cd.hotReload = {.watchStore = true, .pollMs = 20};
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));

    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.zstd.span()));
    KILN_REQUIRE(rt.pump_until([&] { return rt.diags.has(kDiagReloadFailed); }));
    KILN_REQUIRE(store.hand.put("tex/other", AssetKind::Texture, store.plain.span()));
    pump_for(rt, 200);
    KILN_CHECK_EQ(count_code(rt.diags, kDiagReloadFailed), 1u);

    usize const ev0 = rt.events.size();
    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.plain.span()));
    KILN_REQUIRE(rt.pump_until([&] { return rt.find_event(EventKind::Changed, t.bits(), ev0) >= 0; }));
    check_layer_uploaded(rt, t, 1, store.plain.span(), "b after the failed reload");
    release(rt.ctx, t);
}
#endif

// release() while an array is still loading frees its slot and declaration at once: no leak, no
// event for the handle after its release, and the name is free for a fresh, even different, declaration.
KILN_TEST(Runtime, TextureArrayReleaseWhileLoading) {
    ArrayStore store;
    if (!store.init("arrays_release_loading")) return;
    u64 const reg0 = default_alloc_stats(Tag::Registry).bytesCurrent;
    {
        Rt rt;
        ContextDesc cd;
        cd.storeDir = StrView(store.hand.dir());
        if (!rt.init({}, cd)) return;
        for (int round = 0; round < 4; ++round) {
            TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
            KILN_REQUIRE(!t.is_null());
            for (int i = 0; i < round; ++i)
                rt.pump_once(); // 0: right after the request; 1..3: meta or upload job in flight
            usize const ev0 = rt.events.size(); // events before the release are legitimate
            release(rt.ctx, t);
            KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
            KILN_REQUIRE(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
            for (EventKind k :
                 {EventKind::MetaReady, EventKind::Ready, EventKind::Failed, EventKind::Changed})
                KILN_CHECK(rt.find_event(k, t.bits(), ev0) < 0);

            // A different list under the same name succeeds once the old declaration is truly gone.
            TextureHandle const again = request_array(rt, "arr/ab", {"tex/b", "tex/a"});
            KILN_REQUIRE(!again.is_null());
            KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, again); }));
            KILN_CHECK(is_ready(rt.ctx, again));
            release(rt.ctx, again);
            KILN_REQUIRE(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
        }
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Registry).bytesCurrent, reg0);
}

// release() during a reload frees the slot the same way: no leak, no event past the release, and
// the name is free again once the abandoned reload drains.
KILN_TEST(Runtime, TextureArrayReleaseDuringReload) {
    ArrayStore store;
    if (!store.init("arrays_release_reload")) return;
    u64 const reg0 = default_alloc_stats(Tag::Registry).bytesCurrent;
    {
        Rt rt;
        ContextDesc cd;
        cd.storeDir = StrView(store.hand.dir());
        if (!rt.init({}, cd)) return;
        TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
        KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));

        // `plain` keeps tex/b's format, so the abandoned reload (and later requests) stay valid.
        KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.plain.span()));
        request_reload(rt.ctx, t);
        rt.pump_once(); // dispatches the reload's meta job
        KILN_CHECK(stats(rt.ctx).ioJobsInFlight > 0);
        usize const ev0 = rt.events.size();
        release(rt.ctx, t);
        KILN_CHECK_EQ(state(rt.ctx, t), State::Unloaded);
        KILN_REQUIRE(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
        for (EventKind k : {EventKind::MetaReady, EventKind::Ready, EventKind::Failed, EventKind::Changed})
            KILN_CHECK(rt.find_event(k, t.bits(), ev0) < 0);

        TextureHandle const again = request_array(rt, "arr/ab", {"tex/a", "tex/a"}); // a different list
        KILN_REQUIRE(!again.is_null());
        KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, again); }));
        KILN_CHECK(is_ready(rt.ctx, again));
        release(rt.ctx, again);
        KILN_REQUIRE(rt.pump_until([&] { return stats(rt.ctx).ioJobsInFlight == 0; }));
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Registry).bytesCurrent, reg0);
}

// An array bigger than the adapter's staging cap fails with K5004 within a bounded number of
// pumps; it never stays Busy forever (docs/design/runtime-texture-arrays.md).
KILN_TEST(Runtime, TextureArrayExceedsStaging) {
    ArrayStore store;
    if (!store.init("arrays_staging")) return;
    Rt rt;
    NullAdapterDesc nd;
    // Comfortably above any placeholder create() uploads (the largest is the cube Failed checker,
    // 8x8 x 6 faces = 1536 bytes), far below the two-layer, 5-level RGBA8 16x16 array (~2.7 KiB).
    nd.maxUploadBytes = 2048;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init(nd, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return state(rt.ctx, t) == State::Failed; }));
    KILN_CHECK(rt.diags.has(kDiagAdapterRejected));
    KILN_CHECK_EQ(null_adapter_stats(rt.na).busyReturned, 0u);
    release(rt.ctx, t);
}

// Several request_reload() calls while one is already running coalesce into exactly one more
// reload, and the version that lands reflects the store's latest content, not an intermediate one.
KILN_TEST(Runtime, TextureArrayReloadMerging) {
    ArrayStore store;
    if (!store.init("arrays_reload_merge")) return;
    Rt rt;
    ContextDesc cd;
    cd.storeDir = StrView(store.hand.dir());
    if (!rt.init({}, cd)) return;
    TextureHandle const t = request_array(rt, "arr/ab", {"tex/a", "tex/b"});
    KILN_REQUIRE(rt.pump_until([&] { return is_ready(rt.ctx, t); }));
    usize const ev0 = rt.events.size();

    // tex/b starts as `flipped`; both `plain` and `flipped` are R8G8B8A8_UNORM like tex/a, so
    // either reload succeeds (unlike `zstd`, which TextureArrayReload uses to make one fail).
    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.plain.span()));
    request_reload(rt.ctx, t);
    rt.pump_once(); // dispatches the first reload's meta job
    KILN_CHECK(stats(rt.ctx).ioJobsInFlight > 0);
    for (int i = 0; i < 3; ++i)
        request_reload(rt.ctx, t); // coalesced into one pending flag, not a queue of three
    KILN_REQUIRE(store.hand.put("tex/b", AssetKind::Texture, store.flipped.span())); // the content that lands

    KILN_REQUIRE(rt.pump_until([&] { return count_events(rt, EventKind::Changed, t.bits(), ev0) == 2; }));
    for (int i = 0; i < 5; ++i)
        rt.pump_once();
    KILN_CHECK_EQ(count_events(rt, EventKind::Changed, t.bits(), ev0), 2u);
    KILN_CHECK_EQ(version(rt.ctx, t), 3u);
    check_layer_uploaded(rt, t, 1, store.flipped.span(), "b after the merged reload");
    KILN_CHECK_EQ(rt.diags.count, 0u);
    release(rt.ctx, t);
}

#if defined(KILN_TEST_HAS_COOK) && KILN_TEST_HAS_COOK

namespace {

/// A synthetic BC7_UNORM 16x16 image with its full mip chain down to 1x1 (5 levels), written with
/// the KTX2 writer: no corpus file has a full BC mip chain.
struct BcImage {
    static constexpr u32 kWidth  = 16;
    static constexpr u32 kHeight = 16;
    static constexpr u32 kLevels = 5;

    Vec<u8> data[kLevels];
    Span<u8 const> spans[kLevels];

    explicit BcImage(u8 seed) {
        for (u32 i = 0; i < kLevels; ++i) {
            u64 const n = format_image_bytes(Format::BC7_UNORM, max(kWidth >> i, 1u), max(kHeight >> i, 1u));
            data[i].init(default_allocator(), Tag::Test);
            data[i].resize(usize(n));
            for (usize k = 0; k < data[i].size(); ++k)
                data[i][k] = u8(seed + i * 41u + k * 11u);
            spans[i] = data[i].span();
        }
    }

    Vec<u8> write() const {
        ktx2::WriteDesc const wd{.format = Format::BC7_UNORM,
                                 .width  = kWidth,
                                 .height = kHeight,
                                 .levels = Span<Span<u8 const> const>(spans, kLevels)};
        Result<Vec<u8>> r = ktx2::write(wd, default_allocator());
        KILN_CHECK(r.ok());
        return r.ok() ? std::move(r).value() : Vec<u8>(default_allocator(), Tag::Test);
    }
};

/// A Zstd-supercompressed copy of `plainFile`'s exact content (same format/size/levels): mixing
/// with a plain layer needs two files that agree on everything but supercompression.
Vec<u8> zstd_copy(Span<u8 const> plainFile) {
    Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(plainFile);
    KILN_CHECK(v.ok());
    if (!v.ok()) return Vec<u8>(default_allocator(), Tag::Test);
    u32 const levels = v->desc().levels;
    Vec<u8> data[ktx2::kMaxLevels];
    Span<u8 const> spans[ktx2::kMaxLevels];
    for (u32 i = 0; i < levels; ++i) {
        data[i]  = test::corpus::texels(*v, i);
        spans[i] = data[i].span();
    }
    ktx2::WriteDesc const wd{.format    = v->desc().format,
                             .width     = v->desc().width,
                             .height    = v->desc().height,
                             .levels    = Span<Span<u8 const> const>(spans, levels),
                             .zstdLevel = 6};
    Result<Vec<u8>> r = ktx2::write(wd, default_allocator());
    KILN_CHECK(r.ok());
    return r.ok() ? std::move(r).value() : Vec<u8>(default_allocator(), Tag::Test);
}

} // namespace

// A full BC mip chain, with tail levels under one block rounded up to one, loads byte-exactly
// with tight and padded rows.
KILN_TEST(Runtime, TextureArrayBcLayers) {
    test::HandStore hand;
    if (!hand.init("arrays_bc")) return;
    BcImage const a(11), b(97);
    Vec<u8> const fileA = a.write();
    Vec<u8> const fileB = b.write();
    if (fileA.empty() || fileB.empty()) return;
    KILN_REQUIRE(hand.put("bc/a", AssetKind::Texture, fileA.span()));
    KILN_REQUIRE(hand.put("bc/b", AssetKind::Texture, fileB.span()));

    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        ContextDesc cd;
        cd.storeDir = StrView(hand.dir());
        if (!rt.init(nd, cd)) return;
        TextureHandle const t = request_array(rt, "arr/bc", {"bc/a", "bc/b"});
        KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
        if (!KILN_CHECK_MSG(is_ready(rt.ctx, t), "%s", rt.diags.last)) return;
        TextureInfo const ti = texture_info(rt.ctx, t);
        KILN_CHECK_EQ(ti.desc.levels, BcImage::kLevels);
        KILN_CHECK(ti.desc.format == Format::BC7_UNORM);
        check_layer_uploaded(rt, t, 0, fileA.span(), "bc a");
        check_layer_uploaded(rt, t, 1, fileB.span(), "bc b");
        release(rt.ctx, t);
    }
}

// A plain layer and a Zstd layer of the same format/size/levels mix in one array; each decodes
// into its place. Also covers Zstd rows with no padding (rowPitchAlign 1).
KILN_TEST(Runtime, TextureArrayMixedSupercompression) {
    ArrayStore store;
    if (!store.init("arrays_mixed_sc")) return;
    Vec<u8> const zstdOfPlain = zstd_copy(store.plain.span());
    if (zstdOfPlain.empty()) return;
    Result<ktx2::Ktx2View> const check = ktx2::Ktx2View::open(zstdOfPlain.span());
    KILN_REQUIRE(check.ok());
    KILN_CHECK_EQ(check->header().supercompressionScheme, u32(ktx2::Supercompression::Zstd));
    KILN_REQUIRE(store.hand.put("tex/a_zstd", AssetKind::Texture, zstdOfPlain.span()));

    for (u64 pitchAlign : {u64(1), u64(256)}) {
        Rt rt;
        NullAdapterDesc nd;
        nd.rowPitchAlign = pitchAlign;
        ContextDesc cd;
        cd.storeDir = StrView(store.hand.dir());
        if (!rt.init(nd, cd)) return;
        TextureHandle const t = request_array(rt, "arr/mixed", {"tex/a", "tex/a_zstd"});
        KILN_REQUIRE(rt.pump_until([&] { return settled_state(rt, t); }));
        if (!KILN_CHECK_MSG(is_ready(rt.ctx, t), "%s", rt.diags.last)) return;
        check_layer_uploaded(rt, t, 0, store.plain.span(), "plain");
        check_layer_uploaded(rt, t, 1, store.plain.span(), "zstd copy");
        release(rt.ctx, t);
    }
}

#endif // KILN_TEST_HAS_COOK
