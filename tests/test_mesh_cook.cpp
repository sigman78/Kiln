// tests/test_mesh_cook.cpp — cook_mesh (glTF/GLB -> .mesh) tests; cook-only. An in-test GLB quad, and
// every entry of `<corpus_dir>/../gltf/manifest.txt` against the manifest plus per-file checks.
// Cooked files go to `<sample_dir()>/cooked_<stem>.mesh`.
#include "kiln_test.h"
#include "ktx2_corpus.h" // read_file, parse_u32

#include "kiln/assets.h"
#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/io.h"
#include "kiln/mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kiln;
namespace corpus = kiln::test::corpus;

namespace {

struct Diags {
    struct Item {
        u32 code;
        Severity sev;
    };
    Item items[256];
    int count     = 0;
    u32 firstErr  = 0;
    char msg[512] = {};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<Diags*>(user);
        if (self->count < 256) self->items[self->count] = {d.code, d.severity};
        ++self->count;
        if (d.severity == Severity::Error && self->firstErr == 0) {
            self->firstErr = d.code;
            format(self->msg, sizeof self->msg, "K%u %.*s: %.*s", d.code, KILN_SV(d.where),
                   KILN_SV(d.message));
        }
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
    [[nodiscard]] int count_of(u32 code, Severity sev) const {
        int n = 0;
        for (int i = 0; i < count && i < 256; ++i)
            n += items[i].code == code && items[i].sev == sev;
        return n;
    }
};

cook::MeshCookSettings default_settings() {
    Result<cook::MeshCookSettings> r =
        cook::resolve_mesh(cook::MeshCookSettings{}, cook::TargetProfile{}, cook::CookSession{});
    KILN_VERIFY(r.ok());
    return r.value();
}

struct DirResolver {
    char dir[1024];
    static Status fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) {
        auto* self = static_cast<DirResolver*>(user);
        char path[2048];
        format(path, sizeof path, "%s/%.*s", self->dir, KILN_SV(uri));
        Vec<u8> bytes(alloc, Tag::Cook);
        if (!corpus::read_file(path, bytes)) return make_status(Code::NotFound);
        *out = std::move(bytes);
        return kOk;
    }
};

Result<cook::CookedMesh> cook_bytes(Span<u8 const> bytes, StrView assetPath, Diags& d,
                                    cook::MeshCookSettings const& s, DirResolver* resolver = nullptr,
                                    JobSystem const* jobs = nullptr) {
    cook::MeshSource src{};
    src.bytes      = bytes;
    src.assetPath  = assetPath;
    src.sourcePath = assetPath;
    if (resolver) src.resolver = {&DirResolver::fn, resolver};
    DiagSink sink = d.sink();
    return cook::cook_mesh(src, s, cook::TargetProfile{}, {.diag = &sink, .jobs = jobs});
}

/// A cooked file opened with full validation and its decoded payload.
struct Opened {
    Vec<u8> payload{default_allocator(), Tag::Test};
    mesh::MeshView view;
    bool ok = false;
};

void open_cooked(cook::CookedMesh const& m, Opened& o, char const* name) {
    Result<mesh::MeshView> r = mesh::MeshView::open(m.file.span());
    if (!KILN_CHECK_MSG(r.ok(), "%s: MeshView::open failed (%s)", name, code_name(r.code()))) return;
    o.view = r.value();
    o.payload.resize(usize(o.view.decoded_size()));
    mesh::DecodeOptions dopt;
    dopt.verifyChecksums = true;
    Diags dd;
    DiagSink const dsink = dd.sink();
    if (!KILN_CHECK_MSG(mesh::decode_payload(o.view, o.view.encoded(), o.payload.span(), dopt, &dsink).ok(),
                        "%s: decode_payload failed: %s", name, dd.msg))
        return;
    if (!KILN_CHECK_MSG(mesh::check_indices(o.view, o.payload.span()).ok(), "%s: check_indices failed", name))
        return;
    o.ok = true;
}

void write_sample(char const* name, Span<u8 const> bytes) {
    char const* dir = kiln::test::sample_dir();
    char path[1024];
    format(path, sizeof path, "%s/cooked_%s.mesh", dir, name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "cannot write %s", path)) return;
    KILN_CHECK(std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size);
    std::fclose(f);
}

mesh::VertexAttrib const* find_attrib(mesh::VertexLayout const& l, mesh::Semantic s, u8 index = 0) {
    for (u32 i = 0; i < l.attribCount; ++i)
        if (l.attribs[i].semantic == u8(s) && l.attribs[i].semanticIndex == index) return &l.attribs[i];
    return nullptr;
}

f32 half_to_f32(u16 h) {
    u32 const sign = u32(h & 0x8000u) << 16;
    u32 const exp  = (h >> 10) & 0x1Fu;
    u32 const mant = h & 0x3FFu;
    if (exp == 0) {
        f32 const v = std::ldexp(f32(mant), -24);
        return sign ? -v : v;
    }
    if (exp == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (mant << 13));
    return std::bit_cast<f32>(sign | ((exp + 112u) << 23) | (mant << 13));
}

void decode_position(Opened const& o, mesh::MeshPart const& part, mesh::MeshLod const& lod, u32 v,
                     f32 out[3]) {
    mesh::VertexLayout const& l = o.view.layouts()[lod.layout];
    u8 const* p                 = o.payload.data() + lod.streamOffset[0] + usize(v) * l.strides[0];
    if (l.attribs[0].format == u32(Format::R32G32B32_SFLOAT)) {
        std::memcpy(out, p, 12);
        return;
    }
    for (u32 a = 0; a < 3; ++a)
        out[a] = f32(read_unaligned<u16>(p + a * 2)) / 65535.0f * part.posScale[a] + part.posBias[a];
}

void decode_normal(Opened const& o, mesh::MeshLod const& lod, u32 v, f32 out[3]) {
    mesh::VertexLayout const& l = o.view.layouts()[lod.layout];
    mesh::VertexAttrib const* a = find_attrib(l, mesh::Semantic::Normal);
    u8 const* p =
        o.payload.data() + lod.streamOffset[a->stream] + usize(v) * l.strides[a->stream] + a->offset;
    f32 x = max(f32(read_unaligned<i16>(p)) / 32767.0f, -1.0f);
    f32 y = max(f32(read_unaligned<i16>(p + 2)) / 32767.0f, -1.0f);
    f32 z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0) {
        f32 const ox = (1.0f - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f);
        f32 const oy = (1.0f - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f);
        x            = ox;
        y            = oy;
    }
    f32 const len = std::sqrt(x * x + y * y + z * z);
    out[0]        = x / len;
    out[1]        = y / len;
    out[2]        = z / len;
}

u32 read_index(Opened const& o, mesh::MeshLod const& lod, u32 i) {
    u8 const* p = o.payload.data() + lod.indexOffset;
    return lod.indexType == u8(mesh::IndexType::U16) ? read_unaligned<u16>(p + usize(i) * 2)
                                                     : read_unaligned<u32>(p + usize(i) * 4);
}

