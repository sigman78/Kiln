// tests/test_catalog.cpp — build keys, cook units, the manifest format, store writers, the runtime,
// the provider and kiln-cook on a store (docs/design/store-catalog.md); cook-only.
#include "kiln_test.h"

#include "../src/cook/manifest_store.h"
#include "../src/cook/unit.h"
#include "../src/formats/formats_internal.h"
#include "kiln/cook/cli.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/manifest.h"
#include "kiln/cook/provider.h"
#include "kiln/null_adapter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

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

#if KILN_MESH // these cook glTF sources; a KILN_MESH=OFF build cannot
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

#endif
// ---------------------------------------------------------------------------
// Manifest format 0.1
// ---------------------------------------------------------------------------

namespace {

struct LastCode {
    u32 code = 0;
    static void fn(void* user, Diagnostic const& d) noexcept { static_cast<LastCode*>(user)->code = d.code; }
};

Hash128 key_of(char const* s) { return content_of(s); }

/// Four entries, deliberately out of order; "b.glb" has a mesh and an embedded texture name.
struct SampleEntries {
    ManifestEntry e[4] = {
        {"textures/rock.png", AssetKind::Texture, key_of("1"), key_of("c1"), 100},
        {"b.glb",             AssetKind::Mesh,    key_of("2"), key_of("c2"), 200},
        {"a.glb",             AssetKind::Mesh,    key_of("3"), key_of("c3"), 300},
        {"b.glb#img",         AssetKind::Texture, key_of("4"), key_of("c4"), 400},
    };
};

Vec<u8> write_sample(Span<ManifestEntry const> entries) {
    ManifestProfileDesc const profiles[] = {
        {.name = "compat", .hash = 0x1234, .blockFormats = 0x55, .entries = entries}
    };
    Vec<u8> out(default_allocator(), Tag::Test);
    KILN_CHECK(write_manifest({.profiles = profiles}, &out).ok());
    return out;
}

void reseal(Vec<u8>& b) {
    Hash128 const h = fmt::xxh3_128_zeroed(Span<u8 const>(b.data(), b.size()), kManifestChecksumOffset, 16);
    std::memcpy(b.data() + kManifestChecksumOffset, h.bytes, 16);
}

u32 open_code(Vec<u8> const& b) {
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    return ManifestView::open(Span<u8 const>(b.data(), b.size()), &sink).ok() ? 0 : lc.code;
}

/// The first entry of the one-profile sample: after the header and one profile record.
constexpr usize kEntry0 = kManifestHeaderBytes + kManifestProfileBytes;

} // namespace

KILN_TEST(Manifest, RoundTripWithTwoProfiles) {
    SampleEntries s;
    ManifestEntry const desktop[]        = {s.e[2]};
    ManifestProfileDesc const profiles[] = {
        {.name = "desktop", .hash = 0x99, .blockFormats = 0x77, .entries = desktop},
        {.name = "compat",  .hash = 0x12, .blockFormats = 0x55, .entries = s.e    },
    };
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(write_manifest({.profiles = profiles}, &bytes).ok());
    KILN_CHECK_EQ(bytes.size() % 8, usize(0));
    Result<ManifestView> r = ManifestView::open(Span<u8 const>(bytes.data(), bytes.size()));
    KILN_REQUIRE(r.ok());
    KILN_REQUIRE_EQ(r->profile_count(), 2u);
    KILN_CHECK(r->profile(0).name() == "compat"_sv); // sorted by name
    KILN_CHECK(r->profile(1).name() == "desktop"_sv);

    ManifestProfile c, d, none;
    KILN_REQUIRE(r->find_profile("compat", &c));
    KILN_REQUIRE(r->find_profile("desktop", &d));
    KILN_CHECK(!r->find_profile("mobile", &none));
    KILN_CHECK_EQ(c.hash(), u64(0x12));
    KILN_CHECK_EQ(c.block_formats(), u64(0x55));
    KILN_CHECK_EQ(c.size(), u64(4));
    KILN_CHECK_EQ(d.size(), u64(1));
    // Sorted by name bytes within a profile.
    KILN_CHECK(c.entry(0).name == "a.glb"_sv);
    KILN_CHECK(c.entry(1).name == "b.glb"_sv);
    KILN_CHECK(c.entry(2).name == "b.glb#img"_sv);
    KILN_CHECK(c.entry(3).name == "textures/rock.png"_sv);
    for (ManifestEntry const& want : s.e) {
        ManifestEntry got;
        KILN_REQUIRE(c.find(want.kind, want.name, &got));
        KILN_CHECK(got.name == want.name && got.kind == want.kind);
        KILN_CHECK(got.key == want.key && got.checksum == want.checksum);
        KILN_CHECK_EQ(got.bytes, want.bytes);
    }
    ManifestEntry e;
    KILN_CHECK(d.find(AssetKind::Mesh, "a.glb"_sv, &e));
    KILN_CHECK(!d.find(AssetKind::Mesh, "b.glb"_sv, &e));    // another profile's entry
    KILN_CHECK(!c.find(AssetKind::Texture, "a.glb"_sv, &e)); // right name, wrong kind
    KILN_CHECK(!c.find(AssetKind::Mesh, "c.glb"_sv, &e));
}

KILN_TEST(Manifest, SameContentSameBytes) {
    SampleEntries s;
    ManifestEntry const reversed[] = {s.e[3], s.e[2], s.e[1], s.e[0]};
    ManifestEntry const one[]      = {s.e[1]};
    ManifestProfileDesc const a[]  = {
        {.name = "compat",  .entries = s.e},
        {.name = "desktop", .entries = one}
    };
    ManifestProfileDesc const b[] = {
        {.name = "desktop", .entries = one     },
        {.name = "compat",  .entries = reversed}
    };
    Vec<u8> x(default_allocator(), Tag::Test), y(default_allocator(), Tag::Test);
    KILN_REQUIRE(write_manifest({.profiles = a}, &x).ok());
    KILN_REQUIRE(write_manifest({.profiles = b}, &y).ok());
    KILN_REQUIRE_EQ(x.size(), y.size());
    KILN_CHECK(std::memcmp(x.data(), y.data(), x.size()) == 0);
}

