// mesh_cook.cpp — cook_mesh: glTF/GLB -> .mesh.
// Determinism: iterate in file / traversal order only; float math is limited to
// + - * /, std::sqrt and std::floor.
#include "cook_internal.h"

#include "kiln/cook/mesh_writer.h"
#include "kiln/log.h"
#include "parallel.h"

#include <cmath>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "mikktspace.h"
#include <meshoptimizer.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace kiln::cook {

using namespace detail;
using mesh::Bounds;

namespace {

// Working vertex: every attribute as f32, absent channels zero. Exact-match
// welding compares these bytes, so the struct must have no padding.
struct Vtx {
    f32 p[3];
    f32 n[3];
    f32 t[4];
    f32 uv[kMaxUvSets][2];
    f32 c[4];
};
static_assert(sizeof(Vtx) == 18 * sizeof(f32));

struct SubRange {
    u32 materialKey; ///< glTF material index or kDefaultMaterial
    u32 first;
    u32 count;
};

struct LodGeom {
    Vec<Vtx> verts;
    Vec<u32> idx;
    Vec<SubRange> subs;
    bool hasUv[kMaxUvSets] = {false, false};
    bool hasColor          = false;
    bool hasTangent        = false;
    f32 mn[3]              = {0, 0, 0};
    f32 mx[3]              = {0, 0, 0};
    u32 lodIndex           = 0;
};

struct PartFormat {
    bool floatPos            = false;
    bool floatDirs           = false; ///< float normal and tangent (VertexProfile::Float)
    bool floatUv[kMaxUvSets] = {false, false};
    f32 posScale[3]          = {1, 1, 1};
    f32 posBias[3]           = {0, 0, 0};
};

/// What the plan decided for one (part, LOD) task, mirroring the sequential walk.
enum class Fate : u8 {
    Skipped,   ///< not reached: after an empty LOD0 or a failed LOD
    Failed,    ///< LOD too large; the cook fails here
    EmptyPart, ///< LOD0 has no triangles; the part keeps no LODs
    Dropped,   ///< a later LOD has no triangles
    Kept,
};

/// One (part, LOD) of the compute phase. A task reads the import data, the settings and
/// the allocator and writes only itself: no Cook state, no shared arena, no diagnostics.
/// The diagnostics the sequential cook emitted from build_lod are recorded here and the
/// merge emits them in traversal order.
struct LodTask {
    ImportPart const* part = nullptr;
    ImportLod const* lod   = nullptr;
    LodGeom g;
    Arena arena; ///< stream, index and bounds output; must live until mesh::write

    // build_lod
    bool tooLarge          = false; ///< kDiagGltfLimit error, reported with totalV / totalI
    u64 totalV             = 0;
    u64 totalI             = 0;
    bool noteNoNormals     = false;
    bool noteNoTangentUv   = false;
    bool noteMikkFailed    = false;
    bool bigUv[kMaxUvSets] = {false, false}; ///< TEXCOORD_n outside +-2048
    Bounds whole           = {};             ///< every triangle; the part bounds (first LOD only)

    // plan
    Fate fate = Fate::Skipped;
    PartFormat pf;

    // quantize_lod
    mesh::VertexLayout layout = {}; ///< not interned yet
    Span<u8 const> streams[2];
    Span<u8 const> indices;
    u32 vertexCount           = 0;
    u32 indexCount            = 0;
    mesh::IndexType indexType = mesh::IndexType::U16;
    Span<Bounds const> subBounds; ///< one per non-empty SubRange, in order

    u64 buildUs = 0, tangentsUs = 0, optimizeUs = 0, packUs = 0;
};

struct TexRefBuild {
    StrView path; ///< "<asset>#<name>" (arena), or the URI of an external image
    StrView mime;
    Span<u8 const> embedded;
    SlotHint slot = SlotHint::None;
    bool srgb     = false;
    bool external = false;
};

struct Cook {
    Cook(MeshSource const& s, MeshCookSettings const& st, Allocator const* al, DiagSink const* d, Arena& a,
         ImportScene const& sc) noexcept
        : src(s), settings(st), alloc(al), diag(d), arena(a), scene(sc), asset(diag_asset(s)),
          layouts(al, Tag::Cook), parts(al, Tag::Cook), lods(al, Tag::Cook), submeshes(al, Tag::Cook),
          materials(al, Tag::Cook), bindings(al, Tag::Cook), mounts(al, Tag::Cook), matMap(al, Tag::Cook),
          vertexColor(al, Tag::Cook), imageRef(al, Tag::Cook), imageName(al, Tag::Cook), refs(al, Tag::Cook),
          tasks(al, Tag::Cook) {}

    MeshSource const& src;
    MeshCookSettings const& settings;
    Allocator const* alloc;
    DiagSink const* diag;
    Arena& arena;
    ImportScene const& scene;
    StrView asset;

    Vec<mesh::VertexLayout> layouts;
    Vec<mesh::PartDesc> parts;
    Vec<mesh::LodDesc> lods;
    Vec<mesh::Submesh> submeshes;
    Vec<mesh::MaterialDesc> materials;
    Vec<mesh::TextureBindingDesc> bindings;
    Vec<mesh::MountDesc> mounts;

    Vec<u32> matMap; ///< glTF material index -> MATL index
    u32 defaultMat = kInvalid;
    Vec<u8> vertexColor;    ///< per glTF material, +1 slot at the end for the default material
    Vec<u32> imageRef;      ///< glTF image -> refs index
    Vec<StrView> imageName; ///< glTF image -> sub-asset name; empty for external images
    Vec<TexRefBuild> refs;
    Vec<LodTask> tasks; ///< every (part, LOD) in traversal order; LodDesc streams point into them

    u64 triangles = 0;
    u64 vertices  = 0;