/// Count triangles whose geometric normal (CCW) disagrees with the averaged vertex normal.
u32 count_backfacing(Opened const& o, mesh::MeshPart const& part, mesh::MeshLod const& lod) {
    u32 bad = 0;
    for (u32 t = 0; t + 2 < lod.indexCount; t += 3) {
        f32 p[3][3], n[3][3];
        for (u32 c = 0; c < 3; ++c) {
            u32 const v = read_index(o, lod, t + c);
            decode_position(o, part, lod, v, p[c]);
            decode_normal(o, lod, v, n[c]);
        }
        f32 const e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        f32 const e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        f32 const g[3]  = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
        f32 d           = 0;
        for (u32 a = 0; a < 3; ++a)
            d += g[a] * (n[0][a] + n[1][a] + n[2][a]);
        if (d <= 0) ++bad;
    }
    return bad;
}

u32 part_index(mesh::MeshView const& v, StrView name) {
    for (u32 i = 0; i < v.parts().size(); ++i)
        if (v.str(v.parts()[i].nameStr) == name) return i;
    return kInvalid;
}

// Minimal in-test GLB: one quad node "quad" (POSITION/NORMAL/TEXCOORD_0, u16 indices,
// material "paint") with a uniform node scale.
void put_bytes(Vec<u8>& out, void const* p, usize n) {
    out.append(Span<u8 const>(static_cast<u8 const*>(p), n));
}
void put_u32(Vec<u8>& out, u32 v) { put_bytes(out, &v, 4); }

Vec<u8> make_quad_glb(f32 scale, f32 uvScale = 1.0f) {
    f32 const pos[12] = {-1, 0, 1, 1, 0, 1, 1, 0, -1, -1, 0, -1};
    f32 const nrm[12] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    f32 const uv[8]   = {0, 0, uvScale, 0, uvScale, uvScale, 0, uvScale};
    u16 const idx[6]  = {0, 1, 2, 0, 2, 3};
    Vec<u8> bin(default_allocator(), Tag::Test);
    put_bytes(bin, pos, sizeof pos);
    put_bytes(bin, nrm, sizeof nrm);
    put_bytes(bin, uv, sizeof uv);
    put_bytes(bin, idx, sizeof idx);
    while (bin.size() % 4)
        bin.push_back(0);

    char json[2048];
    usize n =
        format(json, sizeof json,
               "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
               "\"nodes\":[{\"name\":\"quad\",\"mesh\":0,\"scale\":[%g,%g,%g]}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
               "\"indices\":3,\"material\":0}]}],"
               "\"materials\":[{\"name\":\"paint\"}],"
               "\"buffers\":[{\"byteLength\":%u}],"
               "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
               "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
               "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
               "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12}],"
               "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
               "\"min\":[-1,0,-1],\"max\":[1,0,1]},"
               "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
               "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
               "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}",
               f64(scale), f64(scale), f64(scale), unsigned(bin.size()));
    while (n % 4)
        json[n++] = ' ';

    Vec<u8> glb(default_allocator(), Tag::Test);
    put_u32(glb, fourcc('g', 'l', 'T', 'F'));
    put_u32(glb, 2);
    put_u32(glb, u32(12 + 8 + n + 8 + bin.size()));
    put_u32(glb, u32(n));
    put_u32(glb, fourcc('J', 'S', 'O', 'N'));
    put_bytes(glb, json, n);
    put_u32(glb, u32(bin.size()));
    put_u32(glb, fourcc('B', 'I', 'N', '\0'));
    put_bytes(glb, bin.data(), bin.size());
    return glb;
}

bool same_bytes(Span<u8 const> a, Span<u8 const> b) { return corpus::bytes_equal(a, b); }

} // namespace

// ===========================================================================
// Always-run tests (in-test GLB)
// ===========================================================================

KILN_TEST(MeshCook, QuadDefaultProfile) {
    Vec<u8> const glb = make_quad_glb(1.0f);
    Diags d;
    Result<cook::CookedMesh> r = cook_bytes(glb.span(), "meshes/quad", d, default_settings());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(d.firstErr, 0u);
    Opened o;
    open_cooked(r.value(), o, "quad");
    KILN_REQUIRE(o.ok);

    KILN_CHECK_EQ(o.view.name(), StrView("quad"));
    KILN_CHECK_EQ(o.view.asset_id(), hash_name("meshes/quad"));
    KILN_CHECK_EQ(r->partCount, 1u);
    KILN_CHECK_EQ(r->lodCount, 1u);
    KILN_CHECK_EQ(r->triangleCount, 2u);
    KILN_CHECK_EQ(r->vertexCount, 4u);

    mesh::MeshLod const& lod    = o.view.lods()[0];
    mesh::VertexLayout const& l = o.view.layouts()[lod.layout];
    KILN_CHECK_EQ(l.streamCount, u8(2));
    KILN_CHECK_EQ(l.attribs[0].format, u32(Format::R16G16B16A16_UNORM));
    KILN_CHECK_EQ(l.strides[0], u16(8));
    KILN_CHECK_EQ(lod.indexType, u8(mesh::IndexType::U16));
    mesh::VertexAttrib const* tan = find_attrib(l, mesh::Semantic::Tangent);
    mesh::VertexAttrib const* uv  = find_attrib(l, mesh::Semantic::TexCoord);
    KILN_REQUIRE(tan && uv);
    KILN_CHECK_EQ(uv->format, u32(Format::R16G16_SFLOAT));
    KILN_CHECK_EQ(l.strides[1], u16(4 + 8 + 4));

    // Positions dequantize to the source corners; UVs round-trip through half.
    mesh::MeshPart const& part = o.view.parts()[0];
    for (u32 v = 0; v < lod.vertexCount; ++v) {
        f32 p[3];
        decode_position(o, part, lod, v, p);
        KILN_CHECK(std::fabs(std::fabs(p[0]) - 1.0f) < 1e-4f && std::fabs(p[1]) < 1e-4f &&
                   std::fabs(std::fabs(p[2]) - 1.0f) < 1e-4f);
        u8 const* q = o.payload.data() + lod.streamOffset[1] + usize(v) * l.strides[1] + uv->offset;
        f32 const u = half_to_f32(read_unaligned<u16>(q));
        f32 const w = half_to_f32(read_unaligned<u16>(q + 2));
        KILN_CHECK((u == 0.0f || u == 1.0f) && (w == 0.0f || w == 1.0f));
        u8 const* t  = o.payload.data() + lod.streamOffset[1] + usize(v) * l.strides[1] + tan->offset;
        i16 const tw = read_unaligned<i16>(t + 6);
        KILN_CHECK(tw == 32767 || tw == -32767);
    }
    KILN_CHECK_EQ(count_backfacing(o, part, lod), 0u);
    KILN_CHECK_EQ(o.view.materials().size(), 1u);
    KILN_CHECK_EQ(o.view.str(o.view.materials()[0].nameStr), StrView("paint"));
    write_sample("quad", r->file.span());
}