KILN_TEST(Manifest, Empty) {
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(write_manifest({}, &bytes).ok());
    Result<ManifestView> r = ManifestView::open(Span<u8 const>(bytes.data(), bytes.size()));
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->profile_count(), 0u);
    ManifestProfile none;
    KILN_CHECK(!r->find_profile("compat", &none));

    Vec<u8> const one       = write_sample({});
    Result<ManifestView> r1 = ManifestView::open(Span<u8 const>(one.data(), one.size()));
    KILN_REQUIRE(r1.ok());
    ManifestProfile p;
    KILN_REQUIRE(r1->find_profile("compat", &p));
    ManifestEntry e;
    KILN_CHECK_EQ(p.size(), u64(0));
    KILN_CHECK(!p.find(AssetKind::Mesh, "a.glb"_sv, &e));
}

KILN_TEST(Manifest, WriterRejectsDuplicatesAndBadNames) {
    SampleEntries s;
    ManifestEntry const dup[]              = {s.e[0], s.e[1], s.e[0]};
    ManifestProfileDesc const dupEntries[] = {
        {.name = "compat", .entries = dup}
    };
    Vec<u8> out(default_allocator(), Tag::Test);
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    KILN_CHECK(write_manifest({.profiles = dupEntries}, &out, &sink).code == Code::InvalidArgument);
    KILN_CHECK_EQ(lc.code, u32(kDiagManifestDuplicate));
    ManifestProfileDesc const dupProfiles[] = {{.name = "compat"}, {.name = "compat"}};
    KILN_CHECK(write_manifest({.profiles = dupProfiles}, &out, &sink).failed());
    KILN_CHECK_EQ(lc.code, u32(kDiagManifestDuplicate));
    ManifestEntry bad[]                  = {s.e[0]};
    bad[0].name                          = "../up.png";
    ManifestProfileDesc const badEntry[] = {
        {.name = "compat", .entries = bad}
    };
    KILN_CHECK(write_manifest({.profiles = badEntry}, &out, &sink).failed());
    KILN_CHECK_EQ(lc.code, u32(kDiagManifestName));
    ManifestProfileDesc const badProfile[] = {{.name = "Compat"}};
    KILN_CHECK(write_manifest({.profiles = badProfile}, &out, &sink).failed());
}

KILN_TEST(Manifest, ReaderRejectsEachDefect) {
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
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestMagic));
    KILN_CHECK_EQ(open_code(Vec<u8>(default_allocator(), Tag::Test)), u32(kDiagManifestMagic));

    fresh();
    b[6] = 2; // minor
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestVersion));
    {
        LastCode lc;
        DiagSink const sink{&LastCode::fn, &lc};
        KILN_CHECK(ManifestView::open(Span<u8 const>(b.data(), b.size()), &sink).status().code ==
                   Code::VersionMismatch);
    }

    fresh();
    for (int i = 0; i < 8; ++i) // truncated
        b.pop_back();
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestSizes));
    fresh();
    write_unaligned<u64>(b.data() + 32, u64(1) << 40); // entry count
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestSizes));
    fresh();
    write_unaligned<u64>(b.data() + kManifestHeaderBytes + 32, u64(3)); // the profile's entry count
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestSizes));

    // Sections past the end: the offsets alone must not reach outside the file.
    for (int which = 0; which < 2; ++which) {
        b.clear();
        b.append(Span<u8 const>(good.data(), kManifestHeaderBytes));
        for (int i = 0; i < 8; ++i)
            b.push_back(0);
        write_unaligned<u64>(b.data() + 16, u64(b.size()));
        write_unaligned<u32>(b.data() + 24, 0u);
        write_unaligned<u64>(b.data() + 32, u64(which == 0 ? 1 : 0)); // entries past the end
        for (u64 at = 40; at <= 64; at += 8)
            write_unaligned<u64>(b.data() + at, u64(kManifestHeaderBytes));
        write_unaligned<u64>(b.data() + 72, which == 1 ? ~u64(0) - 63 : u64(0)); // strings past the end
        KILN_CHECK_EQ(open_code(b), u32(kDiagManifestSizes));
    }

    fresh();
    b[120] = 1; // reserved
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestReserved));

    // Entry 0 ("a.glb") gets kind 3.
    fresh();
    write_unaligned<u16>(b.data() + kEntry0 + 8, u16(3));
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestName));

    // Swap entries 0 and 1: out of order.
    fresh();
    u8 tmp[kManifestEntryBytes];
    u8* e0 = b.data() + kEntry0;
    std::memcpy(tmp, e0, kManifestEntryBytes);
    std::memcpy(e0, e0 + kManifestEntryBytes, kManifestEntryBytes);
    std::memcpy(e0 + kManifestEntryBytes, tmp, kManifestEntryBytes);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestOrder));

    // Entry 2 ("b.glb#img", texture) renamed to entry 1's name ("b.glb") and made a mesh.
    fresh();
    u8* e1 = b.data() + kEntry0 + kManifestEntryBytes;
    std::memcpy(e1 + kManifestEntryBytes, e1, 12);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestDuplicate));

    // An index record pointing at the wrong entry.
    fresh();
    u64 const index = read_unaligned<u64>(b.data() + 56);
    u64 const e     = read_unaligned<u64>(b.data() + index + 8);
    write_unaligned<u64>(b.data() + index + 8, (e + 1) % 4);
    reseal(b);
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestIndex));

    fresh();
    b[kEntry0 + 20] ^= 1; // a key byte
    KILN_CHECK_EQ(open_code(b), u32(kDiagManifestChecksum));
}

