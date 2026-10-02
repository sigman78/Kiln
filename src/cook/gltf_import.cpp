// gltf_import.cpp — glTF/GLB -> detail::ImportScene; the only TU that includes cgltf.
// All memory lives in the cook arena; external URIs go through MeshSource::resolver.
#include "cook_internal.h"

#include "kiln/cook/image.h"
#include "kiln/log.h"

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
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "cgltf.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace kiln::cook::detail {

namespace {

// ---------------------------------------------------------------------------
// Small linear algebra (column-major, glTF convention)
// ---------------------------------------------------------------------------

struct V3 {
    f32 x, y, z;
};

V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 scale(V3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
f32 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
[[nodiscard]] bool normalize_to(V3 v, V3& out) {
    f32 const len2 = dot(v, v);
    if (!(len2 > 1e-30f)) return false;
    out = scale(v, 1.0f / std::sqrt(len2));
    return true;
}

Mat4 mat4_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mul(Mat4 const& a, Mat4 const& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c)
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0;
            for (u32 k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 mat4_from_mat3(Mat3 const& b) {
    Mat4 r = mat4_identity();
    for (u32 c = 0; c < 3; ++c)
        for (u32 row = 0; row < 3; ++row)
            r.m[c * 4 + row] = b.m[c * 3 + row];
    return r;
}

/// M = T * R * B with R a proper rotation (Gram-Schmidt of M's columns) and B
/// the residual scale / shear / mirror.
struct Decomposed {
    f32 t[3];
    f32 q[4];            ///< xyzw, w >= 0
    Mat4 tr;             ///< T * R
    Mat3 residual;       ///< B
    Mat3 residualNormal; ///< inverse-transpose of B
    bool identity;       ///< B == I within 1e-5 (then residual is exactly I)
    bool mirror;         ///< det(B) < 0
};

Decomposed decompose(Mat4 const& M) {
    Decomposed d{};
    d.t[0] = M.m[12];
    d.t[1] = M.m[13];
    d.t[2] = M.m[14];
    V3 const a0{M.m[0], M.m[1], M.m[2]};
    V3 const a1{M.m[4], M.m[5], M.m[6]};
    V3 const a2{M.m[8], M.m[9], M.m[10]};

    V3 r0{1, 0, 0}, r1{0, 1, 0};
    if (!normalize_to(a0, r0)) r0 = {1, 0, 0};
    if (!normalize_to(sub(a1, scale(r0, dot(a1, r0))), r1)) {
        // a1 parallel to a0 (or zero): any unit vector perpendicular to r0
        V3 const axis = (r0.x < 0.9f && r0.x > -0.9f) ? V3{1, 0, 0} : V3{0, 1, 0};
        if (!normalize_to(cross(r0, axis), r1)) r1 = {0, 1, 0};
    }
    V3 const r2 = cross(r0, r1);

    // B = R^T * A: B(row i, col j) = dot(r_i, a_j)
    V3 const rs[3] = {r0, r1, r2};
    V3 const as[3] = {a0, a1, a2};
    for (u32 j = 0; j < 3; ++j)
        for (u32 i = 0; i < 3; ++i)
            d.residual.m[j * 3 + i] = dot(rs[i], as[j]);

    d.identity = true;
    for (u32 j = 0; j < 3; ++j)
        for (u32 i = 0; i < 3; ++i) {
            f32 const want = i == j ? 1.0f : 0.0f;
            f32 const diff = d.residual.m[j * 3 + i] - want;
            if (diff > 1e-5f || diff < -1e-5f) d.identity = false;
        }
    if (d.identity) {
        d.residual = Mat3{
            {1, 0, 0, 0, 1, 0, 0, 0, 1}
        };
    }
    // Inverse-transpose of B: columns (b1 x b2, b2 x b0, b0 x b1) / det.
    V3 const b0{d.residual.m[0], d.residual.m[1], d.residual.m[2]};
    V3 const b1{d.residual.m[3], d.residual.m[4], d.residual.m[5]};
    V3 const b2{d.residual.m[6], d.residual.m[7], d.residual.m[8]};
    V3 const c0 = cross(b1, b2), c1 = cross(b2, b0), c2 = cross(b0, b1);
    f32 const det  = dot(b0, c0);
    d.mirror       = det < 0;
    f32 const inv  = (det > 1e-30f || det < -1e-30f) ? 1.0f / det : (det < 0 ? -1.0f : 1.0f);
    V3 const cs[3] = {c0, c1, c2};
    for (u32 j = 0; j < 3; ++j) {
        d.residualNormal.m[j * 3 + 0] = cs[j].x * inv;
        d.residualNormal.m[j * 3 + 1] = cs[j].y * inv;
        d.residualNormal.m[j * 3 + 2] = cs[j].z * inv;
    }

    // Quaternion from R (columns r0 r1 r2; R(row, col) = r_col[row]).
    f32 const m00 = r0.x, m10 = r0.y, m20 = r0.z;
    f32 const m01 = r1.x, m11 = r1.y, m21 = r1.z;
    f32 const m02 = r2.x, m12 = r2.y, m22 = r2.z;
    f32 const tr = m00 + m11 + m22;
    f32 x, y, z, w;
    if (tr > 0) {
        f32 const s = std::sqrt(tr + 1.0f) * 2.0f;
        w           = 0.25f * s;
        x           = (m21 - m12) / s;
        y           = (m02 - m20) / s;
        z           = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        f32 const s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w           = (m21 - m12) / s;
        x           = 0.25f * s;
        y           = (m01 + m10) / s;
        z           = (m02 + m20) / s;
    } else if (m11 > m22) {
        f32 const s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w           = (m02 - m20) / s;
        x           = (m01 + m10) / s;
        y           = 0.25f * s;
        z           = (m12 + m21) / s;
    } else {
        f32 const s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w           = (m10 - m01) / s;
        x           = (m02 + m20) / s;
        y           = (m12 + m21) / s;
        z           = 0.25f * s;
    }
    f32 const qlen = std::sqrt(x * x + y * y + z * z + w * w);
    f32 const qs   = (w < 0 ? -1.0f : 1.0f) / qlen;
    d.q[0]         = x * qs;
    d.q[1]         = y * qs;
    d.q[2]         = z * qs;
    d.q[3]         = w * qs;
    if (m00 == 1.0f && m11 == 1.0f && m22 == 1.0f) { // exact identity rotation stays exact
        d.q[0] = d.q[1] = d.q[2] = 0;
        d.q[3]                   = 1;
    }
    for (f32& c : d.q)
        if (c == 0.0f) c = 0.0f; // -0 -> +0

    d.tr = mat4_identity();
    for (u32 c = 0; c < 3; ++c) {
        d.tr.m[c * 4 + 0] = rs[c].x;
        d.tr.m[c * 4 + 1] = rs[c].y;
        d.tr.m[c * 4 + 2] = rs[c].z;
    }
    d.tr.m[12] = d.t[0];
    d.tr.m[13] = d.t[1];
    d.tr.m[14] = d.t[2];
    return d;
}

// ---------------------------------------------------------------------------
// Context, diagnostics
// ---------------------------------------------------------------------------

struct PartBuild {
    cgltf_node const* node = nullptr;
    StrView name;
    u32 parent = kInvalid;
    Decomposed xf{};
    Mat4 frame{};
};

struct LodCand {
    cgltf_node const* node = nullptr;
    StrView name;
    StrView base;
    u32 n = 0;
};

struct Ctx {
    Ctx(MeshSource const& s, MeshCookSettings const& st, Arena& a, Allocator const* al, DiagSink const* d)
        : src(s), settings(st), arena(a), alloc(al), diag(d), asset(diag_asset(s)), parts(al, Tag::Cook),
          mounts(al, Tag::Cook), lodCands(al, Tag::Cook), meshPrims(al, Tag::Cook), meshRead(al, Tag::Cook) {}

    MeshSource const& src;
    MeshCookSettings const& settings;
    Arena& arena;
    Allocator const* alloc;
    DiagSink const* diag;
    StrView asset;
    cgltf_options opt{};
    cgltf_data* data = nullptr;

    Vec<PartBuild> parts;
    Vec<ImportMount> mounts;
    Vec<LodCand> lodCands;
    Vec<Span<ImportPrim const>> meshPrims; ///< per glTF mesh, filled lazily
    Vec<u8> meshRead;                      ///< per glTF mesh: 1 once read
};

#define IMPORT_FAIL(c, errCode, diagCode, where, ...)                                                        \
    return diagf((c).diag, make_status(errCode), diagCode, Severity::Error, (c).asset, where, __VA_ARGS__)
#define IMPORT_NOTE(c, sev, diagCode, where, ...)                                                            \
    (void)diagf((c).diag, kOk, diagCode, sev, (c).asset, where, __VA_ARGS__)

void* cgltf_arena_alloc(void* user, cgltf_size size) {
    return static_cast<Arena*>(user)->alloc(size ? size : 1, 16);
}
void cgltf_arena_free(void*, void*) {}

/// cgltf must never touch the file system: external URIs go through the resolver.
cgltf_result cgltf_no_file_read(cgltf_memory_options const*, cgltf_file_options const*, char const*,
                                cgltf_size*, void**) {
    return cgltf_result_file_not_found;
}
void cgltf_no_file_release(cgltf_memory_options const*, cgltf_file_options const*, void*) {}

template <class T> Span<T const> persist(Arena& arena, Vec<T> const& v) {
    if (v.empty()) return {};
    T* p = arena.alloc_array<T>(v.size());
    for (usize i = 0; i < v.size(); ++i)
        ::new (static_cast<void*>(p + i)) T(v[i]);
    return {p, v.size()};
}

StrView sv(char const* s) { return s ? StrView(s) : StrView(); }

char const* result_name(cgltf_result r) {
    switch (r) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "io error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.0";
    case cgltf_result_max_enum: break;
    }
    return "unknown error";
}

// ---------------------------------------------------------------------------
// Buffers and accessors
// ---------------------------------------------------------------------------

/// The one place buffer-view bytes are obtained: the future EXT_meshopt_compression
/// decode hook (docs/design/dependencies.md). Until then such files fail with K1002.
Span<u8 const> resolve_buffer_view(cgltf_buffer_view const* view) {
    if (!view || !view->buffer || !view->buffer->data) return {};
    cgltf_buffer const* b = view->buffer;
    if (view->offset > b->size || view->size > b->size - view->offset) return {};
    return {static_cast<u8 const*>(b->data) + view->offset, view->size};
}

/// Percent-decode a URI into the arena (null-terminated).
StrView decode_uri(Arena& arena, StrView uri) {
    StrView copy = arena.copy(uri);
    char* p      = const_cast<char*>(copy.data);
    usize n      = cgltf_decode_uri(p);
    return {p, n};
}

Status load_buffers(Ctx& c) {
    cgltf_data* d = c.data;
    for (cgltf_size i = 0; i < d->buffers_count; ++i) {
        cgltf_buffer& b = d->buffers[i];
        if (b.data || !b.uri) continue;
        StrView const uri = sv(b.uri);
        if (uri.starts_with("data:")) continue; // cgltf_load_buffers decodes base64
        char where[32];
        format(where, sizeof where, "buffer %u", unsigned(i));
        if (!c.src.resolver.fn)
            IMPORT_FAIL(c, Code::NotFound, kDiagGltfExternalMissing, StrView(where),
                        "external buffer '%.*s' needs a UriResolver", KILN_SV(uri));
        StrView const decoded = decode_uri(c.arena, uri);
        char resolved[kMaxAssetNameLen + 1];
        if (resolve_asset_name(c.src.assetPath, decoded, resolved, sizeof resolved) == 0)
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfUriOutsideRoot, StrView(where),
                        "external buffer '%.*s' is absolute, leaves the root or gives an invalid asset name",
                        KILN_SV(uri));
        Vec<u8> bytes(c.alloc, Tag::Cook);
        Status const st = c.src.resolver.fn(c.src.resolver.user, decoded, c.alloc, &bytes);
        if (st.failed())
            IMPORT_FAIL(c, Code::NotFound, kDiagGltfExternalMissing, StrView(where),
                        "external buffer '%.*s' could not be resolved (%s)", KILN_SV(uri),
                        code_name(st.code));
        if (bytes.size() < b.size)
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, StrView(where),
                        "external buffer '%.*s' has %llu bytes, byteLength is %llu", KILN_SV(uri),
                        static_cast<unsigned long long>(bytes.size()),
                        static_cast<unsigned long long>(b.size));
        u8* copy = c.arena.alloc_array<u8>(bytes.size());
        if (!bytes.empty()) std::memcpy(copy, bytes.data(), bytes.size());
        b.data             = copy;
        b.data_free_method = cgltf_data_free_method_none;
    }
    cgltf_result const r = cgltf_load_buffers(&c.opt, d, nullptr);
    if (r == cgltf_result_data_too_short)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, StrView{},
                    "GLB BIN chunk shorter than buffer 0");
    if (r != cgltf_result_success)
        IMPORT_FAIL(c, Code::ParseError, kDiagGltfParseFailed, StrView{}, "buffer load failed: %s",
                    result_name(r));
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (!d->buffers[i].data) {
            char where[32];
            format(where, sizeof where, "buffer %u", unsigned(i));
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, StrView(where),
                        "buffer has no data");
        }
    return kOk;
}