KILN_TEST(MeshCook, LargeMeshFallsBackToFloatPositions) {
    Vec<u8> const glb = make_quad_glb(100000.0f);
    Diags d;
    Result<cook::CookedMesh> r = cook_bytes(glb.span(), "meshes/huge", d, default_settings());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(d.count_of(cook::kDiagGltfQuantFallback, Severity::Info), 1);
    KILN_CHECK_EQ(d.count_of(cook::kDiagGltfScaleBaked, Severity::Info), 1);
    Opened o;
    open_cooked(r.value(), o, "huge");
    KILN_REQUIRE(o.ok);
    mesh::MeshLod const& lod    = o.view.lods()[0];
    mesh::VertexLayout const& l = o.view.layouts()[lod.layout];
    KILN_CHECK_EQ(l.attribs[0].format, u32(Format::R32G32B32_SFLOAT));
    KILN_CHECK_EQ(l.strides[0], u16(12));
    mesh::MeshPart const& part = o.view.parts()[0];
    KILN_CHECK_EQ(part.posScale[0], 1.0f);
    KILN_CHECK_EQ(part.posBias[0], 0.0f);
    f32 p[3];
    decode_position(o, part, lod, 0, p);
    KILN_CHECK_EQ(std::fabs(p[0]), 100000.0f); // scale baked, exact
    KILN_CHECK(std::fabs(part.bounds.halfExtents[0] - 100000.0f) < 1.0f);
}

KILN_TEST(MeshCook, TolerancesAndProfiles) {
    Vec<u8> const glb = make_quad_glb(1.0f, 4096.0f); // UVs beyond +-2048
    {
        Diags d;
        cook::MeshCookSettings s   = default_settings();
        s.posTolMm                 = 1e-6f; // 2 m / 65535 > 1e-9 m -> float positions
        Result<cook::CookedMesh> r = cook_bytes(glb.span(), "meshes/tol", d, s);
        KILN_REQUIRE(r.ok());
        KILN_CHECK_EQ(d.count_of(cook::kDiagGltfQuantFallback, Severity::Info), 2); // positions + UV0
        Opened o;
        open_cooked(r.value(), o, "tol");
        KILN_REQUIRE(o.ok);
        mesh::VertexLayout const& l = o.view.layouts()[o.view.lods()[0].layout];
        KILN_CHECK_EQ(l.attribs[0].format, u32(Format::R32G32B32_SFLOAT));
        mesh::VertexAttrib const* uv = find_attrib(l, mesh::Semantic::TexCoord);
        KILN_REQUIRE(uv != nullptr);
        KILN_CHECK_EQ(uv->format, u32(Format::R32G32_SFLOAT));
    }
    {
        Diags d;
        cook::MeshCookSettings s   = default_settings();
        s.profile                  = cook::VertexProfile::Precise;
        s.genTangents              = false;
        s.optimize                 = false;
        Result<cook::CookedMesh> r = cook_bytes(glb.span(), "meshes/precise", d, s);
        KILN_REQUIRE(r.ok());
        KILN_CHECK_EQ(d.count_of(cook::kDiagGltfQuantFallback, Severity::Info), 0); // precise by request
        Opened o;
        open_cooked(r.value(), o, "precise");
        KILN_REQUIRE(o.ok);
        mesh::VertexLayout const& l = o.view.layouts()[o.view.lods()[0].layout];
        KILN_CHECK_EQ(l.attribs[0].format, u32(Format::R32G32B32_SFLOAT));
        KILN_CHECK(find_attrib(l, mesh::Semantic::Tangent) == nullptr);
        KILN_CHECK_EQ(l.strides[1], u16(4 + 8));
    }
    {
        Diags d;
        cook::MeshCookSettings s   = default_settings();
        s.profile                  = cook::VertexProfile::Float;
        Result<cook::CookedMesh> r = cook_bytes(glb.span(), "meshes/float", d, s);
        KILN_REQUIRE(r.ok());
        Opened o;
        open_cooked(r.value(), o, "float");
        KILN_REQUIRE(o.ok);
        mesh::VertexLayout const& l = o.view.layouts()[o.view.lods()[0].layout];
        KILN_CHECK_EQ(l.attribs[0].format, u32(Format::R32G32B32_SFLOAT));
        mesh::VertexAttrib const* n  = find_attrib(l, mesh::Semantic::Normal);
        mesh::VertexAttrib const* t  = find_attrib(l, mesh::Semantic::Tangent);
        mesh::VertexAttrib const* uv = find_attrib(l, mesh::Semantic::TexCoord);
        KILN_REQUIRE(n && t && uv);
        KILN_CHECK_EQ(n->format, u32(Format::R32G32B32_SFLOAT));
        KILN_CHECK_EQ(t->format, u32(Format::R32G32B32A32_SFLOAT));
        KILN_CHECK_EQ(uv->format, u32(Format::R32G32_SFLOAT));
        KILN_CHECK_EQ(l.strides[1], u16(12 + 16 + 8));
    }
}

KILN_TEST(MeshCook, DeterministicAndErrors) {
    Vec<u8> const glb = make_quad_glb(2.5f);
    Diags d1, d2;
    Result<cook::CookedMesh> a = cook_bytes(glb.span(), "meshes/det", d1, default_settings());
    Result<cook::CookedMesh> b = cook_bytes(glb.span(), "meshes/det", d2, default_settings());
    KILN_REQUIRE(a.ok() && b.ok());
    KILN_CHECK(same_bytes(a->file.span(), b->file.span()));
    KILN_CHECK_EQ(a->sourceHash, xxh64(glb.span()));

    // Garbage and truncated input: ParseError K1001.
    u8 const junk[16] = {'n', 'o', 't', ' ', 'g', 'l', 't', 'f'};
    Diags d3;
    Result<cook::CookedMesh> c =
        cook_bytes(Span<u8 const>(junk, sizeof junk), "meshes/junk", d3, default_settings());
    KILN_CHECK_EQ(c.code(), Code::ParseError);
    KILN_CHECK_EQ(d3.firstErr, u32(cook::kDiagGltfParseFailed));
    Diags d4;
    Result<cook::CookedMesh> t =
        cook_bytes(glb.span().first(glb.size() - 20), "meshes/trunc", d4, default_settings());
    KILN_CHECK(t.failed());
    KILN_CHECK(d4.firstErr == cook::kDiagGltfParseFailed || d4.firstErr == cook::kDiagGltfBadAccessor);
}

// ===========================================================================
// Corpus (tests/corpus/gltf/manifest.txt)
// ===========================================================================