KILN_TEST(Manifest, Paths) {
    char out[256];
    KILN_CHECK_EQ(manifest_file_path("store"_sv, out, sizeof out), usize(18));
    KILN_CHECK(std::strcmp(out, "store/manifest.dir") == 0);
    Hash128 k;
    for (u8 i = 0; i < 16; ++i)
        k.bytes[i] = u8(0xa0 + i);
    (void)artifact_file_path("store"_sv, k, out, sizeof out);
    KILN_CHECK(std::strcmp(out, "store/ucq2fi5euwtkpkfjvkv2zlnov4") == 0);
    KILN_CHECK(check_profile_name("compat"_sv) == nullptr);
    KILN_CHECK(check_profile_name("my-profile_2"_sv) == nullptr);
    KILN_CHECK(check_profile_name(""_sv) != nullptr);
    KILN_CHECK(check_profile_name("Compat"_sv) != nullptr);
    KILN_CHECK(check_profile_name("a/b"_sv) != nullptr);
}

#if KILN_MESH // these cook glTF sources; a KILN_MESH=OFF build cannot
// ---------------------------------------------------------------------------
// Store writers: artifacts, the lock, the manifest and the input records
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

Status open_store(char const* dir, ManifestStore** out, DiagSink const* diag = nullptr) {
    return open_manifest_store({.storeDir = StrView(dir), .target = &kCompatTarget, .diag = diag}, out);
}

/// Profile `profile` of the store's manifest, read into `bytes`; false when there is none.
bool read_profile(char const* dir, StrView profile, Vec<u8>& bytes, ManifestProfile* out) {
    char path[1024];
    (void)manifest_file_path(StrView(dir), path, sizeof path);
    bytes.clear();
    if (!io_read_file(compat_io_backend(), StrView(path), default_allocator(), &bytes).ok()) return false;
    Result<ManifestView> v = ManifestView::open(bytes.span());
    return v.ok() && v->find_profile(profile, out);
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

    ManifestStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 7, nullptr).ok());
    KILN_REQUIRE(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);

    // The manifest on disk names every output, and each artifact holds its bytes.
    char path[1024];
    Vec<u8> bytes(default_allocator(), Tag::Test);
    ManifestProfile v;
    KILN_REQUIRE(read_profile(dir, "compat", bytes, &v));
    KILN_CHECK_EQ(v.size(), u64(unit.outputs.size()));
    KILN_CHECK_EQ(v.hash(), hash_target(kCompatTarget));
    for (UnitOutput const& o : unit.outputs) {
        ManifestEntry e;
        KILN_REQUIRE(v.find(o.kind, unit.name(o), &e));
        KILN_CHECK(e.key == o.key);
        KILN_CHECK_EQ(e.bytes, u64(o.bytes.size()));
        (void)artifact_file_path(StrView(dir), o.key, path, sizeof path);
        Vec<u8> artifact(default_allocator(), Tag::Test);
        KILN_REQUIRE(read_file(path, artifact));
        KILN_CHECK(xxh3_128(artifact.span()) == e.checksum);
    }

    // A new writer sees the entries and the input record.
    KILN_REQUIRE(open_store(dir, &s).ok());
    Hash128 key;
    KILN_CHECK(manifest_find(s, AssetKind::Mesh, "pbr_textures.glb"_sv, &key) && key == unit.outputs[0].key);
    CookUnit rec(default_allocator());
    u64 digest = 0;
    KILN_REQUIRE(copy_input_record(s, "pbr_textures.glb"_sv, &rec, &digest));
    KILN_CHECK_EQ(digest, u64(7));
    KILN_REQUIRE_EQ(rec.inputs.size(), unit.inputs.size());
    for (usize i = 0; i < rec.inputs.size(); ++i) {
        KILN_CHECK(rec.inputs[i].role == unit.inputs[i].role);
        KILN_CHECK(rec.inputs[i].content == unit.inputs[i].content);
        KILN_CHECK_EQ(rec.inputs[i].stat.mtimeNs, unit.inputs[i].stat.mtimeNs);
        KILN_CHECK_EQ(rec.inputs[i].pathLen, 0u); // records keep no paths
    }
    KILN_REQUIRE_EQ(rec.outputs.size(), unit.outputs.size());
    for (usize i = 0; i < rec.outputs.size(); ++i) {
        KILN_CHECK(rec.name(rec.outputs[i]) == unit.name(unit.outputs[i]));
        KILN_CHECK(rec.outputs[i].key == unit.outputs[i].key);
        KILN_CHECK(rec.outputs[i].slot == unit.outputs[i].slot);
    }
    set_record_digest(s, "pbr_textures.glb"_sv, 8);
    KILN_REQUIRE(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(copy_input_record(s, "pbr_textures.glb"_sv, &rec, &digest));
    KILN_CHECK_EQ(digest, u64(8));
    close_manifest_store(s);
}

KILN_TEST(CatalogStore, OneWriterAtATime) {
    char dir[1024];
    fresh_dir("catalog-lock", dir, sizeof dir);
    ManifestStore* a = nullptr;
    ManifestStore* b = nullptr;
    KILN_REQUIRE(open_store(dir, &a).ok());
    LastCode lc;
    DiagSink const sink{&LastCode::fn, &lc};
    Status const st = open_store(dir, &b, &sink);
    KILN_CHECK(st.code == Code::Busy);
    KILN_CHECK_EQ(lc.code, u32(kDiagStoreLocked));
    KILN_CHECK(b == nullptr);
    close_manifest_store(a);
    KILN_REQUIRE(open_store(dir, &b).ok());
    close_manifest_store(b);
}