f32 read_component(u8 const* p, cgltf_component_type ct, bool normalized) {
    switch (ct) {
    case cgltf_component_type_r_8: {
        f32 const v = f32(read_unaligned<i8>(p));
        return normalized ? max(v / 127.0f, -1.0f) : v;
    }
    case cgltf_component_type_r_8u: {
        f32 const v = f32(read_unaligned<u8>(p));
        return normalized ? v / 255.0f : v;
    }
    case cgltf_component_type_r_16: {
        f32 const v = f32(read_unaligned<i16>(p));
        return normalized ? max(v / 32767.0f, -1.0f) : v;
    }
    case cgltf_component_type_r_16u: {
        f32 const v = f32(read_unaligned<u16>(p));
        return normalized ? v / 65535.0f : v;
    }
    case cgltf_component_type_r_32u: return f32(read_unaligned<u32>(p));
    case cgltf_component_type_r_32f: return read_unaligned<f32>(p);
    case cgltf_component_type_invalid:
    case cgltf_component_type_max_enum: break;
    }
    return 0;
}

/// Byte range of accessor element storage, bounds-checked.
Status accessor_bytes(Ctx& c, cgltf_accessor const* acc, StrView where, char const* what, Span<u8 const>& out,
                      usize& stride) {
    if (acc->is_sparse)
        IMPORT_FAIL(c, Code::Unsupported, kDiagGltfSparseAccessor, where,
                    "%s: sparse accessors are not supported", what);
    usize const elemSize = cgltf_calc_size(acc->type, acc->component_type);
    stride               = acc->stride ? acc->stride : elemSize;
    out                  = {};
    if (!acc->buffer_view) return kOk; // all zeros per spec
    Span<u8 const> const view = resolve_buffer_view(acc->buffer_view);
    if (view.empty() && acc->count > 0)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where, "%s: buffer view has no data",
                    what);
    if (acc->count > 0) {
        u64 const need = u64(acc->offset) + u64(stride) * u64(acc->count - 1) + elemSize;
        if (elemSize == 0 || need > view.size)
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                        "%s: accessor needs %llu bytes, buffer view has %llu", what,
                        static_cast<unsigned long long>(need), static_cast<unsigned long long>(view.size));
    }
    out = view;
    return kOk;
}