namespace {

struct GltfEntry {
    StrView path;
    bool expectOk = false;
    u32 code = 0, parts = 0, lods = 0, materials = 0, textures = 0, mounts = 0;
};

void gltf_dir(char* out, usize cap) { format(out, cap, "%s/../gltf", kiln::test::corpus_dir()); }

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

struct CorpusCook {
    Vec<u8> bytes{default_allocator(), Tag::Test};
    DirResolver resolver{};
    Diags diags;
    Result<cook::CookedMesh> result = Code::NotFound;
    Opened opened;
    char name[128] = {};
};

/// Cook `<gltf dir>/<rel>` with default settings (and a sibling-file resolver).
bool cook_corpus(char const* rel, CorpusCook& c, bool withResolver = true, JobSystem const* jobs = nullptr) {
    char dir[1024];
    gltf_dir(dir, sizeof dir);
    char path[1400];
    format(path, sizeof path, "%s/%s", dir, rel);
    if (!KILN_CHECK_MSG(corpus::read_file(path, c.bytes), "cannot read %s", path)) return false;
    format(c.resolver.dir, sizeof c.resolver.dir, "%s", path);
    if (char* slash = std::strrchr(c.resolver.dir, '/')) *slash = '\0';
    StrView r(rel);
    usize const slash = r.rfind('/');
    StrView stem      = slash == StrView::kNpos ? r : r.substr(slash + 1);
    usize const dot   = stem.rfind('.');
    if (dot != StrView::kNpos) stem = stem.substr(0, dot);
    format(c.name, sizeof c.name, "%.*s", KILN_SV(stem));
    char asset[256];
    format(asset, sizeof asset, "meshes/%s", c.name);
    c.result = cook_bytes(c.bytes.span(), StrView(asset), c.diags, default_settings(),
                          withResolver ? &c.resolver : nullptr, jobs);
    if (c.result.ok()) open_cooked(c.result.value(), c.opened, c.name);
    return true;
}

} // namespace

KILN_TEST(MeshCook, CorpusManifest) {
    char dir[1024];
    gltf_dir(dir, sizeof dir);
    Vec<char> text(default_allocator(), Tag::Test);
    Vec<GltfEntry> entries(default_allocator(), Tag::Test);
    if (!load_gltf_manifest(dir, text, entries)) return;

    for (GltfEntry const& e : entries) {
        char rel[512];
        format(rel, sizeof rel, "%.*s", KILN_SV(e.path));
        CorpusCook c;
        if (!cook_corpus(rel, c)) continue;
        if (!e.expectOk) {
            KILN_CHECK_MSG(c.result.failed(), "%s: expected failure", rel);
            KILN_CHECK_MSG(c.diags.firstErr == e.code, "%s: error K%u, manifest K%u (%s)", rel,
                           c.diags.firstErr, e.code, c.diags.msg);
            continue;
        }
        if (!KILN_CHECK_MSG(c.result.ok(), "%s: cook failed: %s", rel, c.diags.msg)) continue;
        if (!c.opened.ok) continue;
        mesh::MeshView const& v = c.opened.view;
        u32 maxLods             = 0;
        for (mesh::MeshPart const& p : v.parts())
            maxLods = max(maxLods, p.lodCount);
        KILN_CHECK_MSG(v.parts().size() == e.parts, "%s: parts %u, manifest %u", rel, v.parts().size(),
                       e.parts);
        KILN_CHECK_MSG(maxLods == e.lods, "%s: lods %u, manifest %u", rel, maxLods, e.lods);
        KILN_CHECK_MSG(v.materials().size() == e.materials, "%s: materials %u, manifest %u", rel,
                       v.materials().size(), e.materials);
        // Referenced images: the embedded ones the cook outputs, plus distinct external URIs.
        u32 images = u32(c.result->textures.size());
        for (u32 i = 0; i < v.textures().size(); ++i) {
            mesh::TextureBinding const& b = v.textures()[i];
            bool seen                     = !(b.flags & mesh::kTextureExternal);
            for (u32 j = 0; j < i && !seen; ++j)
                seen =
                    (v.textures()[j].flags & mesh::kTextureExternal) && v.textures()[j].pathStr == b.pathStr;
            images += !seen;
            if (!(b.flags & mesh::kTextureExternal)) { // an embedded image's name is used as stored
                char name[kMaxAssetNameLen + 1];
                KILN_CHECK_EQ(texture_asset_name(StrView(rel), v, b, name, sizeof name), v.str(b.pathStr));
            }
        }
        KILN_CHECK_MSG(images == e.textures, "%s: textures %u, manifest %u", rel, images, e.textures);
        KILN_CHECK_MSG(v.mounts().size() == e.mounts, "%s: mounts %u, manifest %u", rel, v.mounts().size(),
                       e.mounts);
        KILN_CHECK_EQ(c.result->partCount, v.parts().size());
        KILN_CHECK_EQ(c.result->lodCount, v.lods().size());

        // Every triangle faces the way its normals say (winding/normal consistency).
        // All generated corpus meshes are authored CCW-front-facing with outward
        // normals (generate.py quad()/box_welded()), so this runs unconditionally.
        for (mesh::MeshPart const& p : v.parts())
            for (u32 li = 0; li < p.lodCount; ++li) {
                u32 const bad = count_backfacing(c.opened, p, v.lods()[p.lodFirst + li]);
                KILN_CHECK_MSG(bad == 0, "%s: part %.*s lod %u: %u triangle(s) disagree with their normals",
                               rel, KILN_SV(v.str(p.nameStr)), li, bad);
            }

        // Determinism: a second cook is byte-identical.
        CorpusCook again;
        if (cook_corpus(rel, again) && again.result.ok())
            KILN_CHECK_MSG(same_bytes(c.result->file.span(), again.result->file.span()),
                           "%s: not deterministic", rel);
        write_sample(c.name, c.result->file.span());
    }
}