KILN_TEST(CatalogStore, OtherBytesForAKeyAreReported) {
    char dir[1024];
    fresh_dir("catalog-nondet", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &unit).ok());
    ManifestStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 0, nullptr).ok());

    // Same key, other bytes: the artifact keeps its bytes and the publish fails.
    char path[1024];
    (void)artifact_file_path(StrView(dir), unit.outputs[0].key, path, sizeof path);
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
    close_manifest_store(s);
}

KILN_TEST(CatalogStore, OutputsTheCookNoLongerMakesLeave) {
    char dir[1024];
    fresh_dir("catalog-drop", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("pbr_textures.glb", AssetKind::Mesh, &unit).ok());
    KILN_REQUIRE(unit.outputs.size() > usize(2));
    ManifestStore* s = nullptr;
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
    KILN_CHECK(manifest_find(s, AssetKind::Mesh, "pbr_textures.glb"_sv, &key));
    KILN_CHECK(!manifest_find(s, AssetKind::Texture, lastName, &key));
    KILN_CHECK(!manifest_find(s, AssetKind::Texture, firstName, &key));
    KILN_REQUIRE(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);

    Vec<u8> bytes(default_allocator(), Tag::Test);
    ManifestProfile v;
    KILN_REQUIRE(read_profile(dir, "compat", bytes, &v));
    KILN_CHECK_EQ(v.size(), u64(unit.outputs.size() - 2));
}

KILN_TEST(CatalogStore, SurvivesLostRecords) {
    char dir[1024];
    ManifestStore* s = nullptr;
    fresh_dir("catalog-lost", dir, sizeof dir);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_corpus("cube_basic.glb", AssetKind::Mesh, &unit).ok());
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_REQUIRE(publish_unit(s, unit, 0, nullptr).ok());
    KILN_REQUIRE(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);

    // A damaged input-record file is dropped; the manifest stays.
    char path[1024];
    format(path, sizeof path, "%s/%s", dir, kInputRecordsFile);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_file(path, bytes));
    bytes[bytes.size() / 2] ^= 1;
    KILN_REQUIRE(store_write(StrView(dir), StrView(kInputRecordsFile), bytes.span(), nullptr, true).ok());
    KILN_REQUIRE(open_store(dir, &s).ok());
    Hash128 key;
    KILN_CHECK(manifest_find(s, AssetKind::Mesh, "cube_basic.glb"_sv, &key));
    u64 digest = 0;
    CookUnit rec(default_allocator());
    KILN_CHECK(!copy_input_record(s, "cube_basic.glb"_sv, &rec, &digest));
    close_manifest_store(s);
}

// ---------------------------------------------------------------------------
// The runtime on a store
// ---------------------------------------------------------------------------

namespace {

struct FirstCode {
    u32 code = 0;
    static void fn(void* user, Diagnostic const& d) noexcept {
        auto* self = static_cast<FirstCode*>(user);
        if (self->code == 0 && d.severity == Severity::Error) self->code = d.code;
    }
};

/// A null adapter and a context over the store `dir` (profile `profile`).
struct CatalogContext {
    Adapter adapter{};
    NullAdapter* na = nullptr;
    Context* ctx    = nullptr;
    FirstCode diag;

    CatalogContext(CatalogContext const&)            = delete;
    CatalogContext& operator=(CatalogContext const&) = delete;
    CatalogContext() noexcept                        = default;

    Status init(char const* dir, bool hotReload = false, StrView profile = "compat") noexcept {
        Result<NullAdapter*> n = null_adapter_create({}, &adapter);
        if (n.failed()) return n.status();
        na = *n;
        ContextDesc desc{};
        desc.adapter       = &adapter;
        desc.storeDir      = StrView(dir);
        desc.profile       = profile;
        desc.diag          = {&FirstCode::fn, &diag};
        desc.hotReload     = {.watchStore = hotReload, .pollMs = 5};
        Result<Context*> c = create(desc);
        if (c.failed()) return c.status();
        ctx = *c;
        return kOk;
    }
    ~CatalogContext() noexcept {
        if (ctx) destroy(ctx);
        if (na) null_adapter_destroy(na);
    }
};

template <class H> State settle(Context* ctx, H h) {
    for (int i = 0; i < 2000; ++i) {
        (void)pump(ctx, {});
        State const s = state(ctx, h);
        if (s == State::Ready || s == State::Failed) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return state(ctx, h);
}

/// Cooks the corpus file `source` as the asset `name` and publishes it into the store `dir`.
void publish(char const* dir, char const* source, char const* name, CookUnit* out = nullptr) {
    static MeshCookSettings const mesh;
    static TextureCookSettings const tex;
    char path[1024];
    format(path, sizeof path, "%s/../gltf/generated/%s", kiln::test::corpus_dir(), source);
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_unit({.kind            = AssetKind::Mesh,
                            .name            = StrView(name),
                            .sourcePath      = StrView(path),
                            .meshDefaults    = &mesh,
                            .textureDefaults = &tex,
                            .target          = &kCompatTarget},
                           &unit)
                     .ok());
    ManifestStore* s = nullptr;
    KILN_REQUIRE(open_store(dir, &s).ok());
    KILN_CHECK(publish_unit(s, unit, 0, nullptr).ok());
    KILN_CHECK(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);
    if (out) *out = std::move(unit);
}

} // namespace