    // Per-stage timing summed over every (part, LOD) task; see CookStats.
    u64 buildUs = 0, tangentsUs = 0, optimizeUs = 0, packUs = 0;
};

#define COOK_FAIL(k, errCode, diagCode, where, ...)                                                          \
    return diagf((k).diag, make_status(errCode), diagCode, Severity::Error, (k).asset, where, __VA_ARGS__)
#define COOK_NOTE(k, sev, diagCode, where, ...)                                                              \
    (void)diagf((k).diag, kOk, diagCode, sev, (k).asset, where, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Encoding helpers (spec §6)
// ---------------------------------------------------------------------------

/// IEEE binary16, round to nearest even (F. Giesen's float_to_half_fast3_rtne).
[[nodiscard]] u16 f32_to_f16(f32 f) noexcept {
    u32 const f32infty    = 255u << 23;
    u32 const f16max      = (127u + 16u) << 23;
    u32 const denormMagic = ((127u - 15u) + (23u - 10u) + 1u) << 23;
    u32 x                 = std::bit_cast<u32>(f);
    u32 const sign        = x & 0x80000000u;
    x ^= sign;
    u16 o;
    if (x >= f16max) {
        o = x > f32infty ? u16(0x7E00) : u16(0x7C00); // NaN -> qNaN, else Inf
    } else if (x < (113u << 23)) {
        // (sub)normal half: let the FPU round by adding a magic denormal
        f32 const r = std::bit_cast<f32>(x) + std::bit_cast<f32>(denormMagic);
        o           = u16(std::bit_cast<u32>(r) - denormMagic);
    } else {
        u32 const mantOdd = (x >> 13) & 1u;
        x += (u32(15 - 127) << 23) + 0xFFFu; // unsigned wrap is intended: rebias exponent
        x += mantOdd;
        o = u16(x >> 13);
    }
    return u16(o | u16(sign >> 16));
}

[[nodiscard]] i16 snorm16(f32 v) noexcept {
    v = clamp(v, -1.0f, 1.0f);
    return i16(std::floor(v * 32767.0f + 0.5f));
}
[[nodiscard]] u16 unorm16(f32 v) noexcept {
    v = clamp(v, 0.0f, 1.0f);
    return u16(std::floor(v * 65535.0f + 0.5f));
}
[[nodiscard]] u8 unorm8(f32 v) noexcept {
    v = clamp(v, 0.0f, 1.0f);
    return u8(std::floor(v * 255.0f + 0.5f));
}

/// Octahedral encoding of a unit normal into two snorm16 values.
void oct_encode(f32 const n[3], i16 out[2]) noexcept {
    f32 const ax = n[0] < 0 ? -n[0] : n[0];
    f32 const ay = n[1] < 0 ? -n[1] : n[1];
    f32 const az = n[2] < 0 ? -n[2] : n[2];
    f32 const s  = ax + ay + az;
    f32 x = 0, y = 0;
    if (s > 0) {
        x = n[0] / s;
        y = n[1] / s;
        if (n[2] < 0) {
            f32 const ox = (1.0f - (y < 0 ? -y : y)) * (x >= 0 ? 1.0f : -1.0f);
            f32 const oy = (1.0f - (x < 0 ? -x : x)) * (y >= 0 ? 1.0f : -1.0f);
            x            = ox;
            y            = oy;
        }
    }
    out[0] = snorm16(x);
    out[1] = snorm16(y);
}

void normalize3(f32 v[3]) noexcept {
    f32 const len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!(len2 > 0)) return;
    f32 const inv = 1.0f / std::sqrt(len2);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

void mul3(Mat3 const& m, f32 const in[3], f32 out[3]) noexcept {
    for (u32 r = 0; r < 3; ++r)
        out[r] = m.m[0 + r] * in[0] + m.m[3 + r] * in[1] + m.m[6 + r] * in[2];
}

void mul_point(Mat4 const& m, f32 const in[3], f32 out[3]) noexcept {
    for (u32 r = 0; r < 3; ++r)
        out[r] = m.m[0 + r] * in[0] + m.m[4 + r] * in[1] + m.m[8 + r] * in[2] + m.m[12 + r];
}

[[nodiscard]] Bounds make_bounds(f32 const mn[3], f32 const mx[3], f32 radius) noexcept {
    Bounds b{};
    for (u32 a = 0; a < 3; ++a) {
        b.center[a]      = (mn[a] + mx[a]) * 0.5f;
        b.halfExtents[a] = (mx[a] - mn[a]) * 0.5f;
    }
    b.radius = radius;
    return b;
}

/// AABB + bounding sphere (around the AABB center) of the vertices referenced by idx[first, first+count).
[[nodiscard]] Bounds bounds_of(Vtx const* v, u32 const* idx, u32 count) noexcept {
    if (count == 0) return Bounds{};
    f32 mn[3], mx[3];
    for (u32 a = 0; a < 3; ++a)
        mn[a] = mx[a] = v[idx[0]].p[a];
    for (u32 i = 1; i < count; ++i)
        for (u32 a = 0; a < 3; ++a) {
            f32 const x = v[idx[i]].p[a];
            mn[a]       = min(mn[a], x);
            mx[a]       = max(mx[a], x);
        }
    Bounds b = make_bounds(mn, mx, 0);
    f32 r2   = 0;
    for (u32 i = 0; i < count; ++i) {
        f32 const dx = v[idx[i]].p[0] - b.center[0];
        f32 const dy = v[idx[i]].p[1] - b.center[1];
        f32 const dz = v[idx[i]].p[2] - b.center[2];
        r2           = max(r2, dx * dx + dy * dy + dz * dz);
    }
    b.radius = std::sqrt(r2);
    return b;
}

// ---------------------------------------------------------------------------
// MikkTSpace glue (over an unindexed triangle list)
// ---------------------------------------------------------------------------

struct MikkUser {
    Vtx* v;
    int faces;
};
Vtx& mikk_vtx(SMikkTSpaceContext const* ctx, int face, int vert) {
    MikkUser* u = static_cast<MikkUser*>(ctx->m_pUserData);
    return u->v[usize(face) * 3 + usize(vert)];
}
int mikk_num_faces(SMikkTSpaceContext const* ctx) { return static_cast<MikkUser*>(ctx->m_pUserData)->faces; }
int mikk_num_verts(SMikkTSpaceContext const*, int) { return 3; }
void mikk_position(SMikkTSpaceContext const* ctx, float out[], int face, int vert) {
    Vtx const& x = mikk_vtx(ctx, face, vert);
    out[0]       = x.p[0];
    out[1]       = x.p[1];
    out[2]       = x.p[2];
}
void mikk_normal(SMikkTSpaceContext const* ctx, float out[], int face, int vert) {
    Vtx const& x = mikk_vtx(ctx, face, vert);
    out[0]       = x.n[0];
    out[1]       = x.n[1];
    out[2]       = x.n[2];
}
void mikk_texcoord(SMikkTSpaceContext const* ctx, float out[], int face, int vert) {
    Vtx const& x = mikk_vtx(ctx, face, vert);
    out[0]       = x.uv[0][0];
    out[1]       = x.uv[0][1];
}
void mikk_set_basic(SMikkTSpaceContext const* ctx, float const t[], float sign, int face, int vert) {
    Vtx& x = mikk_vtx(ctx, face, vert);
    x.t[0] = t[0];
    x.t[1] = t[1];
    x.t[2] = t[2];
    x.t[3] = sign < 0 ? -1.0f : 1.0f;
}

// ---------------------------------------------------------------------------
// Geometry per LOD
// ---------------------------------------------------------------------------

/// -0.0 -> +0.0 on every channel so exact welding does not split on the sign of zero.
void canonicalize(Vtx& v) noexcept {
    f32* f = &v.p[0];
    for (u32 i = 0; i < sizeof(Vtx) / sizeof(f32); ++i)
        if (f[i] == 0.0f) f[i] = 0.0f;
}

void weld(Allocator const* alloc, LodGeom& g) {
    usize const vc = g.verts.size();
    if (vc == 0 || g.idx.empty()) return;
    Vec<u32> remap(alloc, Tag::Cook);
    remap.resize(vc);
    usize const unique = meshopt_generateVertexRemap(remap.data(), g.idx.data(), g.idx.size(), g.verts.data(),
                                                     vc, sizeof(Vtx));
    Vec<Vtx> out(alloc, Tag::Cook);
    out.resize(unique);
    meshopt_remapVertexBuffer(out.data(), g.verts.data(), vc, sizeof(Vtx), remap.data());
    meshopt_remapIndexBuffer(g.idx.data(), g.idx.data(), g.idx.size(), remap.data());
    g.verts = std::move(out);
}

/// Area-weighted smooth normals for the vertices flagged in `need`, accumulated per
/// welded position.
void generate_normals(Allocator const* alloc, LodGeom& g, Vec<u8> const& need) {
    usize const vc = g.verts.size();
    Vec<f32> pos(alloc, Tag::Cook);
    pos.resize(vc * 3);
    for (usize i = 0; i < vc; ++i)
        for (u32 a = 0; a < 3; ++a)
            pos[i * 3 + a] = g.verts[i].p[a] + 0.0f;
    Vec<u32> remap(alloc, Tag::Cook);
    remap.resize(vc);
    usize const unique = meshopt_generateVertexRemap(remap.data(), g.idx.data(), g.idx.size(), pos.data(), vc,
                                                     sizeof(f32) * 3);
    Vec<f32> acc(alloc, Tag::Cook);
    acc.resize(unique * 3, 0.0f);
    for (usize t = 0; t + 2 < g.idx.size(); t += 3) {
        u32 const a = g.idx[t], b = g.idx[t + 1], c = g.idx[t + 2];
        if (!need[a] && !need[b] && !need[c]) continue;
        f32 const* pa        = g.verts[a].p;
        f32 const* pb        = g.verts[b].p;
        f32 const* pc        = g.verts[c].p;
        f32 const e1[3]      = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        f32 const e2[3]      = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        f32 const fn[3]      = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]};
        u32 const corners[3] = {a, b, c};
        for (u32 cc : corners) {
            u32 const w = remap[cc];
            for (u32 q = 0; q < 3; ++q)
                acc[usize(w) * 3 + q] += fn[q];
        }
    }
    for (usize i = 0; i < vc; ++i) {
        if (!need[i]) continue;
        Vtx& v = g.verts[i];
        v.n[0] = 0;
        v.n[1] = 0;
        v.n[2] = 1;
        if (remap[i] == ~0u) continue;
        f32 n[3] = {acc[usize(remap[i]) * 3], acc[usize(remap[i]) * 3 + 1], acc[usize(remap[i]) * 3 + 2]};
        if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0) {
            normalize3(n);
            v.n[0] = n[0];
            v.n[1] = n[1];
            v.n[2] = n[2];
        }
    }
}