KILN_TEST(MeshCook, CorpusHierarchyParts) {
    CorpusCook c;
    if (!cook_corpus("generated/hierarchy_parts.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.parts().size(), 4u);
    u32 const hull = part_index(v, "hull"), wl = part_index(v, "wing_l"), wr = part_index(v, "wing_r"),
              ant = part_index(v, "antenna");
    KILN_REQUIRE(hull != kInvalid && wl != kInvalid && wr != kInvalid && ant != kInvalid);
    KILN_CHECK_EQ(hull, 0u);
    KILN_CHECK_EQ(v.parts()[hull].parent, kInvalid);
    KILN_CHECK_EQ(v.parts()[wl].parent, hull);
    KILN_CHECK_EQ(v.parts()[wr].parent, hull);
    KILN_CHECK_EQ(v.parts()[ant].parent, wl);
    KILN_CHECK_EQ(part_index(v, "ship"), kInvalid);
    // The mirrored wing was baked (K1011) and keeps CCW front faces.
    KILN_CHECK(c.diags.count_of(cook::kDiagGltfScaleBaked, Severity::Info) >= 1);
    mesh::MeshPart const& r = v.parts()[wr];
    KILN_CHECK_EQ(count_backfacing(c.opened, r, v.lods()[r.lodFirst]), 0u);
    // Signed volume of the closed wing box is positive (outward winding) for both wings.
    for (u32 pi : {wl, wr}) {
        mesh::MeshPart const& p  = v.parts()[pi];
        mesh::MeshLod const& lod = v.lods()[p.lodFirst];
        f32 vol                  = 0;
        for (u32 t = 0; t + 2 < lod.indexCount; t += 3) {
            f32 a[3], b[3], cc[3];
            decode_position(c.opened, p, lod, read_index(c.opened, lod, t), a);
            decode_position(c.opened, p, lod, read_index(c.opened, lod, t + 1), b);
            decode_position(c.opened, p, lod, read_index(c.opened, lod, t + 2), cc);
            vol += a[0] * (b[1] * cc[2] - b[2] * cc[1]) - a[1] * (b[0] * cc[2] - b[2] * cc[0]) +
                   a[2] * (b[0] * cc[1] - b[1] * cc[0]);
        }
        KILN_CHECK_MSG(vol > 0, "part %u signed volume %g", pi, f64(vol));
    }
    // Rotations stay unit quaternions; no scale remains on the parts.
    for (mesh::MeshPart const& p : v.parts()) {
        f32 const q = p.rotation[0] * p.rotation[0] + p.rotation[1] * p.rotation[1] +
                      p.rotation[2] * p.rotation[2] + p.rotation[3] * p.rotation[3];
        KILN_CHECK(std::fabs(q - 1.0f) < 1e-5f);
    }
}

KILN_TEST(MeshCook, CorpusMountsExtras) {
    CorpusCook c;
    if (!cook_corpus("generated/mounts_extras.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.mounts().size(), 3u);
    for (u32 i = 1; i < v.mounts().size(); ++i)
        KILN_CHECK(v.mounts()[i - 1].nameHash <= v.mounts()[i].nameHash);
    mesh::Mount const* gun = v.find_mount(hash_name("mount_gun"));
    KILN_REQUIRE(gun != nullptr);
    KILN_CHECK_EQ(v.str(gun->nameStr), StrView("mount_gun"));
    KILN_CHECK_EQ(gun->parentPart, 0u);
    // Pairs in the order the file writes them (this file's JSON keys are sorted).
    KILN_CHECK_EQ(v.str(gun->extrasStr), StrView("enabled=true;size=2;slot=hardpoint"));
    KILN_CHECK(std::fabs(gun->rotation[1] - 0.382683f) < 1e-4f &&
               std::fabs(gun->rotation[3] - 0.92388f) < 1e-4f);
    KILN_CHECK(std::fabs(gun->translation[1] - 0.3f) < 1e-6f &&
               std::fabs(gun->translation[2] - 0.8f) < 1e-6f);
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfExtrasDropped, Severity::Warning), 2);
    mesh::Mount const* e1 = v.find_mount(hash_name("mount_engine_01"));
    KILN_REQUIRE(e1 != nullptr);
    KILN_CHECK_EQ(e1->extrasStr, kInvalid);
}

KILN_TEST(MeshCook, CorpusAuthoredLods) {
    CorpusCook c;
    if (!cook_corpus("generated/authored_lods.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.parts().size(), 1u);
    mesh::MeshPart const& p = v.parts()[0];
    KILN_CHECK_EQ(v.str(p.nameStr), StrView("hull"));
    KILN_REQUIRE_EQ(p.lodCount, 3u);
    u32 const tris[3] = {48, 12, 2};
    for (u32 i = 0; i < 3; ++i) {
        KILN_CHECK_EQ(v.lods()[p.lodFirst + i].indexCount, tris[i] * 3);
        KILN_CHECK_EQ(v.lods()[p.lodFirst + i].geometricError, 0.0f);
    }
    KILN_CHECK_EQ(part_index(v, "col_hull"), kInvalid);
    KILN_CHECK_EQ(part_index(v, "_helper"), kInvalid);
    KILN_CHECK_EQ(v.mounts().size(), 0u);

    // useAuthoredLods = false keeps LOD0 only.
    cook::MeshCookSettings s = default_settings();
    s.useAuthoredLods        = false;
    Diags d;
    Result<cook::CookedMesh> r = cook_bytes(c.bytes.span(), "meshes/authored_lods", d, s);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->lodCount, 1u);
}

KILN_TEST(MeshCook, CorpusMultiMaterial) {
    CorpusCook c;
    if (!cook_corpus("generated/multi_material.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.materials().size(), 3u);
    KILN_CHECK_EQ(v.str(v.materials()[0].nameStr), StrView("metal"));
    KILN_CHECK_EQ(v.str(v.materials()[1].nameStr), StrView("glass"));
    KILN_CHECK_EQ(v.str(v.materials()[2].nameStr), StrView("decal"));
    KILN_CHECK_EQ(v.materials()[1].alphaMode, u8(mesh::AlphaMode::Blend));
    KILN_CHECK_EQ(v.materials()[2].alphaMode, u8(mesh::AlphaMode::Mask));
    KILN_CHECK_EQ(v.materials()[0].nameHash, hash_name("metal"));
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfMaterialRenamed, Severity::Info), 2);
    // PBR factors as authored; glTF defaults for what the source leaves out.
    mesh::MaterialSlot const& metal = v.materials()[0];
    mesh::MaterialSlot const& glass = v.materials()[1];
    KILN_CHECK(metal.baseColorFactor[0] == 0.5f && metal.baseColorFactor[2] == 0.55f &&
               metal.baseColorFactor[3] == 1.0f);
    KILN_CHECK(metal.metallicFactor == 1.0f && metal.roughnessFactor == 0.3f);
    KILN_CHECK(glass.baseColorFactor[3] == 0.3f && glass.metallicFactor == 0.0f &&
               glass.roughnessFactor == 0.05f);
    KILN_CHECK(metal.emissiveFactor[0] == 0.0f && metal.normalScale == 1.0f &&
               metal.occlusionStrength == 1.0f);
    KILN_CHECK_EQ(metal._reserved, 0u);
    mesh::MeshLod const& lod = v.lods()[0];
    KILN_REQUIRE_EQ(lod.submeshCount, 4u);
    KILN_CHECK_EQ(v.submeshes()[lod.submeshFirst + 2].material, 2u);
    KILN_CHECK_EQ(v.submeshes()[lod.submeshFirst + 3].material, 2u);
}

KILN_TEST(MeshCook, CorpusTwoUvSets) {
    CorpusCook c;
    if (!cook_corpus("generated/two_uv_sets.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v     = c.opened.view;
    mesh::VertexLayout const& l = v.layouts()[v.lods()[0].layout];
    KILN_CHECK(find_attrib(l, mesh::Semantic::TexCoord, 0) != nullptr);
    KILN_CHECK(find_attrib(l, mesh::Semantic::TexCoord, 1) != nullptr);
    KILN_REQUIRE_EQ(v.textures().size(), 2u);
    mesh::TextureBinding const& b0 = v.textures()[0];
    mesh::TextureBinding const& b1 = v.textures()[1];
    KILN_CHECK_EQ(b0.slot, u8(mesh::TextureSlot::BaseColor));
    KILN_CHECK_EQ(b0.uvSet, u8(0));
    KILN_CHECK_EQ(b0.flags, u16(mesh::kTextureSrgb));
    KILN_CHECK_EQ(b1.slot, u8(mesh::TextureSlot::Occlusion));
    KILN_CHECK_EQ(b1.uvSet, u8(1));
    KILN_CHECK_EQ(b1.flags, u16(0));
    KILN_CHECK_EQ(v.str(b0.pathStr), StrView("meshes/two_uv_sets#albedo"));
    KILN_CHECK_EQ(b0.textureId, hash_name("meshes/two_uv_sets#albedo"));
}

KILN_TEST(MeshCook, CorpusNoUvNoNormals) {
    CorpusCook c;
    if (!cook_corpus("generated/no_uv_no_normals.glb", c) || !c.opened.ok) return;
    mesh::MeshView const& v     = c.opened.view;
    mesh::VertexLayout const& l = v.layouts()[v.lods()[0].layout];
    KILN_CHECK(find_attrib(l, mesh::Semantic::Normal) != nullptr);
    KILN_CHECK(find_attrib(l, mesh::Semantic::Tangent) == nullptr);
    KILN_CHECK(find_attrib(l, mesh::Semantic::TexCoord) == nullptr);
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfNoTangentSource, Severity::Warning), 1);
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfNoNormals, Severity::Info), 1);
    KILN_REQUIRE_EQ(v.materials().size(), 1u);
    KILN_CHECK_EQ(v.str(v.materials()[0].nameStr), StrView("default"));
    // Smooth normals on the welded cube run roughly along the corner diagonals, on the front side.
    // The cube is wound CCW from outside, so front is outside; the cooker follows authored winding.
    mesh::MeshLod const& lod = v.lods()[0];
    KILN_CHECK_EQ(lod.vertexCount, 8u);
    KILN_CHECK_EQ(count_backfacing(c.opened, v.parts()[0], lod), 0u);
    for (u32 i = 0; i < lod.vertexCount; ++i) {
        f32 p[3], n[3];
        decode_position(c.opened, v.parts()[0], lod, i, p);
        decode_normal(c.opened, lod, i, n);
        f32 const d =
            (p[0] * n[0] + p[1] * n[1] + p[2] * n[2]) / std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        // Area weighting favours the face split into two triangles at this corner, so
        // the normal is near (not exactly on) the diagonal.
        KILN_CHECK_MSG(std::fabs(d) > 0.9f, "vertex %u: normal not along the diagonal (%g)", i, f64(d));
    }
}

KILN_TEST(MeshCook, CorpusPbrTextures) {
    CorpusCook c;
    if (!cook_corpus("generated/pbr_textures.glb", c) || !c.opened.ok) return;
    Vec<cook::TextureRef> const& t = c.result->textures;
    KILN_REQUIRE_EQ(t.size(), usize(4));
    struct Want {
        char const* path;
        cook::SlotHint slot;
        bool srgb;
    } const want[4] = {
        {"meshes/pbr_textures#hull_albedo",   cook::SlotHint::BaseColor,         true },
        {"meshes/pbr_textures#hull_normal",   cook::SlotHint::Normal,            false},
        {"meshes/pbr_textures#hull_orm",      cook::SlotHint::MetallicRoughness, false},
        {"meshes/pbr_textures#hull_emissive", cook::SlotHint::Emissive,          true },
    };
    for (u32 i = 0; i < 4; ++i) {
        KILN_CHECK_EQ(t[i].assetPath, StrView(want[i].path));
        KILN_CHECK_EQ(t[i].slot, want[i].slot);
        KILN_CHECK_EQ(t[i].srgb, want[i].srgb);
        KILN_CHECK(t[i].embedded.size > 8 && t[i].embedded[1] == 'P' && t[i].embedded[2] == 'N');
        KILN_CHECK_EQ(t[i].mimeType, StrView("image/png"));
    }
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.textures().size(), 5u); // ORM bound twice
    KILN_CHECK_EQ(v.materials()[0].textureCount, 5u);
    KILN_CHECK_EQ(v.textures()[2].textureId, v.textures()[3].textureId);
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfUsageConflict, Severity::Warning),
                  0); // MR and AO: both Orm
}