KILN_TEST(CatalogRuntime, LoadsArtifactsByName) {
    char dir[1024];
    fresh_dir("catalog-rt", dir, sizeof dir);
    CookUnit unit(default_allocator());
    publish(dir, "pbr_textures.glb", "pbr_textures.glb", &unit);
    KILN_REQUIRE(unit.outputs.size() > usize(1));

    CatalogContext c;
    KILN_REQUIRE(c.init(dir).ok());
    KILN_CHECK(store_profile(c.ctx) == "compat"_sv);
    MeshHandle const m = request_mesh(c.ctx, "pbr_textures.glb"_sv);
    KILN_CHECK(settle(c.ctx, m) == State::Ready);
    TextureHandle const t = request_texture(c.ctx, unit.name(unit.outputs[1]));
    KILN_CHECK(settle(c.ctx, t) == State::Ready);

    // A name the profile lacks, with no provider: a store miss.
    MeshHandle const missing = request_mesh(c.ctx, "nothing.glb"_sv);
    KILN_CHECK(settle(c.ctx, missing) == State::Failed);
    KILN_CHECK_EQ(c.diag.code, u32(kDiagStoreMiss));

    // An entry whose artifact is gone: a store miss too.
    char path[1024];
    (void)artifact_file_path(StrView(dir), unit.outputs.back().key, path, sizeof path);
    KILN_REQUIRE(std::remove(path) == 0);
    c.diag.code              = 0;
    TextureHandle const gone = request_texture(c.ctx, unit.name(unit.outputs.back()));
    KILN_CHECK(settle(c.ctx, gone) == State::Failed);
    KILN_CHECK_EQ(c.diag.code, u32(kDiagStoreMiss));
}

KILN_TEST(CatalogRuntime, MissingAndBrokenManifests) {
    char dir[1024];
    fresh_dir("catalog-rt-none", dir, sizeof dir);
    {
        CatalogContext c;
        KILN_REQUIRE(c.init(dir).ok()); // no manifest yet: a provider may write one
        MeshHandle const m = request_mesh(c.ctx, "a.glb"_sv);
        KILN_CHECK(settle(c.ctx, m) == State::Failed);
        KILN_CHECK_EQ(c.diag.code, u32(kDiagCatalogMissing));
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char path[1024];
    (void)manifest_file_path(StrView(dir), path, sizeof path);
    std::FILE* f = std::fopen(path, "wb");
    KILN_REQUIRE(f != nullptr);
    std::fputs("not a manifest", f);
    std::fclose(f);
    CatalogContext c;
    KILN_CHECK(c.init(dir).code == Code::Corrupt);
    KILN_CHECK_EQ(c.diag.code, u32(kDiagManifestMagic));
}

KILN_TEST(CatalogRuntime, HotReloadFollowsTheCatalog) {
    char dir[1024];
    fresh_dir("catalog-rt-reload", dir, sizeof dir);
    publish(dir, "cube_basic.glb", "model.glb");

    CatalogContext c;
    KILN_REQUIRE(c.init(dir, true).ok());
    MeshHandle const m = request_mesh(c.ctx, "model.glb"_sv);
    KILN_REQUIRE(settle(c.ctx, m) == State::Ready);
    u64 const gpuBytes     = mesh_view(c.ctx, m)->header().gpuDataSize;
    MeshHandle const later = request_mesh(c.ctx, "later.glb"_sv);
    KILN_REQUIRE(settle(c.ctx, later) == State::Failed);

    // Another cook under the same name, and a new name: the manifest changes on disk.
    publish(dir, "authored_lods.glb", "model.glb");
    publish(dir, "cube_basic.glb", "later.glb");
    bool changed = false;
    for (int i = 0; i < 3000 && !(changed && state(c.ctx, later) == State::Ready); ++i) {
        (void)pump(c.ctx, {});
        for (Event const& e : events(c.ctx))
            if (e.kind == EventKind::Changed && e.handle == m.bits()) changed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    KILN_CHECK(changed);
    KILN_CHECK_EQ(version(c.ctx, m), u32(2));
    KILN_CHECK(mesh_view(c.ctx, m)->header().gpuDataSize != gpuBytes);
    KILN_CHECK(state(c.ctx, later) == State::Ready); // failed before, in the manifest now
}

// ---------------------------------------------------------------------------
// The cook provider on a store
// ---------------------------------------------------------------------------

namespace {

/// Counts the mesh resolutions: a cook or a key check calls the policy once per mesh.
struct PolicyCount {
    std::atomic<u32> meshes{0};
    static Status mesh(void* user, CookAssetInfo const&, TargetProfile const&, MeshCookSettings*,
                       DiagSink const*) noexcept {
        ++static_cast<PolicyCount*>(user)->meshes;
        return kOk;
    }
    CookPolicy policy() noexcept { return {.mesh = &mesh, .user = this}; }
};

/// A context with a provider over the root `sources`.
struct ProviderContext {
    CatalogContext c;
    Root root{};

    Status init(char const* store, char const* sources, cook::ProviderDesc desc, bool hotReload = false) {
        root                   = Root{{}, StrView(sources)};
        Result<NullAdapter*> n = null_adapter_create({}, &c.adapter);
        if (n.failed()) return n.status();
        c.na = *n;
        ContextDesc cd{};
        cd.adapter           = &c.adapter;
        cd.storeDir          = StrView(store);
        cd.roots             = Span<Root const>(&root, 1);
        cd.diag              = {&FirstCode::fn, &c.diag};
        cd.hotReload         = {.watchStore = hotReload, .pollMs = 5};
        Result<Context*> ctx = create(cd);
        if (ctx.failed()) return ctx.status();
        c.ctx = *ctx;
        return cook::install_provider(c.ctx, desc);
    }
};

/// The key of `name` in profile `profile` of the store's manifest on disk; zero if absent.
Hash128 catalog_key(char const* store, AssetKind kind, StrView name, StrView profile = "compat") {
    char path[1024];
    (void)manifest_file_path(StrView(store), path, sizeof path);
    Vec<u8> bytes(default_allocator(), Tag::Test);
    // On Windows an open can fail while a writer renames a new manifest over the file: try again.
    bool read = false;
    for (int i = 0; i < 50 && !read; ++i) {
        bytes.clear();
        read = read_file(path, bytes);
        if (!read) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!read) return {};
    Result<ManifestView> v = ManifestView::open(bytes.span());
    ManifestProfile p;
    ManifestEntry e;
    return v.ok() && v->find_profile(profile, &p) && p.find(kind, name, &e) ? e.key : Hash128{};
}

/// Copies the external_uri glTF, its buffer and image into `dir`.
void copy_sources(char const* dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char from[1024];
    format(from, sizeof from, "%s/../gltf/generated", kiln::test::corpus_dir());
    for (char const* f : {"external_uri.gltf", "external_uri.bin", "external_uri_albedo.png"})
        std::filesystem::copy_file(std::filesystem::path(from) / f, std::filesystem::path(dir) / f,
                                   std::filesystem::copy_options::overwrite_existing, ec);
}

/// Flips the first byte of `path`, keeping its size; with `keepTime`, also its modification time.
void edit_first_byte(char const* path, bool keepTime, int bit = 1) {
    std::error_code ec;
    auto const time = std::filesystem::last_write_time(path, ec);
    std::FILE* f    = std::fopen(path, "r+b");
    KILN_REQUIRE(f != nullptr);
    int const c = std::fgetc(f);
    std::fseek(f, 0, SEEK_SET);
    std::fputc(c ^ bit, f);
    std::fclose(f);
    if (keepTime)
        std::filesystem::last_write_time(path, time, ec);
    else
        std::filesystem::last_write_time(path, time + std::chrono::seconds(2), ec);
}

} // namespace

KILN_TEST(CatalogProvider, ChecksOncePerSessionAndCooksOnlyWhatChanged) {
    char store[1024], sources[1024], bin[1100];
    fresh_dir("catalog-prov-store", store, sizeof store);
    fresh_dir("catalog-prov-src", sources, sizeof sources);
    copy_sources(sources);
    format(bin, sizeof bin, "%s/external_uri.bin", sources);
    PolicyCount count;
    cook::ProviderDesc desc{.policy = count.policy()};

    // Session 1: a miss cooks and publishes.
    {
        ProviderContext p;
        KILN_REQUIRE(p.init(store, sources, desc).ok());
        MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
        KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);
        KILN_CHECK(count.meshes.load() > 0u);
    }
    Hash128 const k1 = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    KILN_REQUIRE(!k1.is_zero());

    // Session 2: nothing changed; the size-and-time check passes without a cook.
    count.meshes = 0;
    {
        ProviderContext p;
        KILN_REQUIRE(p.init(store, sources, desc).ok());
        MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
        KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);
        KILN_CHECK_EQ(count.meshes.load(), 0u);
    }

    // Session 3: the buffer changes but keeps its size and time: not seen (the accepted cost). A new
    // policy version only re-checks the keys from the recorded hashes.
    edit_first_byte(bin, true);
    desc.policyVersion = 1;
    {
        ProviderContext p;
        KILN_REQUIRE(p.init(store, sources, desc).ok());
        MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
        KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);
        KILN_CHECK_EQ(count.meshes.load(), 1u); // one resolution, no cook
    }
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == k1);

    // Session 4: the buffer's time moves: a re-cook with the new content.
    edit_first_byte(bin, false, 2);
    count.meshes = 0;
    {
        ProviderContext p;
        KILN_REQUIRE(p.init(store, sources, desc).ok());
        MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
        KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);
        KILN_CHECK(count.meshes.load() > 0u);
    }
    Hash128 const k4 = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    KILN_CHECK(!k4.is_zero() && !(k4 == k1));
}