/// Compute phase, part 1: expand/bake, normals, tangents, weld, optimize, bounds.
void build_lod(LodTask& task, MeshCookSettings const& settings, Allocator const* alloc) {
    ImportPart const& part = *task.part;
    ImportLod const& lod   = *task.lod;
    LodGeom& g             = task.g;
    Stopwatch swBuild;
    g.lodIndex = lod.lodIndex;
    // Distinct materials in first-appearance order: one submesh each.
    Vec<u32> order(alloc, Tag::Cook);
    u64 totalV = 0, totalI = 0;
    bool missingNormals = false;
    for (ImportPrim const& p : lod.prims) {
        bool seen = false;
        for (u32 m : order)
            seen |= m == p.material;
        if (!seen) order.push_back(p.material);
        totalV += p.vertexCount;
        totalI += p.indexCount;
        for (u32 s = 0; s < kMaxUvSets; ++s)
            g.hasUv[s] |= p.uvs[s] != nullptr;
        g.hasColor |= p.colors != nullptr;
        missingNormals |= p.normals == nullptr;
    }
    task.totalV = totalV;
    task.totalI = totalI;
    if (totalV >= kInvalid || totalI >= kInvalid) {
        task.tooLarge = true;
        return;
    }

    g.verts.reserve(usize(totalV));
    g.idx.reserve(usize(totalI));
    Vec<u8> need(alloc, Tag::Cook);
    if (missingNormals) need.reserve(usize(totalV));

    for (u32 key : order) {
        u32 const first = u32(g.idx.size());
        for (ImportPrim const& p : lod.prims) {
            if (p.material != key) continue;
            u32 const base = u32(g.verts.size());
            for (u32 i = 0; i < p.vertexCount; ++i) {
                Vtx v{};
                f32 const* ps = p.positions + usize(i) * 3;
                if (part.bakeIdentity) {
                    v.p[0] = ps[0];
                    v.p[1] = ps[1];
                    v.p[2] = ps[2];
                } else {
                    mul3(part.bake, ps, v.p);
                }
                if (p.normals) {
                    f32 const* ns = p.normals + usize(i) * 3;
                    if (part.bakeIdentity) {
                        v.n[0] = ns[0];
                        v.n[1] = ns[1];
                        v.n[2] = ns[2];
                    } else {
                        mul3(part.bakeNormal, ns, v.n);
                    }
                    normalize3(v.n);
                }
                for (u32 s = 0; s < kMaxUvSets; ++s)
                    if (p.uvs[s]) {
                        v.uv[s][0] = p.uvs[s][usize(i) * 2];
                        v.uv[s][1] = p.uvs[s][usize(i) * 2 + 1];
                    }
                if (p.colors) {
                    for (u32 q = 0; q < 4; ++q)
                        v.c[q] = p.colors[usize(i) * 4 + q];
                } else {
                    v.c[0] = v.c[1] = v.c[2] = v.c[3] = 1.0f;
                }
                g.verts.push_back(v);
                if (missingNormals) need.push_back(p.normals ? 0 : 1);
            }
            for (u32 t = 0; t + 2 < p.indexCount; t += 3) {
                u32 const a = p.indices[t], b = p.indices[t + 1], c = p.indices[t + 2];
                g.idx.push_back(base + a);
                g.idx.push_back(base + (part.flipWinding ? c : b));
                g.idx.push_back(base + (part.flipWinding ? b : c));
            }
        }
        g.subs.push_back(SubRange{key, first, u32(g.idx.size()) - first});
    }

    if (missingNormals) {
        task.noteNoNormals = true;
        generate_normals(alloc, g, need);
    }
    for (Vtx& v : g.verts)
        canonicalize(v);
    task.buildUs += swBuild.elapsed_us();

    Stopwatch const swTangents;
    if (settings.genTangents) {
        if (!g.hasUv[0]) {
            task.noteNoTangentUv = true;
        } else if (!g.idx.empty()) {
            // Unindexed triangle list -> MikkTSpace -> re-weld (below).
            Vec<Vtx> flat(alloc, Tag::Cook);
            flat.resize(g.idx.size());
            for (usize i = 0; i < g.idx.size(); ++i)
                flat[i] = g.verts[g.idx[i]];
            MikkUser user{flat.data(), int(g.idx.size() / 3)};
            SMikkTSpaceInterface iface{};
            iface.m_getNumFaces          = &mikk_num_faces;
            iface.m_getNumVerticesOfFace = &mikk_num_verts;
            iface.m_getPosition          = &mikk_position;
            iface.m_getNormal            = &mikk_normal;
            iface.m_getTexCoord          = &mikk_texcoord;
            iface.m_setTSpaceBasic       = &mikk_set_basic;
            SMikkTSpaceContext ctx{&iface, &user};
            if (genTangSpaceDefault(&ctx)) {
                for (Vtx& v : flat)
                    canonicalize(v);
                g.verts = std::move(flat);
                for (usize i = 0; i < g.idx.size(); ++i)
                    g.idx[i] = u32(i);
                g.hasTangent = true;
            } else {
                task.noteMikkFailed = true;
            }
        }
    }
    task.tangentsUs += swTangents.elapsed_us();

    Stopwatch const swWeld;
    weld(alloc, g);
    task.buildUs += swWeld.elapsed_us();

    Stopwatch const swOptimize;
    if (settings.optimize && !g.idx.empty()) {
        u32 const vc = u32(g.verts.size());
        Vec<u32> scratch(alloc, Tag::Cook);
        scratch.resize(g.idx.size());
        for (SubRange const& s : g.subs) {
            if (s.count == 0) continue;
            u32* range = g.idx.data() + s.first;
            meshopt_optimizeVertexCache(scratch.data(), range, s.count, vc);
            meshopt_optimizeOverdraw(range, scratch.data(), s.count, &g.verts[0].p[0], vc, sizeof(Vtx),
                                     1.05f);
        }
        Vec<Vtx> out(alloc, Tag::Cook);
        out.resize(vc);
        usize const used = meshopt_optimizeVertexFetch(out.data(), g.idx.data(), g.idx.size(), g.verts.data(),
                                                       vc, sizeof(Vtx));
        out.resize(used);
        g.verts = std::move(out);
    }
    task.optimizeUs += swOptimize.elapsed_us();

    Stopwatch const swBounds;
    if (!g.verts.empty()) {
        for (u32 a = 0; a < 3; ++a)
            g.mn[a] = g.mx[a] = g.verts[0].p[a];
        for (Vtx const& v : g.verts)
            for (u32 a = 0; a < 3; ++a) {
                g.mn[a] = min(g.mn[a], v.p[a]);
                g.mx[a] = max(g.mx[a], v.p[a]);
            }
    }
    // Inputs of the part's quantization plan and bounds (cook_part scanned these before).
    for (u32 s = 0; s < kMaxUvSets; ++s) {
        if (!g.hasUv[s]) continue;
        for (Vtx const& v : g.verts)
            task.bigUv[s] |= v.uv[s][0] > 2048.0f || v.uv[s][0] < -2048.0f || v.uv[s][1] > 2048.0f ||
                             v.uv[s][1] < -2048.0f;
    }
    if (task.lod == part.lods.data) task.whole = bounds_of(g.verts.data(), g.idx.data(), u32(g.idx.size()));
    task.buildUs += swBounds.elapsed_us();
}

