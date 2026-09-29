// tests/test_catalog.cpp — build keys, cook units and the catalog format
// (docs/design/store-catalog.md); cook-only.
#include "kiln_test.h"

#include "../src/cook/catalog_store.h"
#include "../src/cook/unit.h"
#include "../src/formats/formats_internal.h"
#include "kiln/cook/catalog.h"
#include "kiln/cook/cook.h"

#include <cstring>
#include <filesystem>
#include <system_error>

using namespace kiln;
using namespace kiln::cook;

namespace {

Hash128 content_of(char const* s) {
    return xxh3_128(Span<u8 const>(reinterpret_cast<u8 const*>(s), std::strlen(s)));
}

} // namespace

KILN_TEST(BuildKey, EveryFieldCounts) {
    BuildInput const inputs[] = {
        {InputRole::Source, "a.glb", content_of("glb")},
        {InputRole::Buffer, "a.bin", content_of("bin")},
    };
    BuildKeyDesc const base{
        .kind = AssetKind::Mesh, .name = "a.glb", .targetHash = 1, .settingsHash = 2, .inputs = inputs};
    Hash128 const k = build_key(base);
    KILN_CHECK(k == build_key(base));

    BuildKeyDesc d = base;
    d.kind         = AssetKind::Texture;
    KILN_CHECK(!(build_key(d) == k));
    d      = base;
    d.name = "b.glb";
    KILN_CHECK(!(build_key(d) == k));
    d            = base;
    d.targetHash = 3;
    KILN_CHECK(!(build_key(d) == k));
    d              = base;
    d.settingsHash = 3;
    KILN_CHECK(!(build_key(d) == k));
    d        = base;
    d.inputs = Span<BuildInput const>(inputs, 1);
    KILN_CHECK(!(build_key(d) == k));

    BuildInput changed[] = {inputs[0], inputs[1]};
    changed[1].content   = content_of("bin2");
    d                    = base;
    d.inputs             = changed;
    KILN_CHECK(!(build_key(d) == k));
    changed[1]      = inputs[1];
    changed[1].name = "c.bin";
    KILN_CHECK(!(build_key(d) == k));
    changed[1]      = inputs[1];
    changed[1].role = InputRole::Sidecar;
    KILN_CHECK(!(build_key(d) == k));

    // A length prefix keeps name boundaries apart.
    BuildInput const split[] = {
        {InputRole::Source, "ab", {}},
        {InputRole::Source, "c",  {}}
    };
    BuildInput const moved[] = {
        {InputRole::Source, "a",  {}},
        {InputRole::Source, "bc", {}}
    };
    d                = base;
    d.inputs         = split;
    Hash128 const ks = build_key(d);
    d.inputs         = moved;
    KILN_CHECK(!(build_key(d) == ks));
}

KILN_TEST(CookUnit, RecordsEveryInputFile) {
    char dir[1024], source[1024];
    format(dir, sizeof dir, "%s/../gltf/generated", kiln::test::corpus_dir());
    format(source, sizeof source, "%s/external_uri.gltf", dir);

    MeshCookSettings const mesh;
    TextureCookSettings const tex;
    CookUnit unit(default_allocator());
    UnitDesc const d{.kind            = AssetKind::Mesh,
                     .name            = "external_uri.gltf",
                     .sourcePath      = StrView(source),
                     .meshDefaults    = &mesh,
                     .textureDefaults = &tex,
                     .target          = &kCompatTarget,
                     .statInputs      = true};
    KILN_REQUIRE(cook_unit(d, &unit).ok());

    // The source, its absent sidecar, and the buffer; the image it references by URI is an asset.
    KILN_REQUIRE_EQ(unit.inputs.size(), usize(3));
    KILN_CHECK(unit.inputs[0].role == InputRole::Source);
    KILN_CHECK(unit.str(unit.inputs[0].nameOff, unit.inputs[0].nameLen) == "external_uri.gltf"_sv);
    KILN_CHECK(unit.inputs[0].stat.size > 0);
    KILN_CHECK(unit.inputs[1].role == InputRole::Sidecar);
    KILN_CHECK(!unit.inputs[1].present);
    KILN_CHECK(unit.inputs[2].role == InputRole::Buffer);
    KILN_CHECK(unit.str(unit.inputs[2].nameOff, unit.inputs[2].nameLen) == "external_uri.bin"_sv);
    KILN_CHECK(unit.inputs[2].present && !unit.inputs[2].content.is_zero());

    KILN_REQUIRE_EQ(unit.outputs.size(), usize(1));
    UnitOutput const& o = unit.outputs[0];
    KILN_CHECK(o.kind == AssetKind::Mesh && o.status.ok() && !o.bytes.empty());

    // The key is the build key of the recorded inputs.
    BuildInput inputs[3];
    unit_build_inputs(unit, inputs);
    KILN_CHECK(o.key == build_key({.kind         = AssetKind::Mesh,
                                   .name         = "external_uri.gltf",
                                   .targetHash   = hash_target(kCompatTarget),
                                   .settingsHash = o.settingsHash,
                                   .inputs       = inputs}));

    // The same cook gives the same key and bytes.
    CookUnit again(default_allocator());
    KILN_REQUIRE(cook_unit(d, &again).ok());
    KILN_CHECK(again.outputs[0].key == o.key);
    KILN_CHECK(again.outputs[0].bytes.size() == o.bytes.size() &&
               std::memcmp(again.outputs[0].bytes.data(), o.bytes.data(), o.bytes.size()) == 0);
}