/// Read `acc` (count elements of `comps` components) as f32 into `out` with
/// `outComps` floats per element (extra output components are left untouched).
Status read_floats(Ctx& c, cgltf_accessor const* acc, StrView where, char const* what, u32 outComps,
                   f32* out) {
    Span<u8 const> bytes;
    usize stride = 0;
    KILN_TRY(accessor_bytes(c, acc, where, what, bytes, stride));
    usize const comps    = cgltf_num_components(acc->type);
    usize const compSize = cgltf_component_size(acc->component_type);
    usize const n        = min(comps, usize(outComps));
    for (usize i = 0; i < acc->count; ++i)
        for (usize k = 0; k < n; ++k)
            out[i * outComps + k] = bytes.empty()
                                        ? 0.0f
                                        : read_component(bytes.data + acc->offset + i * stride + k * compSize,
                                                         acc->component_type, acc->normalized != 0);
    return kOk;
}

[[nodiscard]] bool is_float_or_norm(cgltf_accessor const* a) {
    return a->component_type == cgltf_component_type_r_32f ||
           ((a->component_type == cgltf_component_type_r_8u ||
             a->component_type == cgltf_component_type_r_16u) &&
            a->normalized);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

Status read_prim(Ctx& c, cgltf_primitive const& p, StrView where, ImportPrim& ip, bool& skip) {
    skip                                 = false;
    cgltf_accessor const* pos            = nullptr;
    cgltf_accessor const* nrm            = nullptr;
    cgltf_accessor const* uv[kMaxUvSets] = {};
    cgltf_accessor const* col            = nullptr;
    for (cgltf_size a = 0; a < p.attributes_count; ++a) {
        cgltf_attribute const& at = p.attributes[a];
        switch (at.type) {
        case cgltf_attribute_type_position:
            if (at.index == 0) pos = at.data;
            break;
        case cgltf_attribute_type_normal:
            if (at.index == 0) nrm = at.data;
            break;
        case cgltf_attribute_type_texcoord:
            if (at.index >= 0 && u32(at.index) < kMaxUvSets) uv[at.index] = at.data;
            break;
        case cgltf_attribute_type_color:
            if (at.index == 0) col = at.data;
            break;
        default: break; // TANGENT (regenerated), JOINTS/WEIGHTS (reserved), custom
        }
    }
    if (!pos)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where, "primitive without POSITION");
    if (pos->type != cgltf_type_vec3 || pos->component_type != cgltf_component_type_r_32f)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where, "POSITION must be float VEC3");
    if (pos->count >= kInvalid)
        IMPORT_FAIL(c, Code::Unsupported, kDiagGltfLimit, where, "too many vertices (%llu)",
                    static_cast<unsigned long long>(pos->count));
    u32 const n = u32(pos->count);
    if (nrm && (nrm->type != cgltf_type_vec3 || nrm->component_type != cgltf_component_type_r_32f))
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where, "NORMAL must be float VEC3");
    for (u32 s = 0; s < kMaxUvSets; ++s)
        if (uv[s] && (uv[s]->type != cgltf_type_vec2 || !is_float_or_norm(uv[s])))
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                        "TEXCOORD_%u must be VEC2 float / normalized u8 / u16", s);
    if (col && ((col->type != cgltf_type_vec3 && col->type != cgltf_type_vec4) || !is_float_or_norm(col)))
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                    "COLOR_0 must be VEC3/VEC4 float / normalized u8 / u16");
    cgltf_accessor const* const all[] = {nrm, uv[0], uv[1], col};
    for (cgltf_accessor const* a : all)
        if (a && a->count != pos->count)
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                        "attribute count %llu != POSITION count %llu",
                        static_cast<unsigned long long>(a->count),
                        static_cast<unsigned long long>(pos->count));

    ip.vertexCount = n;
    f32* positions = c.arena.alloc_array<f32>(usize(n) * 3);
    KILN_TRY(read_floats(c, pos, where, "POSITION", 3, positions));
    ip.positions = positions;
    if (nrm) {
        f32* v = c.arena.alloc_array<f32>(usize(n) * 3);
        KILN_TRY(read_floats(c, nrm, where, "NORMAL", 3, v));
        ip.normals = v;
    }
    for (u32 s = 0; s < kMaxUvSets; ++s)
        if (uv[s]) {
            f32* v = c.arena.alloc_array<f32>(usize(n) * 2);
            KILN_TRY(read_floats(c, uv[s], where, "TEXCOORD", 2, v));
            ip.uvs[s] = v;
        }
    if (col) {
        f32* v = c.arena.alloc_array<f32>(usize(n) * 4);
        for (usize i = 0; i < usize(n); ++i)
            v[i * 4 + 3] = 1.0f; // VEC3 colors: alpha 1
        KILN_TRY(read_floats(c, col, where, "COLOR_0", 4, v));
        ip.colors = v;
    }

    u32* indices = nullptr;
    usize count  = 0;
    if (p.indices) {
        cgltf_accessor const* ia = p.indices;
        if (ia->type != cgltf_type_scalar || (ia->component_type != cgltf_component_type_r_8u &&
                                              ia->component_type != cgltf_component_type_r_16u &&
                                              ia->component_type != cgltf_component_type_r_32u))
            IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                        "indices must be SCALAR u8 / u16 / u32");
        Span<u8 const> bytes;
        usize stride = 0;
        KILN_TRY(accessor_bytes(c, ia, where, "indices", bytes, stride));
        count   = ia->count - ia->count % 3;
        indices = c.arena.alloc_array<u32>(max(count, usize(1)));
        for (usize i = 0; i < count; ++i) {
            u8 const* e = bytes.empty() ? nullptr : bytes.data + ia->offset + i * stride;
            u32 v       = 0;
            if (e) {
                if (ia->component_type == cgltf_component_type_r_8u)
                    v = read_unaligned<u8>(e);
                else if (ia->component_type == cgltf_component_type_r_16u)
                    v = read_unaligned<u16>(e);
                else
                    v = read_unaligned<u32>(e);
            }
            if (v >= n)
                IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, where,
                            "index %u >= vertex count %u", v, n);
            indices[i] = v;
        }
    } else {
        count   = usize(n) - usize(n) % 3;
        indices = c.arena.alloc_array<u32>(max(count, usize(1)));
        for (usize i = 0; i < count; ++i)
            indices[i] = u32(i);
    }
    if (count == 0 || n == 0) {
        skip = true;
        return kOk;
    }
    ip.indices    = indices;
    ip.indexCount = u32(count);
    ip.material   = p.material ? u32(cgltf_material_index(c.data, p.material)) : kDefaultMaterial;
    return kOk;
}

