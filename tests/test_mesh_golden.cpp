// tests/test_mesh_golden.cpp — mesh cooker golden files (cook-only); see tests/golden/README.md.
// Cooks every `ok` entry of `<--corpus dir>/../gltf/manifest.txt` with default resolved settings
// and compares the bytes with <golden_dir>/mesh/<stem>.mesh. Skips without --golden.
#include "kiln_test.h"
#include "ktx2_corpus.h" // read_file, bytes_equal

#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/mesh.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(KILN_OS_WINDOWS)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif

using namespace kiln;
namespace corpus = kiln::test::corpus;

namespace {

// Manifest and cooking helpers, duplicated from test_mesh_cook.cpp (internal linkage there).

struct GltfEntry {
    StrView path;
    bool expectOk = false;
    u32 code = 0, parts = 0, lods = 0, materials = 0, textures = 0, mounts = 0;
};

bool gltf_dir(char* out, usize cap) {
    char const* dir = kiln::test::corpus_dir();
    if (!dir) return false;
    format(out, cap, "%s/../gltf", dir);
    return true;
}

bool load_gltf_manifest(char const* dir, Vec<char>& text, Vec<GltfEntry>& entries) {
    char path[1024];
    format(path, sizeof path, "%s/manifest.txt", dir);
    Vec<u8> raw(default_allocator(), Tag::Test);
    if (!KILN_CHECK_MSG(corpus::read_file(path, raw), "cannot read %s", path)) return false;
    text.resize(raw.size());
    if (!raw.empty()) std::memcpy(text.data(), raw.data(), raw.size());
    StrView const all(text.data(), text.size());
    usize pos = 0;
    while (pos < all.size) {
        usize eol = all.find('\n', pos);
        if (eol == StrView::kNpos) eol = all.size;
        StrView line = all.substr(pos, eol - pos);
        pos          = eol + 1;
        if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size - 1);
        if (line.empty() || line.front() == '#') continue;
        StrView f[9];
        usize n = 0, p = 0;
        for (; n < 9;) {
            usize bar = line.find('|', p);
            f[n++]    = line.substr(p, (bar == StrView::kNpos ? line.size : bar) - p);
            if (bar == StrView::kNpos) break;
            p = bar + 1;
        }
        if (!KILN_CHECK_MSG(n == 9, "manifest line needs 9 fields: %.*s", KILN_SV(line))) return false;
        GltfEntry e;
        e.path     = f[0];
        e.expectOk = f[1] == "ok";
        bool ok    = corpus::parse_u32(f[2], e.code) && corpus::parse_u32(f[3], e.parts) &&
                  corpus::parse_u32(f[4], e.lods) && corpus::parse_u32(f[5], e.materials) &&
                  corpus::parse_u32(f[6], e.textures) && corpus::parse_u32(f[7], e.mounts);
        if (!KILN_CHECK_MSG(ok, "manifest line has a non-numeric column: %.*s", KILN_SV(line))) return false;
        entries.push_back(e);
    }
    return KILN_CHECK(!entries.empty());
}

cook::MeshCookSettings default_settings() {
    Result<cook::MeshCookSettings> r =
        cook::resolve_mesh(cook::MeshCookSettings{}, cook::TargetProfile{}, cook::CookSession{});
    KILN_VERIFY(r.ok());
    return r.value();
}

/// Resolves external URIs relative to the source file's own directory (the .gltf
/// entry); every .glb entry has no external references and never calls this.
struct DirResolver {
    char dir[1024];
    static Status fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) {
        auto* self = static_cast<DirResolver*>(user);
        char path[2048];
        format(path, sizeof path, "%s/%.*s", self->dir, KILN_SV(uri));
        Vec<u8> bytes(alloc, Tag::Test);
        if (!corpus::read_file(path, bytes)) return make_status(Code::NotFound);
        *out = std::move(bytes);
        return kOk;
    }
};