KILN_TEST(CookUnit, EmbeddedImagesAreOutputs) {
    char source[1024];
    format(source, sizeof source, "%s/../gltf/generated/pbr_textures.glb", kiln::test::corpus_dir());
    MeshCookSettings const mesh;
    TextureCookSettings const tex;
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_unit({.kind            = AssetKind::Mesh,
                            .name            = "pbr_textures.glb",
                            .sourcePath      = StrView(source),
                            .meshDefaults    = &mesh,
                            .textureDefaults = &tex,
                            .target          = &kCompatTarget},
                           &unit)
                     .ok());
    KILN_REQUIRE(unit.outputs.size() > usize(1));
    KILN_CHECK(unit.outputs[0].kind == AssetKind::Mesh);
    for (usize i = 1; i < unit.outputs.size(); ++i) {
        UnitOutput const& o = unit.outputs[i];
        KILN_CHECK(o.kind == AssetKind::Texture && o.status.ok());
        KILN_CHECK(unit.name(o).starts_with("pbr_textures.glb#"_sv));
        KILN_CHECK(!(o.key == unit.outputs[0].key));
        KILN_CHECK(unit.find(AssetKind::Texture, unit.name(o)) == &o);
    }
}

// ---------------------------------------------------------------------------
// Catalog format 0.1
// ---------------------------------------------------------------------------

namespace {

struct LastCode {
    u32 code = 0;
    static void fn(void* user, Diagnostic const& d) noexcept { static_cast<LastCode*>(user)->code = d.code; }
};

Hash128 key_of(char const* s) { return content_of(s); }

/// Three entries, deliberately out of order; "b.glb" has a mesh and an embedded texture name.
struct SampleEntries {
    CatalogEntry e[4] = {
        {"textures/rock.png", AssetKind::Texture, key_of("1"), key_of("c1"), 100},
        {"b.glb",             AssetKind::Mesh,    key_of("2"), key_of("c2"), 200},
        {"a.glb",             AssetKind::Mesh,    key_of("3"), key_of("c3"), 300},
        {"b.glb#img",         AssetKind::Texture, key_of("4"), key_of("c4"), 400},
    };
};

CatalogProfile const kProfile{.name = "compat", .hash = 0x1234, .blockFormats = 0x55};

Vec<u8> write_sample(Span<CatalogEntry const> entries) {
    Vec<u8> out(default_allocator(), Tag::Test);
    KILN_CHECK(write_catalog({.profile = kProfile, .entries = entries}, &out).ok());
    return out;
}

void reseal(Vec<u8>& b) {
    Hash128 const h = fmt::xxh3_128_zeroed(Span<u8 const>(b.data(), b.size()), kCatalogChecksumOffset, 16);
    std::memcpy(b.data() + kCatalogChecksumOffset, h.bytes, 16);
}

u32 open_code(Vec<u8> const& b) {
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    return CatalogView::open(Span<u8 const>(b.data(), b.size()), &sink).ok() ? 0 : lc.code;
}

} // namespace