Status read_mesh(Ctx& c, cgltf_node const* node, Span<ImportPrim const>& out) {
    cgltf_mesh const* mesh = node->mesh;
    usize const mi         = cgltf_mesh_index(c.data, mesh);
    if (c.meshRead[mi]) {
        out = c.meshPrims[mi];
        return kOk;
    }
    StrView const where = sv(node->name);
    Vec<ImportPrim> prims(c.alloc, Tag::Cook);
    for (cgltf_size i = 0; i < mesh->primitives_count; ++i) {
        cgltf_primitive const& p = mesh->primitives[i];
        if (p.type != cgltf_primitive_type_triangles) {
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfPrimitiveSkipped, where,
                        "mesh '%s' primitive %u: not a triangle list (glTF mode %d), skipped",
                        mesh->name ? mesh->name : "", unsigned(i), int(p.type) - 1);
            continue;
        }
        ImportPrim ip;
        bool skip = false;
        KILN_TRY(read_prim(c, p, where, ip, skip));
        if (!skip) prims.push_back(ip);
    }
    c.meshPrims[mi] = persist(c.arena, prims);
    c.meshRead[mi]  = 1;
    out             = c.meshPrims[mi];
    return kOk;
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------

/// "<base>_lod<N>": returns N and sets base, or kInvalid when the name does not match.
u32 parse_lod_suffix(StrView name, StrView& base) {
    usize d = name.size;
    while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9')
        --d;
    usize const digits = name.size - d;
    if (digits == 0 || digits > 9 || d < 5) return kInvalid; // need at least one base char
    if (name.substr(d - 4, 4) != "_lod") return kInvalid;
    u32 n = 0;
    for (usize i = d; i < name.size; ++i)
        n = n * 10 + u32(name[i] - '0');
    base = name.substr(0, d - 4);
    return n;
}