// ---------------------------------------------------------------------------
// Materials and texture references
// ---------------------------------------------------------------------------

[[nodiscard]] SlotHint slot_hint(mesh::TextureSlot s) noexcept {
    switch (s) {
    case mesh::TextureSlot::BaseColor: return SlotHint::BaseColor;
    case mesh::TextureSlot::Normal: return SlotHint::Normal;
    case mesh::TextureSlot::MetalRough: return SlotHint::MetallicRoughness;
    case mesh::TextureSlot::Occlusion: return SlotHint::Occlusion;
    case mesh::TextureSlot::Emissive: return SlotHint::Emissive;
    }
    return SlotHint::None;
}

[[nodiscard]] StrView concat(Arena& arena, StrView a, StrView b, StrView c = {}) noexcept {
    usize const n = a.size + b.size + c.size;
    char* p       = arena.alloc_array<char>(n + 1);
    if (a.size) std::memcpy(p, a.data, a.size);
    if (b.size) std::memcpy(p + a.size, b.data, b.size);
    if (c.size) std::memcpy(p + a.size + b.size, c.data, c.size);
    p[n] = '\0';
    return {p, n};
}

/// Names every embedded image: its glTF name, or "image<N>" when unnamed. The name becomes
/// the sub-asset part of "<asset>#<name>", so it must be unique and give a valid asset name.
/// An external image's URI must resolve to a name in the source's root.
Status name_embedded_images(Cook& k) {
    k.imageName.resize(k.scene.images.size);
    for (u32 i = 0; i < u32(k.scene.images.size); ++i) {
        ImportImage const& img = k.scene.images[i];
        if (!img.uri.empty()) {
            char resolved[kMaxAssetNameLen + 1];
            if (resolve_asset_name(k.src.assetPath, img.uri, resolved, sizeof resolved) == 0)
                COOK_FAIL(k, Code::ValidationFailed, kDiagGltfUriOutsideRoot, img.uri,
                          "image %u: URI '%.*s' is absolute, leaves the root or gives an invalid asset name",
                          i, KILN_SV(img.uri));
            continue;
        }
        if (!img.usable) continue;
        StrView name = img.name;
        if (name.empty()) {
            char buf[32];
            name = k.arena.copy(StrView(buf, format(buf, sizeof buf, "image%u", i)));
        }
        char full[kMaxAssetNameLen + 2];
        usize const n   = format(full, sizeof full, "%.*s#%.*s", KILN_SV(k.src.assetPath), KILN_SV(name));
        char const* why = n > kMaxAssetNameLen ? "longer than 255 bytes" : check_asset_name(StrView(full, n));
        if (why)
            COOK_FAIL(k, Code::ValidationFailed, kDiagGltfImageName, name,
                      "embedded image %u: name '%.*s' does not give a valid texture name (%s)", i,
                      KILN_SV(name), why);
        for (u32 j = 0; j < i; ++j)
            if (k.imageName[j] == name)
                COOK_FAIL(k, Code::ValidationFailed, kDiagGltfImageName, name,
                          "embedded images %u and %u are both named '%.*s'", j, i, KILN_SV(name));
        k.imageName[i] = name;
    }
    return kOk;
}

[[nodiscard]] StrView with_suffix(Arena& arena, StrView base, u32 n) noexcept {
    char buf[16];
    usize const len = format(buf, sizeof buf, "_%u", n);
    return concat(arena, base, StrView(buf, len));
}

u32 texture_ref(Cook& k, u32 image, mesh::TextureSlot slot, StrView where) {
    SlotHint const hint = slot_hint(slot);
    if (k.imageRef[image] != kInvalid) {
        TexRefBuild const& r = k.refs[k.imageRef[image]];
        if (usage_from_slot(r.slot) != usage_from_slot(hint))
            COOK_NOTE(k, Severity::Warning, kDiagGltfUsageConflict, where,
                      "image %u is used as %s and %s; cooked as %s (first use)", image,
                      texture_usage_name(usage_from_slot(r.slot)), texture_usage_name(usage_from_slot(hint)),
                      texture_usage_name(usage_from_slot(r.slot)));
        return k.imageRef[image];
    }
    ImportImage const& img = k.scene.images[image];
    TexRefBuild r;
    r.external = !img.uri.empty();
    if (r.external) {
        r.path = img.uri;
    } else {
        r.path     = concat(k.arena, k.src.assetPath, "#", k.imageName[image]);
        r.mime     = img.mimeType;
        r.embedded = img.bytes;
    }
    r.slot            = hint;
    r.srgb            = color_space_for(usage_from_slot(hint)) == ColorSpace::Srgb;
    k.imageRef[image] = u32(k.refs.size());
    k.refs.push_back(r);
    return k.imageRef[image];
}

/// "name.001" -> "name" (dot followed by 3+ digits at the end).
[[nodiscard]] StrView strip_blender_suffix(StrView name) noexcept {
    usize const dot = name.rfind('.');
    if (dot == StrView::kNpos || dot == 0 || name.size - dot - 1 < 3) return name;
    for (usize i = dot + 1; i < name.size; ++i)
        if (name[i] < '0' || name[i] > '9') return name;
    return name.substr(0, dot);
}

[[nodiscard]] bool same_material(Cook const& k, mesh::MaterialDesc const& a, mesh::MaterialDesc const& b,
                                 Span<mesh::TextureBindingDesc const> bBind) noexcept {
    if (a.flags != b.flags || a.alphaMode != b.alphaMode || a.alphaCutoff != b.alphaCutoff ||
        a.textureCount != bBind.size || a.metallicFactor != b.metallicFactor ||
        a.roughnessFactor != b.roughnessFactor || a.normalScale != b.normalScale ||
        a.occlusionStrength != b.occlusionStrength)
        return false;
    for (u32 i = 0; i < 4; ++i)
        if (a.baseColorFactor[i] != b.baseColorFactor[i] ||
            (i < 3 && a.emissiveFactor[i] != b.emissiveFactor[i]))
            return false;
    for (u32 i = 0; i < a.textureCount; ++i) {
        mesh::TextureBindingDesc const& x = k.bindings[a.textureFirst + i];
        mesh::TextureBindingDesc const& y = bBind[i];
        if (x.path != y.path || x.slot != y.slot || x.uvSet != y.uvSet || x.flags != y.flags) return false;
    }
    return true;
}