KILN_TEST(CatalogProvider, ProfileAndLock) {
    char store[1024], sources[1024];
    fresh_dir("catalog-prov-lock", store, sizeof store);
    fresh_dir("catalog-prov-lock-src", sources, sizeof sources);
    copy_sources(sources);

    ProviderContext wrongProfile;
    KILN_CHECK(wrongProfile.init(store, sources, {.target = cook::kDesktopTarget}).code ==
               Code::InvalidArgument);
    KILN_CHECK_EQ(wrongProfile.c.diag.code, u32(kDiagStoreProfileMismatch));

    ProviderContext first;
    KILN_REQUIRE(first.init(store, sources, {}).ok());
    ProviderContext second;
    KILN_CHECK(second.init(store, sources, {}).code == Code::Busy);
    KILN_CHECK_EQ(second.c.diag.code, u32(kDiagStoreLocked));

    // Memory mode writes nothing and takes no lock.
    ProviderContext memory;
    KILN_REQUIRE(memory.init(store, sources, {.storeMode = cook::StoreMode::Memory}).ok());
    MeshHandle const m = request_mesh(memory.c.ctx, "external_uri.gltf"_sv);
    KILN_CHECK(settle(memory.c.ctx, m) == State::Ready);
}

KILN_TEST(CatalogProvider, SourceEditsReachLoadedAssets) {
    char store[1024], sources[1024], bin[1100];
    fresh_dir("catalog-prov-hot", store, sizeof store);
    fresh_dir("catalog-prov-hot-src", sources, sizeof sources);
    copy_sources(sources);
    format(bin, sizeof bin, "%s/external_uri.bin", sources);

    ProviderContext p;
    KILN_REQUIRE(p.init(store, sources, {.watchSources = true, .pollMs = 5}, true).ok());
    MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
    KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);

    // Nothing changed yet: the manifest appearing on disk must not reload the asset.
    u32 changes         = 0;
    auto const pump_for = [&](int ms) {
        for (int i = 0; i < ms; ++i) {
            (void)pump(p.c.ctx, {});
            for (Event const& e : events(p.c.ctx))
                if (e.kind == EventKind::Changed && e.handle == m.bits()) ++changes;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    pump_for(600);
    KILN_CHECK_EQ(changes, 0u);
    Hash128 const before = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    KILN_REQUIRE(!before.is_zero());

    edit_first_byte(bin, false);
    for (int i = 0; i < 50 && changes == 0; ++i)
        pump_for(100);
    KILN_CHECK_EQ(changes, 1u);
    KILN_CHECK_EQ(version(p.c.ctx, m), u32(2));
    Hash128 const after = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    char hb[33], ha[33];
    hash128_hex(before, hb);
    hash128_hex(after, ha);
    KILN_CHECK_MSG(!after.is_zero() && !(after == before), "before %s, after %s", hb, ha);
}

// ---------------------------------------------------------------------------
// kiln-cook on a store
// ---------------------------------------------------------------------------

namespace {

/// Runs kiln-cook on `sources` into `store`, with one extra argument or none.
int run_cook(char* sources, char* store, char const* extra = nullptr, CookPolicy const& policy = {}) {
    char arg0[] = "kiln-cook", argO[] = "-o", argQ[] = "-q";
    char extraArg[64] = {};
    format(extraArg, sizeof extraArg, "%s", extra ? extra : "");
    char* argv[] = {arg0, sources, argO, store, argQ, extraArg};
    return cook::cook_cli_main(extra ? 6 : 5, argv, policy);
}

/// The recorded modification time of the input `role` of the unit `name`, as the store keeps it.
u64 recorded_mtime(char const* store, StrView name, InputRole role) {
    ManifestStore* s = nullptr;
    if (open_store(store, &s).failed()) return 0;
    CookUnit rec(default_allocator());
    u64 digest = 0, mtime = 0;
    if (copy_input_record(s, name, &rec, &digest))
        for (UnitInput const& in : rec.inputs)
            if (in.role == role) mtime = in.stat.mtimeNs;
    close_manifest_store(s);
    return mtime;
}

} // namespace

KILN_TEST(CatalogCli, CooksOnlyWhatChangedAndVerifies) {
    char store[1024], sources[1024], bin[1100];
    fresh_dir("catalog-cli-store", store, sizeof store);
    fresh_dir("catalog-cli-src", sources, sizeof sources);
    copy_sources(sources);
    format(bin, sizeof bin, "%s/external_uri.bin", sources);

    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    Hash128 const mesh = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    Hash128 const tex  = catalog_key(store, AssetKind::Texture, "external_uri_albedo.png"_sv);
    KILN_REQUIRE(!mesh.is_zero() && !tex.is_zero());
    char path[1024];
    (void)artifact_file_path(StrView(store), mesh, path, sizeof path);
    KILN_CHECK(io_file_exists(StrView(path)));

    // Unchanged sources: nothing cooks. A buffer edited in place (same size and time) is seen
    // only with --verify.
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    edit_first_byte(bin, true);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == mesh);
    KILN_REQUIRE_EQ(run_cook(sources, store, "--verify"), 0);
    KILN_CHECK(!(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == mesh));
    KILN_CHECK(catalog_key(store, AssetKind::Texture, "external_uri_albedo.png"_sv) == tex);

    // A provider uses what kiln-cook wrote without cooking: an edit it cannot see stays unseen.
    Hash128 const verified = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    edit_first_byte(bin, true, 2);
    ProviderContext p;
    KILN_REQUIRE(p.init(store, sources, {}).ok());
    MeshHandle const m = request_mesh(p.c.ctx, "external_uri.gltf"_sv);
    KILN_REQUIRE(settle(p.c.ctx, m) == State::Ready);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == verified);
}