constexpr u32 kMaxDepth = 256;

Status visit(Ctx& c, cgltf_node const* node, u32 parentPart, Mat4 const& rel, Mat4 const& parentFrame,
             u32 depth) {
    if (depth > kMaxDepth)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, sv(node->name),
                    "node hierarchy deeper than %u", kMaxDepth);
    StrView const name = sv(node->name);
    if (name.starts_with("col_") || name.starts_with("_")) return kOk; // skipped subtree

    Mat4 local{};
    cgltf_node_transform_local(node, local.m);
    Mat4 const relNode = mul(rel, local);

    if (name.starts_with("mount_")) {
        if (node->mesh)
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfLodConvention, name, "mesh on mount node ignored");
        Decomposed const x = decompose(relNode);
        if (!x.identity)
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfScaleBaked, name,
                        "mount has a non-unit scale; the scale is ignored");
        ImportMount m;
        m.name       = name;
        m.parentPart = parentPart;
        for (u32 i = 0; i < 3; ++i)
            m.translation[i] = x.t[i];
        for (u32 i = 0; i < 4; ++i)
            m.rotation[i] = x.q[i];
        m.extras = c.arena.copy(sv(node->extras.data));
        c.mounts.push_back(m);
        for (cgltf_size i = 0; i < node->children_count; ++i)
            KILN_TRY(visit(c, node->children[i], parentPart, relNode, parentFrame, depth + 1));
        return kOk;
    }

    StrView base;
    u32 const lod = parse_lod_suffix(name, base);
    if (lod != kInvalid && lod >= 1) {
        if (!c.settings.useAuthoredLods) return kOk;
        if (!node->mesh) {
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfLodConvention, name,
                        "LOD node without a mesh ignored");
            return kOk;
        }
        c.lodCands.push_back(LodCand{node, name, base, lod});
        return kOk; // a LOD node's subtree is not traversed
    }

    if (node->mesh) {
        PartBuild pb;
        pb.node   = node;
        pb.name   = lod == 0 ? base : name;
        pb.parent = parentPart;
        pb.xf     = decompose(relNode);
        pb.frame  = mul(parentFrame, pb.xf.tr);
        if (pb.name.empty()) {
            char buf[32];
            usize const len = format(buf, sizeof buf, "node_%u", unsigned(cgltf_node_index(c.data, node)));
            pb.name         = c.arena.copy(StrView(buf, len));
        }
        if (!pb.xf.identity)
            IMPORT_NOTE(c, Severity::Info, kDiagGltfScaleBaked, pb.name,
                        "node scale%s baked into the vertices", pb.xf.mirror ? " (mirrored)" : "");
        u32 const self = u32(c.parts.size());
        c.parts.push_back(pb);
        Mat4 const childRel = mat4_from_mat3(c.parts[self].xf.residual);
        Mat4 const frame    = c.parts[self].frame;
        for (cgltf_size i = 0; i < node->children_count; ++i)
            KILN_TRY(visit(c, node->children[i], self, childRel, frame, depth + 1));
        return kOk;
    }

    for (cgltf_size i = 0; i < node->children_count; ++i)
        KILN_TRY(visit(c, node->children[i], parentPart, relNode, parentFrame, depth + 1));
    return kOk;
}