KILN_TEST(Catalog, RoundTrip) {
    SampleEntries s;
    Vec<u8> const bytes = write_sample(s.e);
    KILN_CHECK_EQ(bytes.size() % 8, usize(0));
    Result<CatalogView> r = CatalogView::open(Span<u8 const>(bytes.data(), bytes.size()));
    KILN_REQUIRE(r.ok());
    CatalogView const& v = *r;
    KILN_CHECK_EQ(v.size(), u64(4));
    KILN_CHECK(v.profile().name == "compat"_sv);
    KILN_CHECK_EQ(v.profile().hash, u64(0x1234));
    KILN_CHECK_EQ(v.profile().blockFormats, u64(0x55));

    // Sorted by name bytes.
    KILN_CHECK(v.entry(0).name == "a.glb"_sv);
    KILN_CHECK(v.entry(1).name == "b.glb"_sv);
    KILN_CHECK(v.entry(2).name == "b.glb#img"_sv);
    KILN_CHECK(v.entry(3).name == "textures/rock.png"_sv);

    for (CatalogEntry const& want : s.e) {
        CatalogEntry got;
        KILN_REQUIRE(v.find(want.kind, want.name, &got));
        KILN_CHECK(got.name == want.name && got.kind == want.kind);
        KILN_CHECK(got.key == want.key && got.checksum == want.checksum);
        KILN_CHECK_EQ(got.bytes, want.bytes);
    }
    CatalogEntry none;
    KILN_CHECK(!v.find(AssetKind::Texture, "a.glb"_sv, &none)); // right name, wrong kind
    KILN_CHECK(!v.find(AssetKind::Mesh, "c.glb"_sv, &none));
}

KILN_TEST(Catalog, SameEntriesSameBytes) {
    SampleEntries s;
    Vec<u8> const a               = write_sample(s.e);
    CatalogEntry const reversed[] = {s.e[3], s.e[2], s.e[1], s.e[0]};
    Vec<u8> const b               = write_sample(reversed);
    KILN_REQUIRE_EQ(a.size(), b.size());
    KILN_CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

KILN_TEST(Catalog, Empty) {
    Vec<u8> const bytes   = write_sample({});
    Result<CatalogView> r = CatalogView::open(Span<u8 const>(bytes.data(), bytes.size()));
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->size(), u64(0));
    CatalogEntry none;
    KILN_CHECK(!r->find(AssetKind::Mesh, "a.glb"_sv, &none));
}

KILN_TEST(Catalog, WriterRejectsDuplicatesAndBadNames) {
    SampleEntries s;
    CatalogEntry const dup[] = {s.e[0], s.e[1], s.e[0]};
    Vec<u8> out(default_allocator(), Tag::Test);
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    KILN_CHECK(write_catalog({.profile = kProfile, .entries = dup}, &out, &sink).code ==
               Code::InvalidArgument);
    KILN_CHECK_EQ(lc.code, u32(kDiagCatalogDuplicate));
    CatalogEntry bad[] = {s.e[0]};
    bad[0].name        = "../up.png";
    KILN_CHECK(write_catalog({.profile = kProfile, .entries = bad}, &out, &sink).failed());
    KILN_CHECK_EQ(lc.code, u32(kDiagCatalogName));
    KILN_CHECK(write_catalog({.profile = {.name = "Compat"}, .entries = {}}, &out, &sink).failed());
}

