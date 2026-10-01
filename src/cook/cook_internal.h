// src/cook/cook_internal.h — private interface between the glTF importer
// (gltf_import.cpp, the only file that sees cgltf) and the mesh cooker (mesh_cook.cpp).
// Also holds Stopwatch, a tiny CookStats timer shared with texture_cook.cpp.
#pragma once

#include "kiln/assets.h" // asset name rules
#include "kiln/cook/cook.h"
#include "kiln/mesh.h"

#include <chrono>

namespace kiln::cook::detail {

/// A CookStats timer that is also a profile zone: stop() ends both, and so does scope exit (an
/// early return). Stop stages in reverse order of creation, so zones nest.
class Stage {
public:
    Stage(ProfileHooks const* hooks, char const* name, StrView asset)
        : begin_(std::chrono::steady_clock::now()), hooks_(hooks && hooks->zone_begin ? hooks : nullptr),
          name_(name), asset_(asset) {
        if (hooks_) hooks_->zone_begin(hooks_->user, name_, asset_);
    }
    ~Stage() { (void)stop(); }
    Stage(Stage const&)            = delete;
    Stage& operator=(Stage const&) = delete;

    /// Microseconds since construction; the first call ends the zone, later calls return the same.
    u64 stop() {
        if (!stopped_) {
            auto const dt = std::chrono::steady_clock::now() - begin_;
            us_           = u64(std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
            stopped_      = true;
            if (hooks_ && hooks_->zone_end) hooks_->zone_end(hooks_->user, name_, asset_);
        }
        return us_;
    }

private:
    std::chrono::steady_clock::time_point begin_;
    ProfileHooks const* hooks_;
    char const* name_;
    StrView asset_;
    u64 us_       = 0;
    bool stopped_ = false;
};

/// Wall-clock timer for CookStats (rollout step 1, docs/design/cook-kernels.md).
/// std::chrono is fine here (a .cpp-only internal header); never in include/.
struct Stopwatch {
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    u64 elapsed_us() const {
        auto const dt = std::chrono::steady_clock::now() - begin;
        return u64(std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
    }
};

inline constexpr u32 kMaxUvSets       = 2;        ///< TEXCOORD_0 / TEXCOORD_1 are cooked
inline constexpr u32 kSlotCount       = 5;        ///< mesh::TextureSlot BaseColor..Emissive
inline constexpr u32 kDefaultMaterial = kInvalid; ///< primitive without a material

/// Column-major 4x4 matrix (glTF convention: m[col * 4 + row]).
struct Mat4 {
    f32 m[16];
};
/// Column-major 3x3 matrix (m[col * 3 + row]).
struct Mat3 {
    f32 m[9];
};

/// One triangle-list primitive, attributes expanded to f32 in the arena.
struct ImportPrim {
    u32 material               = kDefaultMaterial; ///< glTF material index or kDefaultMaterial
    u32 vertexCount            = 0;
    u32 indexCount             = 0;       ///< multiple of 3; every index < vertexCount
    f32 const* positions       = nullptr; ///< 3 * vertexCount
    f32 const* normals         = nullptr; ///< 3 * vertexCount, or null (generated)
    f32 const* uvs[kMaxUvSets] = {};      ///< 2 * vertexCount, or null
    f32 const* colors          = nullptr; ///< 4 * vertexCount (RGBA), or null
    u32 const* indices         = nullptr; ///< indexCount
};

struct ImportLod {
    StrView nodeName; ///< the node the geometry came from (diagnostics)
    u32 lodIndex = 0; ///< authored N (0 for the base)
    Span<ImportPrim const> prims;
};

struct ImportPart {
    StrView name;
    u32 parent         = kInvalid; ///< index into ImportScene::parts
    f32 translation[3] = {0, 0, 0};
    f32 rotation[4]    = {0, 0, 0, 1};
    /// Residual scale/shear (and mirror) baked into every LOD's vertices:
    /// position' = bake * position, normal' = normalize(bakeNormal * normal).
    Mat3 bake         = {};
    Mat3 bakeNormal   = {};
    bool bakeIdentity = true;
    bool flipWinding  = false;  ///< det(bake) < 0
    Mat4 frame        = {};     ///< model-space frame of the cooked part (translation/rotation chain)
    Span<ImportLod const> lods; ///< LOD0 first; empty if the node's mesh had no triangles
};

struct ImportTexture {
    bool present = false;    ///< the material slot references a texture
    u32 image    = kInvalid; ///< glTF image index, kInvalid if the texture has no image
    u32 texcoord = 0;
};

struct ImportMaterial {
    StrView name;                  ///< as written; empty if unnamed
    u32 index                 = 0; ///< glTF material index
    bool doubleSided          = false;
    mesh::AlphaMode alphaMode = mesh::AlphaMode::Opaque;
    f32 alphaCutoff           = 0.5f;
    f32 baseColorFactor[4]    = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 emissiveFactor[3]     = {0.0f, 0.0f, 0.0f}; ///< emissive strength folded in
    f32 metallicFactor        = 1.0f;
    f32 roughnessFactor       = 1.0f;
    f32 normalScale           = 1.0f;
    f32 occlusionStrength     = 1.0f;
    ImportTexture slots[kSlotCount]; ///< indexed by mesh::TextureSlot
};

struct ImportImage {
    StrView name;         ///< as written; may be empty
    StrView uri;          ///< external URI, percent-decoded; empty for embedded / data: images
    StrView mimeType;     ///< may be empty
    Span<u8 const> bytes; ///< embedded bytes (buffer view or decoded data: URI)
    bool usable = false;  ///< has an external URI or embedded bytes
};

struct ImportMount {
    StrView name;
    u32 parentPart     = kInvalid;
    f32 translation[3] = {0, 0, 0};
    f32 rotation[4]    = {0, 0, 0, 1};
    StrView extras; ///< "key=value;..." or empty
};

struct ImportScene {
    Span<ImportPart const> parts;         ///< topological (parent < self)
    Span<ImportMaterial const> materials; ///< every glTF material, glTF order
    Span<ImportImage const> images;       ///< every glTF image, glTF order
    Span<ImportMount const> mounts;       ///< traversal order
};

/// Parse, load buffers, validate, apply naming conventions and traverse. Everything
/// in `out` is plain data in `arena`. Emits K1xxx diagnostics; fails per cook.h.
Status import_gltf(MeshSource const& src, MeshCookSettings const& settings, Arena& arena,
                   Allocator const* alloc, DiagSink const* diag, ImportScene& out);

/// The asset name used in diagnostics (sourcePath if set, else assetPath).
inline StrView diag_asset(MeshSource const& src) {
    return src.sourcePath.empty() ? src.assetPath : src.sourcePath;
}

/// The block format of an explicit encoding (Undefined for Auto and Uncompressed).
Format encoding_format(TextureEncoding e, bool srgb);
/// True when the block formats of this texture are the sRGB variants: sRGB Color and UI only.
[[nodiscard]] bool srgb_blocks(ColorSpace cs, TextureUsage usage);

} // namespace kiln::cook::detail