Status traverse(Ctx& c) {
    cgltf_data const* d      = c.data;
    Mat4 const I             = mat4_identity();
    cgltf_scene const* scene = d->scene ? d->scene : (d->scenes_count ? &d->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            KILN_TRY(visit(c, scene->nodes[i], kInvalid, I, I, 0));
    } else {
        for (cgltf_size i = 0; i < d->nodes_count; ++i)
            if (!d->nodes[i].parent) KILN_TRY(visit(c, &d->nodes[i], kInvalid, I, I, 0));
    }
    return kOk;
}

// ---------------------------------------------------------------------------
// Materials and images
// ---------------------------------------------------------------------------

void read_texture_view(Ctx& c, cgltf_texture_view const& v, ImportTexture& out) {
    if (!v.texture) return;
    out.present = true;
    // The core source (PNG/JPEG) wins over EXT_texture_webp: it is never lossier.
    cgltf_image const* img = v.texture->image;
    if (!img && webp_decode_enabled()) img = v.texture->webp_image;
    if (!img) img = v.texture->basisu_image;
    out.image    = img ? u32(cgltf_image_index(c.data, img)) : kInvalid;
    cgltf_int tc = v.texcoord;
    if (v.has_transform && v.transform.has_texcoord) tc = v.transform.texcoord;
    out.texcoord = tc > 0 ? u32(tc) : 0;
}

StrView mime_from_extension(StrView uri) {
    usize const q   = uri.find('?');
    StrView const p = q == StrView::kNpos ? uri : uri.substr(0, q);
    usize const dot = p.rfind('.');
    if (dot == StrView::kNpos) return {};
    StrView const ext = p.substr(dot + 1);
    auto ieq          = [](StrView a, StrView b) {
        if (a.size != b.size) return false;
        for (usize i = 0; i < a.size; ++i) {
            char x = a[i];
            if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
            if (x != b[i]) return false;
        }
        return true;
    };
    if (ieq(ext, "png")) return "image/png";
    if (ieq(ext, "ktx2")) return "image/ktx2";
    if (ieq(ext, "jpg") || ieq(ext, "jpeg")) return "image/jpeg";
    if (ieq(ext, "webp")) return "image/webp";
    return {};
}