u32 material_for(Cook& k, u32 key) {
    if (key == kDefaultMaterial) {
        if (k.defaultMat != kInvalid) return k.defaultMat;
    } else if (k.matMap[key] != kInvalid) {
        return k.matMap[key];
    }

    mesh::MaterialDesc md;
    FixedArray<mesh::TextureBindingDesc, kSlotCount> bind;
    StrView base;
    if (key == kDefaultMaterial) {
        base     = "default";
        md.flags = k.vertexColor[k.scene.materials.size] ? mesh::kMaterialVertexColor : 0u;
    } else {
        ImportMaterial const& im = k.scene.materials[key];
        StrView const where      = im.name;
        if (im.name.empty()) {
            char buf[32];
            base = k.arena.copy(StrView(buf, format(buf, sizeof buf, "material_%u", key)));
        } else {
            base = strip_blender_suffix(im.name);
            if (base.size != im.name.size)
                COOK_NOTE(k, Severity::Info, kDiagGltfMaterialRenamed, where,
                          "material '%.*s' renamed to '%.*s'", KILN_SV(im.name), KILN_SV(base));
        }
        md.flags = (im.doubleSided ? u32(mesh::kMaterialDoubleSided) : 0u) |
                   (k.vertexColor[key] ? u32(mesh::kMaterialVertexColor) : 0u);
        md.alphaMode   = im.alphaMode;
        md.alphaCutoff = im.alphaCutoff;
        std::memcpy(md.baseColorFactor, im.baseColorFactor, sizeof md.baseColorFactor);
        std::memcpy(md.emissiveFactor, im.emissiveFactor, sizeof md.emissiveFactor);
        md.metallicFactor    = im.metallicFactor;
        md.roughnessFactor   = im.roughnessFactor;
        md.normalScale       = im.normalScale;
        md.occlusionStrength = im.occlusionStrength;
        for (u32 s = 0; s < kSlotCount; ++s) {
            ImportTexture const& t = im.slots[s];
            if (!t.present) continue;
            auto const slot = mesh::TextureSlot(s);
            if (t.image == kInvalid || !k.scene.images[t.image].usable) {
                COOK_NOTE(k, Severity::Warning, kDiagGltfImageUnresolvable, where,
                          "%s texture has no usable image source; binding dropped",
                          mesh::texture_slot_name(slot));
                continue;
            }
            u32 const r = texture_ref(k, t.image, slot, where);
            mesh::TextureBindingDesc b;
            b.path  = k.refs[r].path;
            b.slot  = slot;
            b.uvSet = u8(min(t.texcoord, 255u));
            b.flags = u16((k.refs[r].srgb ? mesh::kTextureSrgb : 0) |
                          (k.refs[r].external ? mesh::kTextureExternal : 0));
            bind.push_back(b);
        }
    }

    // Same final name: merge if identical, else suffix _2, _3, ...
    StrView name = base;
    u32 found    = kInvalid;
    for (u32 n = 2;; ++n) {
        u32 same = kInvalid;
        for (u32 i = 0; i < u32(k.materials.size()); ++i)
            if (k.materials[i].name == name) same = i;
        if (same == kInvalid) break;
        if (same_material(k, k.materials[same], md, bind.span())) {
            found = same;
            break;
        }
        name = with_suffix(k.arena, base, n);
    }
    if (found == kInvalid) {
        if (name.size != base.size)
            COOK_NOTE(k, Severity::Warning, kDiagGltfMaterialRenamed, base,
                      "material name '%.*s' already used by a different material; renamed to '%.*s'",
                      KILN_SV(base), KILN_SV(name));
        md.name         = name;
        md.textureFirst = u32(k.bindings.size());
        md.textureCount = u32(bind.size());
        for (mesh::TextureBindingDesc const& b : bind)
            k.bindings.push_back(b);
        found = u32(k.materials.size());
        k.materials.push_back(md);
    }
    if (key == kDefaultMaterial)
        k.defaultMat = found;
    else
        k.matMap[key] = found;
    return found;
}

// ---------------------------------------------------------------------------
// Quantize + pack one LOD
// ---------------------------------------------------------------------------

void add_attrib(mesh::VertexLayout& l, mesh::Semantic sem, u8 semIndex, u8 stream, Format f,
                u16 offset) noexcept {
    mesh::VertexAttrib& a = l.attribs[l.attribCount++];
    a.semantic            = u8(sem);
    a.semanticIndex       = semIndex;
    a.stream              = stream;
    a.format              = u32(f);
    a.offset              = offset;
}

u32 intern_layout(Cook& k, mesh::VertexLayout const& l) {
    for (u32 i = 0; i < u32(k.layouts.size()); ++i)
        if (std::memcmp(&k.layouts[i], &l, sizeof l) == 0) return i;
    k.layouts.push_back(l);
    return u32(k.layouts.size() - 1);
}