KILN_TEST(CatalogCli, LostManifestOrArtifactCooksAgain) {
    char store[1024], sources[1024], path[1100];
    fresh_dir("catalog-cli-lost", store, sizeof store);
    fresh_dir("catalog-cli-lost-src", sources, sizeof sources);
    copy_sources(sources);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    Hash128 const mesh = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    KILN_REQUIRE(!mesh.is_zero());

    // The input records survive a deleted manifest; they must not make the sources look done.
    (void)manifest_file_path(StrView(store), path, sizeof path);
    KILN_REQUIRE(std::remove(path) == 0);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == mesh);

    (void)artifact_file_path(StrView(store), mesh, path, sizeof path);
    KILN_REQUIRE(std::remove(path) == 0);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    KILN_CHECK(io_file_exists(StrView(path)));
}

// kiln-cook --watch next to a read-only app: edits and new sources reach the manifest it watches.
KILN_TEST(CatalogCli, WatchCooksEditsAndNewSources) {
    char store[1024], sources[1024], bin[1100], png[1100];
    fresh_dir("catalog-cli-watch", store, sizeof store);
    fresh_dir("catalog-cli-watch-src", sources, sizeof sources);
    copy_sources(sources);
    format(bin, sizeof bin, "%s/external_uri.bin", sources);
    format(png, sizeof png, "%s/external_uri_albedo.png", sources);

    int code = -1;
    std::thread cook([&] {
        char arg0[] = "kiln-cook", argO[] = "-o", argQ[] = "-q", argW[] = "--watch", argT[] = "--timeout",
             argS[]  = "4";
        char* argv[] = {arg0, sources, argO, store, argQ, argW, argT, argS};
        code         = cook::cook_cli_main(8, argv, {});
    });
    Hash128 first;
    for (int i = 0; i < 3000 && first.is_zero(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        first = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    }
    edit_first_byte(bin, false);
    char copy[1100];
    format(copy, sizeof copy, "%s/second.png", sources);
    std::error_code ec;
    std::filesystem::copy_file(png, copy, ec);
    cook.join();

    KILN_CHECK_EQ(code, 0);
    KILN_REQUIRE(!first.is_zero());
    Hash128 const after = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);
    KILN_CHECK(!after.is_zero() && !(after == first));
    KILN_CHECK(!catalog_key(store, AssetKind::Texture, "second.png"_sv).is_zero());
}