void read_images(Ctx& c, Vec<ImportImage>& out) {
    cgltf_data const* d = c.data;
    for (cgltf_size i = 0; i < d->images_count; ++i) {
        cgltf_image const& img = d->images[i];
        ImportImage ii;
        ii.name     = sv(img.name);
        ii.mimeType = sv(img.mime_type);
        if (img.buffer_view) {
            ii.bytes  = resolve_buffer_view(img.buffer_view);
            ii.usable = !ii.bytes.empty();
        } else if (img.uri) {
            StrView const uri = sv(img.uri);
            if (uri.starts_with("data:")) {
                usize const comma = uri.find(',');
                if (comma != StrView::kNpos && comma >= 7 && uri.substr(comma - 7, 7) == ";base64") {
                    if (ii.mimeType.empty()) {
                        usize const semi = uri.find(';');
                        ii.mimeType      = uri.substr(5, (semi < comma ? semi : comma) - 5);
                    }
                    StrView b64 = uri.substr(comma + 1);
                    usize len   = b64.size;
                    while (len > 0 && b64[len - 1] == '=')
                        --len;
                    usize const size = len * 3 / 4;
                    void* bytes      = nullptr;
                    if (size > 0 &&
                        cgltf_load_buffer_base64(&c.opt, size, b64.data, &bytes) == cgltf_result_success) {
                        ii.bytes  = {static_cast<u8 const*>(bytes), size};
                        ii.usable = true;
                    }
                }
            } else {
                ii.uri    = decode_uri(c.arena, uri);
                ii.usable = true;
                if (ii.mimeType.empty()) ii.mimeType = mime_from_extension(ii.uri);
            }
        }
        out.push_back(ii);
    }
}

void read_materials(Ctx& c, Vec<ImportMaterial>& out) {
    cgltf_data const* d = c.data;
    for (cgltf_size i = 0; i < d->materials_count; ++i) {
        cgltf_material const& m = d->materials[i];
        ImportMaterial im;
        im.name        = sv(m.name);
        im.index       = u32(i);
        im.doubleSided = m.double_sided != 0;
        im.alphaMode   = m.alpha_mode == cgltf_alpha_mode_mask    ? mesh::AlphaMode::Mask
                         : m.alpha_mode == cgltf_alpha_mode_blend ? mesh::AlphaMode::Blend
                                                                  : mesh::AlphaMode::Opaque;
        im.alphaCutoff = m.alpha_cutoff;
        if (m.has_pbr_metallic_roughness) {
            for (u32 k = 0; k < 4; ++k)
                im.baseColorFactor[k] = m.pbr_metallic_roughness.base_color_factor[k];
            im.metallicFactor  = m.pbr_metallic_roughness.metallic_factor;
            im.roughnessFactor = m.pbr_metallic_roughness.roughness_factor;
        }
        f32 const emissiveStrength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
        for (u32 k = 0; k < 3; ++k)
            im.emissiveFactor[k] = m.emissive_factor[k] * emissiveStrength;
        // cgltf leaves a view's scale 0 without a texture; glTF's default is 1.
        if (m.normal_texture.texture) im.normalScale = m.normal_texture.scale;
        if (m.occlusion_texture.texture) im.occlusionStrength = m.occlusion_texture.scale;
        if (m.has_pbr_metallic_roughness) {
            read_texture_view(c, m.pbr_metallic_roughness.base_color_texture,
                              im.slots[u32(mesh::TextureSlot::BaseColor)]);
            read_texture_view(c, m.pbr_metallic_roughness.metallic_roughness_texture,
                              im.slots[u32(mesh::TextureSlot::MetalRough)]);
        }
        read_texture_view(c, m.normal_texture, im.slots[u32(mesh::TextureSlot::Normal)]);
        read_texture_view(c, m.occlusion_texture, im.slots[u32(mesh::TextureSlot::Occlusion)]);
        read_texture_view(c, m.emissive_texture, im.slots[u32(mesh::TextureSlot::Emissive)]);
        out.push_back(im);
    }
}

// ---------------------------------------------------------------------------
// Parts and LODs
// ---------------------------------------------------------------------------