// generated/jpeg_texture.glb: baseColorTexture is a real embedded JPEG (image/jpeg), not
// the pure-Python PNGs every other corpus file embeds.
KILN_TEST(MeshCook, CorpusJpegTexture) {
    CorpusCook c;
    if (!cook_corpus("generated/jpeg_texture.glb", c) || !c.opened.ok) return;
    Vec<cook::TextureRef> const& t = c.result->textures;
    KILN_REQUIRE_EQ(t.size(), usize(1));
    KILN_CHECK_EQ(t[0].assetPath, StrView("meshes/jpeg_texture#albedo"));
    KILN_CHECK_EQ(t[0].slot, cook::SlotHint::BaseColor);
    KILN_CHECK(t[0].srgb);
    KILN_CHECK_EQ(t[0].mimeType, StrView("image/jpeg"));
    KILN_REQUIRE(t[0].embedded.size >= 3);
    KILN_CHECK(t[0].embedded[0] == 0xFF && t[0].embedded[1] == 0xD8 && t[0].embedded[2] == 0xFF);

    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.textures().size(), 1u);
    KILN_CHECK_EQ(v.materials()[0].textureCount, 1u);
}

KILN_TEST(MeshCook, CorpusRejects) {
    CorpusCook draco;
    if (!cook_corpus("generated/draco_required.glb", draco)) return;
    KILN_CHECK_EQ(draco.result.code(), Code::Unsupported);
    KILN_CHECK_EQ(draco.diags.firstErr, u32(cook::kDiagGltfUnsupportedExt));
    CorpusCook sparse;
    if (!cook_corpus("generated/sparse_accessor.glb", sparse)) return;
    KILN_CHECK_EQ(sparse.result.code(), Code::Unsupported);
    KILN_CHECK_EQ(sparse.diags.firstErr, u32(cook::kDiagGltfSparseAccessor));
}

KILN_TEST(MeshCook, CorpusNonTriangle) {
    CorpusCook c;
    if (!cook_corpus("generated/non_triangle.glb", c) || !c.opened.ok) return;
    KILN_CHECK_EQ(c.diags.count_of(cook::kDiagGltfPrimitiveSkipped, Severity::Warning), 1);
    KILN_CHECK_EQ(c.opened.view.lods().size(), 1u);
    KILN_CHECK_EQ(c.opened.view.lods()[0].submeshCount, 1u);
}

KILN_TEST(MeshCook, CorpusExternalUri) {
    CorpusCook c;
    if (!cook_corpus("generated/external_uri.gltf", c)) return;
    if (!KILN_CHECK_MSG(c.result.ok(), "%s", c.diags.msg)) return;
    KILN_REQUIRE(c.opened.ok);
    // An external image is not an output of the mesh cook: only the binding records it.
    KILN_CHECK(c.result->textures.empty());
    mesh::MeshView const& v = c.opened.view;
    KILN_REQUIRE_EQ(v.textures().size(), 1u);
    mesh::TextureBinding const& b = v.textures()[0];
    KILN_CHECK_EQ(v.str(b.pathStr), StrView("external_uri_albedo.png"));
    KILN_CHECK_EQ(b.flags, u16(mesh::kTextureExternal | mesh::kTextureSrgb));
    KILN_CHECK_EQ(b.textureId, u64(0));
    char name[kMaxAssetNameLen + 1];
    KILN_CHECK_EQ(texture_asset_name("models/external_uri.gltf", v, b, name, sizeof name),
                  StrView("models/external_uri_albedo.png"));
    KILN_CHECK_EQ(texture_asset_name("pool:external_uri.gltf", v, b, name, sizeof name),
                  StrView("pool:external_uri_albedo.png"));
    KILN_CHECK(texture_asset_name("external_uri.gltf", v, b, name, 8).empty()); // does not fit

    CorpusCook none;
    if (!cook_corpus("generated/external_uri.gltf", none, false)) return;
    KILN_CHECK_EQ(none.result.code(), Code::NotFound);
    KILN_CHECK_EQ(none.diags.firstErr, u32(cook::kDiagGltfExternalMissing));

    // Same bytes cooked through the resolver are deterministic too.
    CorpusCook again;
    if (cook_corpus("generated/external_uri.gltf", again) && again.result.ok())
        KILN_CHECK(same_bytes(c.result->file.span(), again.result->file.span()));
}