/// Compute phase, part 2: quantize into the task arena, build the (not yet interned)
/// layout and the submesh bounds. Frees the working vertices and indices.
void quantize_lod(LodTask& task) {
    Stopwatch const sw;
    LodGeom& g           = task.g;
    PartFormat const& pf = task.pf;
    // Layout: stream 0 = position only; stream 1 = normal, tangent, uv0, uv1, color.
    mesh::VertexLayout l;
    std::memset(&l, 0, sizeof l);
    l.streamCount = 2;
    add_attrib(l, mesh::Semantic::Position, 0, 0,
               pf.floatPos ? Format::R32G32B32_SFLOAT : Format::R16G16B16A16_UNORM, 0);
    l.strides[0]      = pf.floatPos ? 12 : 8;
    u16 o             = 0;
    u16 const oNormal = o;
    add_attrib(l, mesh::Semantic::Normal, 0, 1,
               pf.floatDirs ? Format::R32G32B32_SFLOAT : Format::R16G16_SNORM, o);
    o                  = u16(o + (pf.floatDirs ? 12 : 4));
    u16 const oTangent = o;
    if (g.hasTangent) {
        add_attrib(l, mesh::Semantic::Tangent, 0, 1,
                   pf.floatDirs ? Format::R32G32B32A32_SFLOAT : Format::R16G16B16A16_SNORM, o);
        o = u16(o + (pf.floatDirs ? 16 : 8));
    }
    u16 oUv[kMaxUvSets] = {0, 0};
    for (u32 s = 0; s < kMaxUvSets; ++s) {
        if (!g.hasUv[s]) continue;
        oUv[s] = o;
        add_attrib(l, mesh::Semantic::TexCoord, u8(s), 1,
                   pf.floatUv[s] ? Format::R32G32_SFLOAT : Format::R16G16_SFLOAT, o);
        o = u16(o + (pf.floatUv[s] ? 8 : 4));
    }
    u16 const oColor = o;
    if (g.hasColor) {
        add_attrib(l, mesh::Semantic::Color, 0, 1, Format::R8G8B8A8_UNORM, o);
        o = u16(o + 4);
    }
    l.strides[1] = u16((u32(o) + 3u) & ~3u);

    u32 const vc      = u32(g.verts.size());
    usize const s0    = usize(vc) * l.strides[0];
    usize const s1    = usize(vc) * l.strides[1];
    u8* const stream0 = task.arena.alloc_array<u8>(max(s0, usize(1)));
    u8* const stream1 = task.arena.alloc_array<u8>(max(s1, usize(1)));
    std::memset(stream0, 0, s0);
    std::memset(stream1, 0, s1);

    for (u32 i = 0; i < vc; ++i) {
        Vtx const& v = g.verts[i];
        u8* p0       = stream0 + usize(i) * l.strides[0];
        if (pf.floatPos) {
            std::memcpy(p0, v.p, 12);
        } else {
            u16 q[4] = {0, 0, 0, 0};
            for (u32 a = 0; a < 3; ++a)
                q[a] = unorm16((v.p[a] - pf.posBias[a]) / pf.posScale[a]);
            std::memcpy(p0, q, 8);
        }
        u8* p1 = stream1 + usize(i) * l.strides[1];
        if (pf.floatDirs) {
            std::memcpy(p1 + oNormal, v.n, 12);
            if (g.hasTangent) {
                f32 const t[4] = {v.t[0], v.t[1], v.t[2], v.t[3] < 0 ? -1.0f : 1.0f};
                std::memcpy(p1 + oTangent, t, 16);
            }
        } else {
            i16 oct[2];
            oct_encode(v.n, oct);
            std::memcpy(p1 + oNormal, oct, 4);
            if (g.hasTangent) {
                i16 const t[4] = {snorm16(v.t[0]), snorm16(v.t[1]), snorm16(v.t[2]),
                                  i16(v.t[3] < 0 ? -32767 : 32767)};
                std::memcpy(p1 + oTangent, t, 8);
            }
        }
        for (u32 s = 0; s < kMaxUvSets; ++s) {
            if (!g.hasUv[s]) continue;
            if (pf.floatUv[s]) {
                std::memcpy(p1 + oUv[s], v.uv[s], 8);
            } else {
                u16 const h[2] = {f32_to_f16(v.uv[s][0]), f32_to_f16(v.uv[s][1])};
                std::memcpy(p1 + oUv[s], h, 4);
            }
        }
        if (g.hasColor) {
            u8 const c[4] = {unorm8(v.c[0]), unorm8(v.c[1]), unorm8(v.c[2]), unorm8(v.c[3])};
            std::memcpy(p1 + oColor, c, 4);
        }
    }

    u32 const ic                = u32(g.idx.size());
    mesh::IndexType const itype = vc <= 0xFFFFu ? mesh::IndexType::U16 : mesh::IndexType::U32;
    usize const ibytes          = usize(ic) * mesh::index_size(itype);
    u8* const ib                = task.arena.alloc_array<u8>(max(ibytes, usize(1)));
    for (u32 i = 0; i < ic; ++i) {
        if (itype == mesh::IndexType::U16)
            write_unaligned(ib + usize(i) * 2, u16(g.idx[i]));
        else
            write_unaligned(ib + usize(i) * 4, g.idx[i]);
    }

    std::memcpy(&task.layout, &l, sizeof l); // interning compares bytes
    task.vertexCount = vc;
    task.streams[0]  = {stream0, s0};
    task.streams[1]  = {stream1, s1};
    task.indices     = {ib, ibytes};
    task.indexCount  = ic;
    task.indexType   = itype;

    u32 subCount = 0;
    for (SubRange const& s : g.subs)
        subCount += s.count != 0;
    Bounds* const sb = task.arena.alloc_array<Bounds>(max(subCount, 1u));
    u32 n            = 0;
    for (SubRange const& s : g.subs)
        if (s.count != 0) sb[n++] = bounds_of(g.verts.data(), g.idx.data() + s.first, s.count);
    task.subBounds = {sb, subCount};

    g.verts.release();
    g.idx.release();
    task.packUs += sw.elapsed_us();
}

/// Merge phase for one kept LOD: intern the layout, resolve materials, append the LOD
/// and submesh records. Runs sequentially in traversal order.
void merge_lod(Cook& k, LodTask const& t) {
    Stopwatch const sw;
    mesh::LodDesc ld;
    ld.layout       = intern_layout(k, t.layout);
    ld.vertexCount  = t.vertexCount;
    ld.streams[0]   = t.streams[0];
    ld.streams[1]   = t.streams[1];
    ld.indices      = t.indices;
    ld.indexCount   = t.indexCount;
    ld.indexType    = t.indexType;
    ld.submeshFirst = u32(k.submeshes.size());
    ld.submeshCount = 0;
    for (SubRange const& s : t.g.subs) {
        if (s.count == 0) continue;
        mesh::Submesh sm{};
        sm.material   = material_for(k, s.materialKey);
        sm.indexFirst = s.first;
        sm.indexCount = s.count;
        sm.vertexBase = 0;
        sm.bounds     = t.subBounds[ld.submeshCount];
        k.submeshes.push_back(sm);
        ++ld.submeshCount;
    }
    k.lods.push_back(ld);
    k.triangles += t.indexCount / 3;
    k.vertices += t.vertexCount;
    k.packUs += sw.elapsed_us();
}

// ---------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------

/// Per-part decisions between the two compute rounds: which LODs are kept and the
/// quantization format, plus the notes merge_part emits about them.
struct PartPlan {
    u32 firstTask = 0; ///< into Cook::tasks
    u32 taskCount = 0;
    u32 keptCount = 0;
    PartFormat pf;
    bool posFallback            = false;
    f32 maxExtent               = 0;
    bool uvFallback[kMaxUvSets] = {false, false};
};

/// Walks the part's LODs as the sequential cook did, sets each task's fate and the part
/// format. Emits nothing. Returns false at a failed LOD (the walk stops there).
bool plan_part(MeshCookSettings const& settings, Span<LodTask> tasks, PartPlan& plan) {
    for (LodTask& t : tasks) {
        if (t.tooLarge) {
            t.fate = Fate::Failed;
            return false;
        }
        if (t.g.idx.empty()) {
            if (plan.keptCount == 0) {
                t.fate = Fate::EmptyPart; // LOD0 empty: the part keeps no LODs at all
                break;
            }
            t.fate = Fate::Dropped;
            continue;
        }
        t.fate = Fate::Kept;
        ++plan.keptCount;
    }
    if (plan.keptCount == 0) return true;

    // Quantization box: union of every LOD's AABB (== LOD0's unless coarser LODs stick out).
    f32 mn[3], mx[3];
    for (u32 a = 0; a < 3; ++a) {
        mn[a] = tasks[0].g.mn[a];
        mx[a] = tasks[0].g.mx[a];
    }
    for (LodTask const& t : tasks) {
        if (t.fate != Fate::Kept) continue;
        for (u32 a = 0; a < 3; ++a) {
            mn[a] = min(mn[a], t.g.mn[a]);
            mx[a] = max(mx[a], t.g.mx[a]);
        }
    }
    f32 maxExtent = 0;
    for (u32 a = 0; a < 3; ++a)
        maxExtent = max(maxExtent, mx[a] - mn[a]);

    PartFormat& pf     = plan.pf;
    bool const precise = settings.profile != VertexProfile::Default; // Precise or Float
    pf.floatPos        = precise;
    pf.floatDirs       = settings.profile == VertexProfile::Float;
    if (!precise && maxExtent / 65535.0f > settings.posTolMm / 1000.0f) {
        pf.floatPos      = true;
        plan.posFallback = true;
        plan.maxExtent   = maxExtent;
    }
    if (!pf.floatPos)
        for (u32 a = 0; a < 3; ++a) {
            f32 const e    = mx[a] - mn[a];
            pf.posScale[a] = e > 0 ? e : 1.0f;
            pf.posBias[a]  = mn[a];
        }
    for (u32 s = 0; s < kMaxUvSets; ++s) {
        pf.floatUv[s] = precise;
        if (precise) continue;
        bool big = false;
        for (LodTask const& t : tasks)
            big |= t.fate == Fate::Kept && t.bigUv[s];
        if (big) {
            pf.floatUv[s]      = true;
            plan.uvFallback[s] = true;
        }
    }
    for (LodTask& t : tasks)
        if (t.fate == Fate::Kept) t.pf = pf;
    return true;
}