bool ensure_dir(char const* dir) {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(dir) == 0) return true;
#else
    if (mkdir(dir, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// Human name of the section (or header/gap) containing `offset`, for mismatch diagnosis.
char const* section_name(mesh::MeshView const& v, u64 offset) {
    if (offset < sizeof(mesh::FileHeader)) return "header";
    for (mesh::SectionEntry const& s : v.sections()) {
        if (offset < s.offset || offset >= s.offset + s.size) continue;
        switch (s.id) {
        case mesh::kSecModel: return "MODL (model info)";
        case mesh::kSecStrings: return "STRS (string table)";
        case mesh::kSecLayouts: return "LAYT (vertex layouts)";
        case mesh::kSecParts: return "PART (mesh parts)";
        case mesh::kSecLods: return "LODS (LOD records)";
        case mesh::kSecSubmeshes: return "SUBM (submeshes)";
        case mesh::kSecMaterials: return "MATL (materials)";
        case mesh::kSecTextures: return "MTEX (texture bindings)";
        case mesh::kSecMounts: return "MNTS (mounts)";
        case mesh::kSecBlobs: return "BLOB (blob table)";
        case mesh::kSecGpuData: return "GPUD (vertex/index payload)";
        default: return "unknown section";
        }
    }
    return "section table / header padding";
}

/// Compares `got` against the committed golden `<golden_dir>/mesh/<stem>.mesh`, or
/// (with --update-golden) writes it.
void check_golden_mesh(char const* stem, Span<u8 const> got) {
    char const* golden = kiln::test::golden_dir();
    if (!golden) return;
    char path[1024];
    format(path, sizeof path, "%s/mesh/%s.mesh", golden, stem);

    if (kiln::test::update_golden()) {
        char dir[1024];
        format(dir, sizeof dir, "%s/mesh", golden);
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

    Result<mesh::MeshView> gv = mesh::MeshView::open(got);
    Result<mesh::MeshView> wv = mesh::MeshView::open(want.span());
    char gs[40] = "n/a (open failed)", ws[40] = "n/a (open failed)";
    if (gv.ok() && diffAt < got.size) format(gs, sizeof gs, "%s", section_name(gv.value(), diffAt));
    if (wv.ok() && diffAt < want.size()) format(ws, sizeof ws, "%s", section_name(wv.value(), diffAt));

    KILN_CHECK_MSG(false,
                   "%s: mismatch vs golden (got %zu bytes, golden %zu bytes; first differs at offset "
                   "%zu -- got: %s, golden: %s). If this is a deliberate change, bump kCookerVersion "
                   "(kiln/cook/cook.h) and rerun with --update-golden.",
                   stem, got.size, want.size(), diffAt, gs, ws);
}

} // namespace

KILN_TEST(MeshGolden, Corpus) {
    if (!kiln::test::golden_dir()) return; // needs --golden
    char dir[1024];
    if (!gltf_dir(dir, sizeof dir)) return; // needs --corpus
    Vec<char> text(default_allocator(), Tag::Test);
    Vec<GltfEntry> entries(default_allocator(), Tag::Test);
    if (!load_gltf_manifest(dir, text, entries)) return;

    cook::MeshCookSettings const settings = default_settings();

    for (GltfEntry const& e : entries) {
        if (!e.expectOk) continue;

        char path[1400];
        format(path, sizeof path, "%s/%.*s", dir, KILN_SV(e.path));
        Vec<u8> bytes(default_allocator(), Tag::Test);
        if (!KILN_CHECK_MSG(corpus::read_file(path, bytes), "cannot read %s", path)) continue;

        DirResolver resolver;
        format(resolver.dir, sizeof resolver.dir, "%s", path);
        if (char* slash = std::strrchr(resolver.dir, '/')) *slash = '\0';

        StrView rel       = e.path;
        usize const slash = rel.rfind('/');
        StrView stem      = slash == StrView::kNpos ? rel : rel.substr(slash + 1);
        usize const dot   = stem.rfind('.');
        if (dot != StrView::kNpos) stem = stem.substr(0, dot);
        char stemBuf[128];
        format(stemBuf, sizeof stemBuf, "%.*s", KILN_SV(stem));
        char asset[256];
        format(asset, sizeof asset, "meshes/%s", stemBuf);

        cook::MeshSource src{};
        src.bytes                  = bytes.span();
        src.assetPath              = asset;
        src.sourcePath             = asset;
        src.resolver               = {&DirResolver::fn, &resolver};
        Result<cook::CookedMesh> r = cook::cook_mesh(src, settings, cook::TargetProfile{});
        if (!KILN_CHECK_MSG(r.ok(), "%s: cook failed", stemBuf)) continue;
        check_golden_mesh(stemBuf, r->file.span());
    }
}