KILN_TEST(MeshCook, CorpusExternalUriPercentEncoded) {
    CorpusCook c;
    if (!cook_corpus("generated/external_uri.gltf", c)) return;
    // Encode the '_' in both URIs as %5F; the resolver and the binding see plain file names.
    Vec<u8> patched(default_allocator(), Tag::Test);
    StrView const text(reinterpret_cast<char const*>(c.bytes.data()), c.bytes.size());
    StrView const needles[] = {"\"external_uri.bin\"", "\"external_uri_albedo.png\""};
    StrView const encoded[] = {"\"external%5Furi.bin\"", "\"external%5Furi%5Falbedo.png\""};
    usize at                = 0;
    for (usize i = 0; i < 2; ++i) {
        usize pos = at;
        while (pos + needles[i].size <= text.size && text.substr(pos, needles[i].size) != needles[i])
            ++pos;
        KILN_REQUIRE(pos + needles[i].size <= text.size);
        patched.append(Span<u8 const>(c.bytes.data() + at, pos - at));
        patched.append(Span<u8 const>(reinterpret_cast<u8 const*>(encoded[i].data), encoded[i].size));
        at = pos + needles[i].size;
    }
    patched.append(Span<u8 const>(c.bytes.data() + at, c.bytes.size() - at));

    Diags d;
    Result<cook::CookedMesh> r =
        cook_bytes(patched.span(), "meshes/external_uri", d, default_settings(), &c.resolver);
    if (!KILN_CHECK_MSG(r.ok(), "%s", d.msg)) return;
    Opened o;
    open_cooked(r.value(), o, "external_uri (encoded)");
    if (!o.ok) return;
    KILN_REQUIRE_EQ(o.view.textures().size(), 1u);
    KILN_CHECK_EQ(o.view.str(o.view.textures()[0].pathStr), StrView("external_uri_albedo.png"));
}

// A URI must name a file in the source's root (K1020), and the source needs a valid name.
KILN_TEST(MeshCook, ExternalUriStaysInTheRoot) {
    CorpusCook c;
    if (!cook_corpus("generated/external_uri.gltf", c)) return;
    StrView const text(reinterpret_cast<char const*>(c.bytes.data()), c.bytes.size());
    auto const patch = [&text](StrView needle, StrView with, Vec<u8>& out) {
        usize pos = 0;
        while (pos + needle.size <= text.size && text.substr(pos, needle.size) != needle)
            ++pos;
        KILN_REQUIRE(pos + needle.size <= text.size);
        out.append(Span<u8 const>(reinterpret_cast<u8 const*>(text.data), pos));
        out.append(Span<u8 const>(reinterpret_cast<u8 const*>(with.data), with.size));
        out.append(Span<u8 const>(reinterpret_cast<u8 const*>(text.data) + pos + needle.size,
                                  text.size - pos - needle.size));
    };
    struct Case {
        StrView needle, with, asset;
    };
    Case const cases[] = {
        {"\"external_uri.bin\"",        "\"../external_uri.bin\"",        "external_uri.gltf"       },
        {"\"external_uri_albedo.png\"", "\"../external_uri_albedo.png\"", "external_uri.gltf"       },
        {"\"external_uri_albedo.png\"", "\"/external_uri_albedo.png\"",   "meshes/external_uri.gltf"},
    };
    for (Case const& k : cases) {
        Vec<u8> patched(default_allocator(), Tag::Test);
        patch(k.needle, k.with, patched);
        Diags d;
        Result<cook::CookedMesh> r = cook_bytes(patched.span(), k.asset, d, default_settings(), &c.resolver);
        KILN_CHECK_EQ(r.code(), Code::ValidationFailed);
        KILN_CHECK_EQ(d.firstErr, u32(cook::kDiagGltfUriOutsideRoot));
    }

    Diags d;
    Result<cook::CookedMesh> r =
        cook_bytes(c.bytes.span(), "./external_uri.gltf", d, default_settings(), &c.resolver);
    KILN_CHECK_EQ(r.code(), Code::InvalidArgument);
}

// Embedded image names become "<mesh>#<name>", so a duplicate name or a reserved character is
// an error (K1019). The corpus file is patched with same-length names, so GLB chunk sizes hold.
KILN_TEST(MeshCook, EmbeddedImageNameRules) {
    CorpusCook c;
    if (!cook_corpus("generated/pbr_textures.glb", c)) return;
    KILN_REQUIRE(c.result.ok());
    for (StrView const renamed :
         {StrView("\"hull_albedo\""), StrView("\"hull#normal\""), StrView("\"hull?normal\"")}) {
        Vec<u8> patched(default_allocator(), Tag::Test);
        patched.append(c.bytes.span());
        StrView const from = "\"hull_normal\"";
        usize hits         = 0;
        for (usize i = 0; i + from.size <= patched.size(); ++i)
            if (std::memcmp(patched.data() + i, from.data, from.size) == 0) {
                std::memcpy(patched.data() + i, renamed.data, renamed.size);
                ++hits;
            }
        KILN_REQUIRE(hits > 0);
        Diags d;
        Result<cook::CookedMesh> r = cook_bytes(patched.span(), "meshes/pbr_textures", d, default_settings());
        KILN_CHECK_EQ(r.code(), Code::ValidationFailed);
        KILN_CHECK_EQ(d.firstErr, u32(cook::kDiagGltfImageName));
    }
}

namespace {

bool same_diags(Diags const& a, Diags const& b) {
    if (a.count != b.count || a.firstErr != b.firstErr) return false;
    for (int i = 0; i < a.count && i < 256; ++i)
        if (a.items[i].code != b.items[i].code || a.items[i].sev != b.items[i].sev) return false;
    return true;
}

bool same_texture_refs(cook::CookedMesh const& a, cook::CookedMesh const& b) {
    if (a.textures.size() != b.textures.size()) return false;
    for (usize i = 0; i < a.textures.size(); ++i) {
        cook::TextureRef const& x = a.textures[i];
        cook::TextureRef const& y = b.textures[i];
        if (x.assetPath != y.assetPath || x.mimeType != y.mimeType || x.slot != y.slot || x.srgb != y.srgb ||
            !same_bytes(x.embedded, y.embedded))
            return false;
    }
    return true;
}

} // namespace