KILN_TEST(Catalog, ReaderRejectsEachDefect) {
    SampleEntries s;
    Vec<u8> const good = write_sample(s.e);
    KILN_REQUIRE_EQ(open_code(good), u32(0));

    Vec<u8> b(default_allocator(), Tag::Test);
    auto const fresh = [&] {
        b.clear();
        b.append(good.span());
    };

    fresh();
    b[0] = 'X';
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogMagic));
    KILN_CHECK_EQ(open_code(Vec<u8>(default_allocator(), Tag::Test)), u32(kDiagCatalogMagic));

    fresh();
    b[6] = 2; // minor
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogVersion));
    {
        LastCode lc;
        DiagSink const sink{&LastCode::fn, &lc};
        KILN_CHECK(CatalogView::open(Span<u8 const>(b.data(), b.size()), &sink).status().code ==
                   Code::VersionMismatch);
    }

    fresh();
    b.resize(b.size() - 8); // truncated
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogSizes));
    fresh();
    write_unaligned<u64>(b.data() + 24, u64(1) << 40); // entry count
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogSizes));

    fresh();
    b[104] = 1; // reserved
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogReserved));

    // Entry 0 ("a.glb") gets kind 3.
    fresh();
    write_unaligned<u16>(b.data() + kCatalogHeaderBytes + 8, u16(3));
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogName));

    // Swap entries 0 and 1: out of order.
    fresh();
    u8 tmp[kCatalogEntryBytes];
    u8* e0 = b.data() + kCatalogHeaderBytes;
    std::memcpy(tmp, e0, kCatalogEntryBytes);
    std::memcpy(e0, e0 + kCatalogEntryBytes, kCatalogEntryBytes);
    std::memcpy(e0 + kCatalogEntryBytes, tmp, kCatalogEntryBytes);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogOrder));

    // Entry 2 ("b.glb#img", texture) renamed to entry 1's name ("b.glb") and made a mesh.
    fresh();
    u8* e1 = b.data() + kCatalogHeaderBytes + kCatalogEntryBytes;
    std::memcpy(e1 + kCatalogEntryBytes, e1, 12);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogDuplicate));

    // An index record pointing at the wrong entry.
    fresh();
    u64 const index = read_unaligned<u64>(b.data() + 40);
    u64 const e     = read_unaligned<u64>(b.data() + index + 8);
    write_unaligned<u64>(b.data() + index + 8, (e + 1) % 4);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogIndex));

    fresh();
    b[kCatalogHeaderBytes + 20] ^= 1; // a key byte
    KILN_CHECK_EQ(open_code(b), u32(kDiagCatalogChecksum));
}

KILN_TEST(Catalog, Paths) {
    char out[256];
    KILN_CHECK_EQ(catalog_file_path("store"_sv, "compat"_sv, out, sizeof out), usize(26));
    KILN_CHECK(std::strcmp(out, "store/catalogs/compat.kcat") == 0);
    Hash128 k;
    for (u8 i = 0; i < 16; ++i)
        k.bytes[i] = u8(0xa0 + i);
    (void)artifact_file_path("store"_sv, AssetKind::Mesh, k, out, sizeof out);
    KILN_CHECK(std::strcmp(out, "store/artifacts/a0/a0a1a2a3a4a5a6a7a8a9aaabacadaeaf.mesh") == 0);
    (void)artifact_file_path("store"_sv, AssetKind::Texture, k, out, sizeof out);
    KILN_CHECK(std::strcmp(out, "store/artifacts/a0/a0a1a2a3a4a5a6a7a8a9aaabacadaeaf.ktx2") == 0);
    KILN_CHECK(check_profile_name("compat"_sv) == nullptr);
    KILN_CHECK(check_profile_name("my-profile_2"_sv) == nullptr);
    KILN_CHECK(check_profile_name(""_sv) != nullptr);
    KILN_CHECK(check_profile_name("Compat"_sv) != nullptr);
    KILN_CHECK(check_profile_name("a/b"_sv) != nullptr);
}

// ---------------------------------------------------------------------------
// Catalog stores: artifacts, the lock, catalog and input records
// ---------------------------------------------------------------------------