/// Merge phase for one part, in traversal order: emits the part's diagnostics in the
/// order the sequential cook did, then appends its LODs and the part record.
Status merge_part(Cook& k, ImportPart const& part, PartPlan const& plan, f32 modelMin[3], f32 modelMax[3],
                  bool& anyModel, Vec<Bounds>& worldSpheres) {
    mesh::PartDesc pd;
    pd.name   = part.name;
    pd.parent = part.parent;
    for (u32 a = 0; a < 3; ++a)
        pd.translation[a] = part.translation[a];
    for (u32 a = 0; a < 4; ++a)
        pd.rotation[a] = part.rotation[a];
    pd.lodFirst = u32(k.lods.size());
    pd.lodCount = 0;

    Span<LodTask const> const tasks(k.tasks.data() + plan.firstTask, plan.taskCount);
    for (LodTask const& t : tasks) { // every task that ran, also those the walk below skips
        k.buildUs += t.buildUs;
        k.tangentsUs += t.tangentsUs;
        k.optimizeUs += t.optimizeUs;
        k.packUs += t.packUs;
    }

    // build_lod's diagnostics, LOD by LOD; the missing-normals and missing-UV notes once per part.
    bool notedNormals = false, notedTangentUv = false;
    for (LodTask const& t : tasks) {
        if (t.fate == Fate::Skipped) break;
        ImportLod const& lod = *t.lod;
        if (t.fate == Fate::Failed)
            COOK_FAIL(k, Code::Unsupported, kDiagGltfLimit, lod.nodeName,
                      "LOD too large (%llu vertices, %llu indices)",
                      static_cast<unsigned long long>(t.totalV), static_cast<unsigned long long>(t.totalI));
        if (t.noteNoNormals && !notedNormals)
            COOK_NOTE(k, Severity::Info, kDiagGltfNoNormals, lod.nodeName,
                      "normals missing; generated smooth normals");
        notedNormals |= t.noteNoNormals;
        if (t.noteNoTangentUv && !notedTangentUv)
            COOK_NOTE(k, Severity::Warning, kDiagGltfNoTangentSource, lod.nodeName,
                      "tangents requested but TEXCOORD_0 is missing; no tangents");
        notedTangentUv |= t.noteNoTangentUv;
        if (t.noteMikkFailed)
            COOK_NOTE(k, Severity::Warning, kDiagGltfNoTangentSource, lod.nodeName,
                      "MikkTSpace failed; no tangents");
        if (t.fate == Fate::EmptyPart) {
            COOK_NOTE(k, Severity::Warning, kDiagGltfEmptyMesh, part.name,
                      "part has no triangles; kept as a hierarchy node without LODs");
            break;
        }
        if (t.fate == Fate::Dropped)
            COOK_NOTE(k, Severity::Warning, kDiagGltfEmptyMesh, lod.nodeName,
                      "LOD %u has no triangles; dropped", lod.lodIndex);
    }

    if (plan.keptCount != 0) {
        if (plan.posFallback)
            COOK_NOTE(k, Severity::Info, kDiagGltfQuantFallback, part.name,
                      "extent %g m exceeds 16-bit precision at %g mm; float positions", f64(plan.maxExtent),
                      f64(k.settings.posTolMm));
        for (u32 s = 0; s < kMaxUvSets; ++s)
            if (plan.uvFallback[s])
                COOK_NOTE(k, Severity::Info, kDiagGltfQuantFallback, part.name,
                          "TEXCOORD_%u outside +-2048; float UVs", s);

        pd.bounds = tasks[0].whole;
        for (LodTask const& t : tasks)
            if (t.fate == Fate::Kept) merge_lod(k, t);
        pd.lodCount = plan.keptCount;

        // Model bounds: part AABB corners through the part's model-space frame.
        Bounds const& b = pd.bounds;
        for (u32 c = 0; c < 8; ++c) {
            f32 const corner[3] = {b.center[0] + ((c & 1) ? b.halfExtents[0] : -b.halfExtents[0]),
                                   b.center[1] + ((c & 2) ? b.halfExtents[1] : -b.halfExtents[1]),
                                   b.center[2] + ((c & 4) ? b.halfExtents[2] : -b.halfExtents[2])};
            f32 w[3];
            mul_point(part.frame, corner, w);
            for (u32 a = 0; a < 3; ++a) {
                modelMin[a] = anyModel ? min(modelMin[a], w[a]) : w[a];
                modelMax[a] = anyModel ? max(modelMax[a], w[a]) : w[a];
            }
            anyModel = true;
        }
        Bounds ws{};
        mul_point(part.frame, b.center, ws.center);
        ws.radius = b.radius;
        worldSpheres.push_back(ws);
    }
    for (u32 a = 0; a < 3; ++a) {
        pd.posScale[a] = plan.pf.posScale[a];
        pd.posBias[a]  = plan.pf.posBias[a];
    }
    k.parts.push_back(pd);
    return kOk;
}

/// parallel_for payload for the two compute rounds.
struct TaskRun {
    LodTask* tasks                   = nullptr;
    MeshCookSettings const* settings = nullptr;
    Allocator const* alloc           = nullptr;
    ProfileHooks const* profile      = nullptr;
    StrView asset;
};

void build_tasks(void* user, u32 begin, u32 end) noexcept {
    TaskRun const& r = *static_cast<TaskRun const*>(user);
    ProfileZone const zone(r.profile, "cook.build", r.asset);
    for (u32 i = begin; i < end; ++i)
        build_lod(r.tasks[i], *r.settings, r.alloc);
}

void quantize_tasks(void* user, u32 begin, u32 end) noexcept {
    TaskRun const& r = *static_cast<TaskRun const*>(user);
    ProfileZone const zone(r.profile, "cook.pack", r.asset);
    for (u32 i = begin; i < end; ++i)
        if (r.tasks[i].fate == Fate::Kept) quantize_lod(r.tasks[i]);
}

[[nodiscard]] StrView last_component(StrView path) noexcept {
    usize const slash = path.rfind('/');
    return slash == StrView::kNpos ? path : path.substr(slash + 1);
}

} // namespace