// Two profiles in one store: each writer edits its own and keeps the other's entries and records;
// each context reads its own profile; a profile cooked for another definition of it goes alone.
KILN_TEST(CatalogCli, TwoProfilesShareOneStore) {
    char store[1024], sources[1024];
    fresh_dir("catalog-two-profiles", store, sizeof store);
    fresh_dir("catalog-two-profiles-src", sources, sizeof sources);
    copy_sources(sources);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    KILN_REQUIRE_EQ(run_cook(sources, store, "--target=desktop"), 0);
    Hash128 const compat  = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv, "compat");
    Hash128 const desktop = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv, "desktop");
    KILN_REQUIRE(!compat.is_zero() && !desktop.is_zero());
    KILN_CHECK(!(compat == desktop)); // the target is part of the key

    // Neither run cooks again: the other writer kept each profile's records.
    PolicyCount count;
    KILN_REQUIRE_EQ(run_cook(sources, store, nullptr, count.policy()), 0);
    KILN_REQUIRE_EQ(run_cook(sources, store, "--target=desktop", count.policy()), 0);
    KILN_CHECK_EQ(count.meshes.load(), 0u);

    for (char const* profile : {"compat", "desktop"}) {
        CatalogContext c;
        KILN_REQUIRE(c.init(store, false, StrView(profile)).ok());
        MeshHandle const m = request_mesh(c.ctx, "external_uri.gltf"_sv);
        KILN_CHECK_MSG(settle(c.ctx, m) == State::Ready, "profile %s", profile);
    }
    {
        CatalogContext c;
        KILN_REQUIRE(c.init(store, false, "mobile").ok()); // no such profile: like no manifest
        MeshHandle const m = request_mesh(c.ctx, "external_uri.gltf"_sv);
        KILN_CHECK(settle(c.ctx, m) == State::Failed);
        KILN_CHECK_EQ(c.diag.code, u32(kDiagCatalogMissing));
    }

    // Another definition of `compat` (another hash_target) drops compat's entries only.
    TargetProfile other  = kCompatTarget;
    other.maxTextureSize = 1024;
    ManifestStore* s     = nullptr;
    KILN_REQUIRE(open_manifest_store({.storeDir = StrView(store), .target = &other}, &s).ok());
    KILN_REQUIRE(commit_manifest(s, nullptr).ok());
    close_manifest_store(s);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv, "compat").is_zero());
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv, "desktop") == desktop);
    KILN_REQUIRE_EQ(run_cook(sources, store, "--target=desktop", count.policy()), 0);
    KILN_CHECK_EQ(count.meshes.load(), 0u); // desktop's records survived
}

// Records keep no paths: the same sources at another place, with their times, cook nothing.
KILN_TEST(CatalogCli, RecordsSurviveMovedSources) {
    char store[1024], a[1024], b[1024];
    fresh_dir("catalog-moved", store, sizeof store);
    fresh_dir("catalog-moved-a", a, sizeof a);
    fresh_dir("catalog-moved-b", b, sizeof b);
    copy_sources(a);
    KILN_REQUIRE_EQ(run_cook(a, store), 0);
    Hash128 const key = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);

    std::error_code ec;
    std::filesystem::create_directories(b, ec);
    for (auto const& f : std::filesystem::directory_iterator(a, ec)) {
        std::filesystem::path const to = std::filesystem::path(b) / f.path().filename();
        std::filesystem::copy_file(f.path(), to, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::last_write_time(to, std::filesystem::last_write_time(f.path(), ec), ec);
    }
    std::filesystem::remove_all(a, ec);
    PolicyCount count;
    KILN_REQUIRE_EQ(run_cook(b, store, nullptr, count.policy()), 0);
    KILN_CHECK_EQ(count.meshes.load(), 0u);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == key);
}

// A new time on unchanged content is no change: the file is hashed, its record takes the new stat,
// and nothing cooks. A content change cooks.
KILN_TEST(CatalogCli, HashBeforeCook) {
    char store[1024], sources[1024], bin[1100];
    fresh_dir("catalog-touch", store, sizeof store);
    fresh_dir("catalog-touch-src", sources, sizeof sources);
    copy_sources(sources);
    format(bin, sizeof bin, "%s/external_uri.bin", sources);
    KILN_REQUIRE_EQ(run_cook(sources, store), 0);
    Hash128 const key = catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv);

    std::error_code ec;
    std::filesystem::last_write_time(bin, std::filesystem::last_write_time(bin, ec) + std::chrono::seconds(3),
                                     ec);
    IoStat touched;
    KILN_REQUIRE(stat_file(StrView(bin), &touched).ok());
    KILN_CHECK(recorded_mtime(store, "external_uri.gltf"_sv, InputRole::Buffer) != touched.mtimeNs);
    PolicyCount count;
    KILN_REQUIRE_EQ(run_cook(sources, store, nullptr, count.policy()), 0);
    KILN_CHECK_EQ(count.meshes.load(), 0u);
    KILN_CHECK(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == key);
    KILN_CHECK_EQ(recorded_mtime(store, "external_uri.gltf"_sv, InputRole::Buffer), touched.mtimeNs);

    edit_first_byte(bin, false);
    KILN_REQUIRE_EQ(run_cook(sources, store, nullptr, count.policy()), 0);
    KILN_CHECK(count.meshes.load() > 0u);
    KILN_CHECK(!(catalog_key(store, AssetKind::Mesh, "external_uri.gltf"_sv) == key));
}

#endif // KILN_MESH