namespace {

/// An empty `<sample_dir()>/<suffix>`.
void fresh_dir(char const* suffix, char* out, usize cap) {
    format(out, cap, "%s/%s", kiln::test::sample_dir(), suffix);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

Status cook_corpus(char const* file, AssetKind kind, CookUnit* unit) {
    static MeshCookSettings const mesh;
    static TextureCookSettings const tex;
    char source[1024];
    format(source, sizeof source, "%s/../gltf/generated/%s", kiln::test::corpus_dir(), file);
    return cook_unit({.kind            = kind,
                      .name            = StrView(file),
                      .sourcePath      = StrView(source),
                      .meshDefaults    = &mesh,
                      .textureDefaults = &tex,
                      .target          = &kCompatTarget,
                      .statInputs      = true},
                     unit);
}

Status open_store(char const* dir, CatalogStore** out, DiagSink const* diag = nullptr) {
    return open_catalog_store({.storeDir = StrView(dir), .target = &kCompatTarget, .diag = diag}, out);
}

bool read_file(char const* path, Vec<u8>& out) {
    return io_read_file(compat_io_backend(), StrView(path), default_allocator(), &out).ok();
}

} // namespace

KILN_TEST(CatalogStore, PublishCommitReopen) {
    char dir[1024];
    fresh_dir("catalog-store", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("pbr_textures.glb", AssetKind::Mesh, &unit).ok());
    KILN_REQUIRE(unit.outputs.size() > usize(1));

    CatalogStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 7, nullptr).ok());
    KILN_REQUIRE(commit_catalog(s, nullptr).ok());
    close_catalog_store(s);

    // The catalog on disk names every output, and each artifact holds its bytes.
    char path[1024];
    (void)catalog_file_path(StrView(dir), "compat"_sv, path, sizeof path);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, bytes));
    Result<CatalogView> v = CatalogView::open(bytes.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->size(), u64(unit.outputs.size()));
    KILN_CHECK(v->profile().name == "compat"_sv);
    KILN_CHECK_EQ(v->profile().hash, hash_target(kCompatTarget));
    for (UnitOutput const& o : unit.outputs) {
        CatalogEntry e;
        KILN_REQUIRE(v->find(o.kind, unit.name(o), &e));
        KILN_CHECK(e.key == o.key);
        KILN_CHECK_EQ(e.bytes, u64(o.bytes.size()));
        (void)artifact_file_path(StrView(dir), o.kind, o.key, path, sizeof path);
        Vec<u8> artifact(default_allocator(), Tag::Test);
        KILN_REQUIRE(read_file(path, artifact));
        KILN_CHECK(xxh3_128(artifact.span()) == e.checksum);
    }

    // A new writer sees the entries and the input record.
    KILN_REQUIRE(open_store(dir, &s).ok());
    Hash128 key;
    KILN_CHECK(catalog_find(s, AssetKind::Mesh, "pbr_textures.glb"_sv, &key) && key == unit.outputs[0].key);
    CookUnit rec(default_allocator());
    u64 digest = 0;
    KILN_REQUIRE(copy_input_record(s, "pbr_textures.glb"_sv, &rec, &digest));
    KILN_CHECK_EQ(digest, u64(7));
    KILN_REQUIRE_EQ(rec.inputs.size(), unit.inputs.size());
    for (usize i = 0; i < rec.inputs.size(); ++i) {
        KILN_CHECK(rec.inputs[i].role == unit.inputs[i].role);
        KILN_CHECK(rec.inputs[i].content == unit.inputs[i].content);
        KILN_CHECK_EQ(rec.inputs[i].stat.mtimeNs, unit.inputs[i].stat.mtimeNs);
        KILN_CHECK(rec.str(rec.inputs[i].pathOff, rec.inputs[i].pathLen) ==
                   unit.str(unit.inputs[i].pathOff, unit.inputs[i].pathLen));
    }
    KILN_REQUIRE_EQ(rec.outputs.size(), unit.outputs.size());
    for (usize i = 0; i < rec.outputs.size(); ++i) {
        KILN_CHECK(rec.name(rec.outputs[i]) == unit.name(unit.outputs[i]));
        KILN_CHECK(rec.outputs[i].key == unit.outputs[i].key);
        KILN_CHECK(rec.outputs[i].slot == unit.outputs[i].slot);
    }
    set_record_digest(s, "pbr_textures.glb"_sv, 8);
    KILN_REQUIRE(commit_catalog(s, nullptr).ok());
    close_catalog_store(s);
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(copy_input_record(s, "pbr_textures.glb"_sv, &rec, &digest));
    KILN_CHECK_EQ(digest, u64(8));
    close_catalog_store(s);
}

KILN_TEST(CatalogStore, OneWriterAtATime) {
    char dir[1024];
    fresh_dir("catalog-lock", dir, sizeof dir);
    CatalogStore* a = nullptr;
    CatalogStore* b = nullptr;
    KILN_REQUIRE(open_store(dir, &a).ok());
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    Status const st = open_store(dir, &b, &sink);
    KILN_CHECK(st.code == Code::Busy);
    KILN_CHECK_EQ(lc.code, u32(kDiagCatalogLocked));
    KILN_CHECK(b == nullptr);
    close_catalog_store(a);
    KILN_REQUIRE(open_store(dir, &b).ok());
    close_catalog_store(b);
}