Result<CookedMesh> cook_mesh(MeshSource const& src, MeshCookSettings const& settings,
                             TargetProfile const& target, CookEnv const& env) noexcept {
    Allocator const* const alloc = env.alloc ? env.alloc : default_allocator();
    DiagSink const* const diag   = env.diag;
    Stage swTotal(env.profile, "cook.mesh", src.assetPath);
    Arena arena(Arena::Desc{alloc, usize(1) << 20, Tag::Cook});

    if (char const* why = check_asset_name(src.assetPath))
        return diagf(diag, make_status(Code::InvalidArgument), 0, Severity::Error, src.assetPath, "cook_mesh",
                     "MeshSource::assetPath is not a valid asset name: %s", why);

    ImportScene scene;
    Stage swImport(env.profile, "cook.import", src.assetPath);
    KILN_TRY(import_gltf(src, settings, arena, alloc, diag, scene));
    u64 const importUs = swImport.stop();

    Cook k(src, settings, alloc, diag, arena, scene);
    k.matMap.resize(scene.materials.size, kInvalid);
    k.vertexColor.resize(scene.materials.size + 1, 0);
    k.imageRef.resize(scene.images.size, kInvalid);
    KILN_TRY(name_embedded_images(k));

    // kMaterialVertexColor: any cooked primitive using the material has COLOR_0.
    for (ImportPart const& p : scene.parts)
        for (ImportLod const& l : p.lods)
            for (ImportPrim const& pr : l.prims)
                if (pr.colors)
                    k.vertexColor[pr.material == kDefaultMaterial ? scene.materials.size : pr.material] = 1;

    // One task per (part, LOD) in traversal order: build (parallel), plan each part
    // (sequential), quantize (parallel), merge (sequential, traversal order).
    Vec<PartPlan> plans(alloc, Tag::Cook);
    plans.resize(scene.parts.size);
    u32 taskCount = 0;
    for (u32 i = 0; i < u32(scene.parts.size); ++i) {
        plans[i].firstTask = taskCount;
        plans[i].taskCount = u32(scene.parts[i].lods.size);
        taskCount += plans[i].taskCount;
    }
    k.tasks.resize(taskCount);
    for (u32 i = 0; i < u32(scene.parts.size); ++i)
        for (u32 j = 0; j < plans[i].taskCount; ++j) {
            LodTask& t = k.tasks[plans[i].firstTask + j];
            t.part     = &scene.parts[i];
            t.lod      = &scene.parts[i].lods[j];
            t.g.verts.init(alloc, Tag::Cook);
            t.g.idx.init(alloc, Tag::Cook);
            t.g.subs.init(alloc, Tag::Cook);
            t.arena.init(Arena::Desc{alloc, usize(16) << 10, Tag::Cook});
        }

    // Thread safety: meshoptimizer is pure and reentrant, MikkTSpace is reentrant per context, cgltf is done.
    TaskRun run{k.tasks.data(), &settings, alloc, env.profile, src.assetPath};
    parallel_for(env.jobs, alloc, taskCount, 1, &build_tasks, &run, env.maxThreads);
    u32 planned = 0;
    while (planned < u32(scene.parts.size)) {
        PartPlan& plan = plans[planned++];
        if (!plan_part(settings, Span<LodTask>(k.tasks.data() + plan.firstTask, plan.taskCount), plan)) break;
    }
    parallel_for(env.jobs, alloc, taskCount, 1, &quantize_tasks, &run, env.maxThreads);

    f32 modelMin[3] = {0, 0, 0}, modelMax[3] = {0, 0, 0};
    bool anyModel = false;
    Vec<Bounds> spheres(alloc, Tag::Cook);
    for (u32 i = 0; i < planned; ++i)
        KILN_TRY(merge_part(k, scene.parts[i], plans[i], modelMin, modelMax, anyModel, spheres));
    if (k.lods.empty())
        return diagf(diag, make_status(Code::ValidationFailed), kDiagGltfNoScene, Severity::Error, k.asset,
                     StrView{}, "no triangle geometry in any part");

    for (ImportMount const& m : scene.mounts) {
        mesh::MountDesc md;
        md.name       = m.name;
        md.parentPart = m.parentPart;
        for (u32 a = 0; a < 3; ++a)
            md.translation[a] = m.translation[a];
        for (u32 a = 0; a < 4; ++a)
            md.rotation[a] = m.rotation[a];
        md.extras = m.extras;
        k.mounts.push_back(md);
    }

    Bounds model = make_bounds(modelMin, modelMax, 0);
    f32 radius   = 0;
    for (Bounds const& s : spheres) {
        f32 const dx = s.center[0] - model.center[0];
        f32 const dy = s.center[1] - model.center[1];
        f32 const dz = s.center[2] - model.center[2];
        radius       = max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + s.radius);
    }
    model.radius = radius;

    u64 const sourceHash = src.sourceHash ? src.sourceHash : xxh64(src.bytes);
    u64 const cookHash =
        hash_combine(hash_combine(hash_settings(settings), hash_target(target)), u64(kCookerVersion));

    mesh::WriteDesc wd;
    wd.name       = last_component(src.assetPath);
    wd.assetId    = hash_name(src.assetPath);
    wd.bounds     = model;
    wd.sourceHash = sourceHash;
    wd.cookHash   = cookHash;
    wd.layouts    = k.layouts.span();
    wd.parts      = k.parts.span();
    wd.lods       = k.lods.span();
    wd.submeshes  = k.submeshes.span();
    wd.materials  = k.materials.span();
    wd.textures   = k.bindings.span();
    wd.mounts     = k.mounts.span();
    Stage swWrite(env.profile, "cook.write", src.assetPath);
    Result<Vec<u8>> file = mesh::write(
        wd, mesh::WriteOptions{.compression = settings.compression, .zstdLevel = settings.zstdLevel}, alloc,
        diag);
    if (file.failed()) return file.status();
    u64 const writeUs = swWrite.stop();

    CookedMesh out;
    out.file = std::move(file).value();
    out.textures.init(alloc, Tag::Cook);
    out.strings.init(alloc, Tag::Cook);
    out.sourceHash    = sourceHash;
    out.partCount     = u32(k.parts.size());
    out.lodCount      = u32(k.lods.size());
    out.triangleCount = u32(min(k.triangles, u64(kInvalid)));
    out.vertexCount   = u32(min(k.vertices, u64(kInvalid)));

    // TextureRef views point into out.strings (or the source bytes), so fill the
    // backing store completely first and resolve offsets afterwards.
    struct Off {
        usize path, pathLen, mime, mimeLen, emb, embLen;
        bool embInSrc;
    };
    Vec<Off> offs(alloc, Tag::Cook);
    auto put = [&](StrView s, usize& off, usize& len) {
        off = out.strings.size();
        len = s.size;
        out.strings.append(Span<char const>(s.data, s.size));
        out.strings.push_back('\0');
    };
    auto const srcBegin = reinterpret_cast<std::uintptr_t>(src.bytes.data);
    auto const srcEnd   = srcBegin + src.bytes.size;
    for (TexRefBuild const& r : k.refs) {
        if (r.external) continue;
        Off o{};
        put(r.path, o.path, o.pathLen);
        put(r.mime, o.mime, o.mimeLen);
        if (!r.embedded.empty()) {
            auto const b = reinterpret_cast<std::uintptr_t>(r.embedded.data);
            o.embInSrc   = b >= srcBegin && b + r.embedded.size <= srcEnd;
            if (o.embInSrc) {
                o.emb    = usize(b - srcBegin);
                o.embLen = r.embedded.size;
            } else { // external buffer or data: URI: copy out of the arena
                put(StrView(reinterpret_cast<char const*>(r.embedded.data), r.embedded.size), o.emb,
                    o.embLen);
            }
        }
        offs.push_back(o);
    }
    usize next = 0;
    for (TexRefBuild const& r : k.refs) {
        if (r.external) continue;
        Off const& o  = offs[next++];
        char const* s = out.strings.data();
        TextureRef t{};
        t.assetPath = StrView(s + o.path, o.pathLen);
        t.mimeType  = o.mimeLen ? StrView(s + o.mime, o.mimeLen) : StrView();
        if (o.embLen)
            t.embedded = o.embInSrc ? Span<u8 const>(src.bytes.data + o.emb, o.embLen)
                                    : Span<u8 const>(reinterpret_cast<u8 const*>(s + o.emb), o.embLen);
        t.slot = r.slot;
        t.srgb = r.srgb;
        out.textures.push_back(t);
    }

    out.stats.importUs   = importUs;
    out.stats.buildUs    = k.buildUs;
    out.stats.tangentsUs = k.tangentsUs;
    out.stats.optimizeUs = k.optimizeUs;
    out.stats.packUs     = k.packUs;
    out.stats.writeUs    = writeUs;
    out.stats.totalUs    = swTotal.stop();
    return out;
}

} // namespace kiln::cook