// The per-(part, LOD) tasks must not change a byte or the diagnostic order: every
// manifest entry and the float-fallback quad, cooked without a pool and with 1 and 8 threads.
KILN_TEST(MeshCook, threads_byte_identical) {
    Result<JobSystem> one   = create_thread_pool({.threads = 1});
    Result<JobSystem> eight = create_thread_pool({.threads = 8});
    KILN_REQUIRE(one.ok() && eight.ok());
    JobSystem const* const pools[] = {&*one, &*eight};

    {
        Vec<u8> const glb = make_quad_glb(100000.0f, 4096.0f); // float positions and UVs
        Diags d0;
        Result<cook::CookedMesh> r0 = cook_bytes(glb.span(), "meshes/huge", d0, default_settings());
        KILN_REQUIRE(r0.ok());
        for (JobSystem const* jobs : pools) {
            Diags d;
            Result<cook::CookedMesh> r =
                cook_bytes(glb.span(), "meshes/huge", d, default_settings(), nullptr, jobs);
            KILN_REQUIRE(r.ok());
            KILN_CHECK(same_bytes(r0->file.span(), r->file.span()));
            KILN_CHECK(same_diags(d0, d));
        }
    }

    char dir[1024];
    gltf_dir(dir, sizeof dir);
    {
        Vec<char> text(default_allocator(), Tag::Test);
        Vec<GltfEntry> entries(default_allocator(), Tag::Test);
        if (load_gltf_manifest(dir, text, entries))
            for (GltfEntry const& e : entries) {
                char rel[512];
                format(rel, sizeof rel, "%.*s", KILN_SV(e.path));
                CorpusCook base;
                if (!cook_corpus(rel, base)) continue;
                for (JobSystem const* jobs : pools) {
                    CorpusCook c;
                    if (!cook_corpus(rel, c, true, jobs)) continue;
                    KILN_CHECK_MSG(same_diags(base.diags, c.diags),
                                   "%s: diagnostics differ with %u thread(s)", rel,
                                   thread_pool_thread_count(*jobs));
                    if (!KILN_CHECK_MSG(base.result.ok() == c.result.ok(), "%s: status differs", rel))
                        continue;
                    if (!base.result.ok()) continue;
                    KILN_CHECK_MSG(same_bytes(base.result->file.span(), c.result->file.span()),
                                   "%s: .mesh bytes differ with %u thread(s)", rel,
                                   thread_pool_thread_count(*jobs));
                    KILN_CHECK_MSG(same_texture_refs(base.result.value(), c.result.value()),
                                   "%s: texture refs differ with %u thread(s)", rel,
                                   thread_pool_thread_count(*jobs));
                }
            }
    }
    destroy_thread_pool(*eight);
    destroy_thread_pool(*one);
}

/// The decoded payloads of two cooks of one source hold the same mesh: every vertex stream byte for
/// byte, every triangle with the same vertices in the same winding (meshopt's index codec may rotate
/// a triangle's vertices). Non-triangle index ranges compare byte for byte.
bool same_mesh(mesh::MeshView const& v, Span<u8 const> a, Span<u8 const> b) {
    for (u32 li = 0; li < v.lods().size(); ++li) {
        mesh::MeshLod const l         = v.lods()[li];
        mesh::VertexLayout const& lay = v.layouts()[l.layout];
        for (u32 s = 0; s < lay.streamCount; ++s) {
            usize const off = l.streamOffset[s], n = usize(l.vertexCount) * lay.strides[s];
            if (!corpus::bytes_equal(a.subspan(off, n), b.subspan(off, n))) return false;
        }
        usize const isz = mesh::index_size(mesh::IndexType(l.indexType));
        auto index      = [&](Span<u8 const> p, u32 k) {
            u8 const* q = p.data + l.indexOffset + usize(k) * isz;
            u32 x       = 0;
            std::memcpy(&x, q, isz); // little-endian, as kiln targets
            return x;
        };
        bool const triangles = l.indexCount % 3 == 0;
        for (u32 t = 0; t + 2 < l.indexCount + (triangles ? 0 : 2); t += triangles ? 3 : 1) {
            if (!triangles) {
                if (index(a, t) != index(b, t)) return false;
                continue;
            }
            u32 const x[3] = {index(a, t), index(a, t + 1), index(a, t + 2)};
            u32 const y[3] = {index(b, t), index(b, t + 1), index(b, t + 2)};
            bool rotation  = false;
            for (u32 r = 0; r < 3; ++r)
                rotation = rotation || (x[0] == y[r] && x[1] == y[(r + 1) % 3] && x[2] == y[(r + 2) % 3]);
            if (!rotation) return false;
        }
    }
    return true;
}

// Every compression scheme decodes to the mesh an uncompressed cook gives (Basic byte for byte, the
// meshopt schemes up to triangle rotation), in the default and float profiles; never a larger file.
KILN_TEST(MeshCook, CompressionRoundTrip) {
    static char const* const kFiles[] = {
        "generated/cube_basic.glb",       "generated/hierarchy_parts.glb",
        "generated/multi_material.glb",   "generated/pbr_textures.glb",
        "generated/two_uv_sets.glb",      "generated/u32_indices.glb",
        "generated/authored_lods.glb",    "generated/non_triangle.glb",
        "generated/no_uv_no_normals.glb", "khronos/Box.glb",
        "khronos/BoxTextured.glb",        "khronos/BoxVertexColors.glb",
        "khronos/MultiUVTest.glb",
    };
    static cook::CompressionScheme const kSchemes[] = {cook::CompressionScheme::Basic,
                                                       cook::CompressionScheme::Meshopt,
                                                       cook::CompressionScheme::MeshoptZstd};
    for (cook::VertexProfile profile : {cook::VertexProfile::Default, cook::VertexProfile::Float}) {
        for (char const* file : kFiles) {
            char path[1024];
            format(path, sizeof path, "%s/../gltf/%s", kiln::test::corpus_dir(), file);
            Vec<u8> bytes(default_allocator(), Tag::Test);
            if (!KILN_CHECK_MSG(corpus::read_file(path, bytes), "cannot read %s", path)) continue;
            cook::MeshCookSettings s = default_settings();
            s.profile                = profile;
            Diags d;
            Result<cook::CookedMesh> plain = cook_bytes(bytes.span(), "test/mesh.glb", d, s);
            if (!KILN_CHECK_MSG(plain.ok(), "%s: %s", file, d.msg)) continue;
            Opened p;
            open_cooked(*plain, p, file);
            if (!p.ok) continue;
            for (cook::CompressionScheme scheme : kSchemes) {
                cook::MeshCookSettings sc = s;
                sc.compression            = scheme;
                sc = cook::resolve_mesh(sc, cook::TargetProfile{}, cook::CookSession{}).value();
                Result<cook::CookedMesh> packed = cook_bytes(bytes.span(), "test/mesh.glb", d, sc);
                if (!KILN_CHECK_MSG(packed.ok(), "%s scheme %u: %s", file, u32(scheme), d.msg)) continue;
                Opened c;
                open_cooked(*packed, c, file);
                if (!c.ok) continue;
                bool const exact = scheme == cook::CompressionScheme::Basic;
                KILN_CHECK_MSG(exact ? corpus::bytes_equal(c.payload.span(), p.payload.span())
                                     : same_mesh(p.view, p.payload.span(), c.payload.span()),
                               "%s scheme %u: the payload differs", file, u32(scheme));
                KILN_CHECK_MSG(packed->file.size() <= plain->file.size(), "%s scheme %u: %llu > %llu bytes",
                               file, u32(scheme), static_cast<unsigned long long>(packed->file.size()),
                               static_cast<unsigned long long>(plain->file.size()));
            }
        }
    }
}