Status build_parts(Ctx& c, Vec<ImportPart>& out) {
    u32 const nParts = u32(c.parts.size());

    // LOD candidates -> owning part (first part with the base name).
    Vec<u32> candPart(c.alloc, Tag::Cook);
    candPart.resize(c.lodCands.size(), kInvalid);
    for (usize k = 0; k < c.lodCands.size(); ++k) {
        LodCand const& lc = c.lodCands[k];
        for (u32 p = 0; p < nParts; ++p)
            if (c.parts[p].name == lc.base) {
                candPart[k] = p;
                break;
            }
        if (candPart[k] == kInvalid)
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfLodConvention, lc.name,
                        "LOD node has no base part named '%.*s'; ignored", KILN_SV(lc.base));
    }

    Vec<u32> mine(c.alloc, Tag::Cook);
    Vec<ImportLod> lods(c.alloc, Tag::Cook);
    for (u32 p = 0; p < nParts; ++p) {
        PartBuild const& pb = c.parts[p];
        ImportPart ip;
        ip.name   = pb.name;
        ip.parent = pb.parent;
        for (u32 i = 0; i < 3; ++i)
            ip.translation[i] = pb.xf.t[i];
        for (u32 i = 0; i < 4; ++i)
            ip.rotation[i] = pb.xf.q[i];
        ip.bake         = pb.xf.residual;
        ip.bakeNormal   = pb.xf.residualNormal;
        ip.bakeIdentity = pb.xf.identity;
        ip.flipWinding  = pb.xf.mirror;
        ip.frame        = pb.frame;

        lods.clear();
        ImportLod l0;
        l0.nodeName = sv(pb.node->name);
        l0.lodIndex = 0;
        KILN_TRY(read_mesh(c, pb.node, l0.prims));
        lods.push_back(l0);

        // Authored LODs of this part, ascending N (stable in traversal order).
        mine.clear();
        for (usize k = 0; k < c.lodCands.size(); ++k)
            if (candPart[k] == p) mine.push_back(u32(k));
        for (usize a = 1; a < mine.size(); ++a) // insertion sort: small, stable
            for (usize b = a; b > 0 && c.lodCands[mine[b - 1]].n > c.lodCands[mine[b]].n; --b) {
                u32 const t = mine[b - 1];
                mine[b - 1] = mine[b];
                mine[b]     = t;
            }
        u32 expect = 1;
        bool gap   = false;
        for (usize a = 0; a < mine.size(); ++a) {
            LodCand const& lc = c.lodCands[mine[a]];
            if (a > 0 && c.lodCands[mine[a - 1]].n == lc.n) {
                IMPORT_NOTE(c, Severity::Warning, kDiagGltfLodConvention, lc.name,
                            "duplicate LOD %u for part '%.*s'; ignored", lc.n, KILN_SV(pb.name));
                continue;
            }
            if (lc.n != expect) gap = true;
            expect = lc.n + 1;
            ImportLod l;
            l.nodeName = lc.name;
            l.lodIndex = lc.n;
            KILN_TRY(read_mesh(c, lc.node, l.prims));
            lods.push_back(l);
        }
        if (gap)
            IMPORT_NOTE(c, Severity::Warning, kDiagGltfLodConvention, pb.name,
                        "gaps in authored LOD numbers; LODs compacted in ascending order");
        ip.lods = persist(c.arena, lods);
        out.push_back(ip);
    }
    return kOk;
}

} // namespace

Status import_gltf(MeshSource const& src, MeshCookSettings const& settings, Arena& arena,
                   Allocator const* alloc, DiagSink const* diag, ImportScene& out) {
    Ctx c(src, settings, arena, alloc, diag);

    c.opt.memory.alloc_func = &cgltf_arena_alloc;
    c.opt.memory.free_func  = &cgltf_arena_free;
    c.opt.memory.user_data  = &arena;
    c.opt.file.read         = &cgltf_no_file_read;
    c.opt.file.release      = &cgltf_no_file_release;

    if (src.bytes.empty()) IMPORT_FAIL(c, Code::ParseError, kDiagGltfParseFailed, StrView{}, "empty source");
    cgltf_result const pr = cgltf_parse(&c.opt, src.bytes.data, src.bytes.size, &c.data);
    if (pr != cgltf_result_success || !c.data)
        IMPORT_FAIL(c, Code::ParseError, kDiagGltfParseFailed, StrView{}, "not a valid glTF/GLB: %s",
                    result_name(pr));
    cgltf_data* const d = c.data;

    // Unsupported required extensions, checked before touching buffers.
    for (cgltf_size i = 0; i < d->extensions_required_count; ++i) {
        StrView const ext = sv(d->extensions_required[i]);
        if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression")
            IMPORT_FAIL(c, Code::Unsupported, kDiagGltfUnsupportedExt, ext,
                        "required extension %.*s is not supported", KILN_SV(ext));
    }
    for (cgltf_size m = 0; m < d->meshes_count; ++m)
        for (cgltf_size p = 0; p < d->meshes[m].primitives_count; ++p)
            if (d->meshes[m].primitives[p].has_draco_mesh_compression)
                IMPORT_FAIL(c, Code::Unsupported, kDiagGltfUnsupportedExt, sv(d->meshes[m].name),
                            "KHR_draco_mesh_compression is not supported");

    KILN_TRY(load_buffers(c));
    cgltf_result const vr = cgltf_validate(d);
    if (vr != cgltf_result_success)
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfBadAccessor, StrView{}, "glTF validation failed: %s",
                    result_name(vr));

    c.meshPrims.resize(d->meshes_count);
    c.meshRead.resize(d->meshes_count, 0);

    KILN_TRY(traverse(c));
    if (c.parts.empty())
        IMPORT_FAIL(c, Code::ValidationFailed, kDiagGltfNoScene, StrView{},
                    "no mesh nodes left after applying the naming conventions");

    Vec<ImportPart> parts(alloc, Tag::Cook);
    KILN_TRY(build_parts(c, parts));
    Vec<ImportMaterial> materials(alloc, Tag::Cook);
    read_materials(c, materials);
    Vec<ImportImage> images(alloc, Tag::Cook);
    read_images(c, images);

    out.parts     = persist(arena, parts);
    out.materials = persist(arena, materials);
    out.images    = persist(arena, images);
    out.mounts    = persist(arena, c.mounts);
    // cgltf_free is not called: every cgltf allocation lives in the arena.
    return kOk;
}

} // namespace kiln::cook::detail