KILN_TEST(CatalogStore, OtherBytesForAKeyAreReported) {
    char dir[1024];
    fresh_dir("catalog-nondet", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &unit).ok());
    CatalogStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 0, nullptr).ok());

    // Same key, other bytes: the artifact keeps its bytes and the publish fails.
    char path[1024];
    (void)artifact_file_path(StrView(dir), AssetKind::Mesh, unit.outputs[0].key, path, sizeof path);
    Vec<u8> before(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, before));
    CookUnit again(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &again).ok());
    again.outputs[0].bytes[0] ^= 0xFF;
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    KILN_CHECK(publish_unit(s, again, 0, &sink).code == Code::ValidationFailed);
    KILN_CHECK_EQ(lc.code, u32(kDiagNondeterministicCook));
    Vec<u8> after(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, after));
    KILN_CHECK(after.size() == before.size() && std::memcmp(after.data(), before.data(), after.size()) == 0);

    // The same bytes again are fine.
    CookUnit same(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &same).ok());
    KILN_CHECK(publish_unit(s, same, 0, nullptr).ok());
    close_catalog_store(s);
}

KILN_TEST(CatalogStore, OutputsTheCookNoLongerMakesLeave) {
    char dir[1024];
    fresh_dir("catalog-drop", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("pbr_textures.glb", AssetKind::Mesh, &unit).ok());
    KILN_REQUIRE(unit.outputs.size() > usize(2));
    CatalogStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 0, nullptr).ok());

    // A second cook without the last image, and with the first image failing.
    CookUnit fewer(default_allocator());
    KILN_REQUIRE(cook_corpus("pbr_textures.glb", AssetKind::Mesh, &fewer).ok());
    StrView const lastName  = unit.name(unit.outputs.back());
    StrView const firstName = unit.name(unit.outputs[1]);
    fewer.outputs.pop_back();
    fewer.outputs[1].status = make_status(Code::ParseError);
    KILN_REQUIRE(publish_unit(s, fewer, 0, nullptr).ok());
    Hash128 key;
    KILN_CHECK(catalog_find(s, AssetKind::Mesh, "pbr_textures.glb"_sv, &key));
    KILN_CHECK(!catalog_find(s, AssetKind::Texture, lastName, &key));
    KILN_CHECK(!catalog_find(s, AssetKind::Texture, firstName, &key));
    KILN_REQUIRE(commit_catalog(s, nullptr).ok());
    close_catalog_store(s);

    char path[1024];
    (void)catalog_file_path(StrView(dir), "compat"_sv, path, sizeof path);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, bytes));
    Result<CatalogView> v = CatalogView::open(bytes.span());
    KILN_REQUIRE(v.ok());
    KILN_CHECK_EQ(v->size(), u64(unit.outputs.size() - 2));
}

KILN_TEST(CatalogStore, RefusesNamedStoresAndSurvivesLostRecords) {
    char dir[1024];
    fresh_dir("catalog-named", dir, sizeof dir);
    KILN_REQUIRE(bind_store_profile(StrView(dir), kCompatTarget).ok()); // writes kiln-store.txt
    CatalogStore* s = nullptr;
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    KILN_CHECK(open_store(dir, &s, &sink).failed());
    KILN_CHECK_EQ(lc.code, u32(kDiagStoreProfileMismatch));

    fresh_dir("catalog-lost", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &unit).ok());
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 0, nullptr).ok());
    KILN_REQUIRE(commit_catalog(s, nullptr).ok());
    close_catalog_store(s);

    // A damaged input-record file is dropped; the catalog stays.
    char path[1024];
    format(path, sizeof path, "%s/inputs/compat.kin", dir);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, bytes));
    bytes[bytes.size() / 2] ^= 1;
    KILN_REQUIRE(store_write(StrView(dir), "inputs/compat.kin"_sv, bytes.span(), nullptr, true).ok());
    KILN_REQUIRE(open_store(dir, &s).ok());
    Hash128 key;
    KILN_CHECK(catalog_find(s, AssetKind::Mesh, "cube_basic.glb"_sv, &key));
    u64 digest = 0;
    CookUnit rec(default_allocator());
    KILN_CHECK(!copy_input_record(s, "cube_basic.glb"_sv, &rec, &digest));
    close_catalog_store(s);
}

KILN_TEST(CatalogStore, NamedLayoutRefusesACatalogStore) {
    char dir[1024];
    fresh_dir("catalog-then-named", dir, sizeof dir);
    CatalogStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    close_catalog_store(s);
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    KILN_CHECK(bind_store_profile(StrView(dir), kCompatTarget, &sink).failed());
    KILN_CHECK_EQ(lc.code, u32(kDiagStoreProfileMismatch));
}
