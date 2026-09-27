// kiln/cook/cook.h — the cooker: glTF/GLB -> .mesh, PNG/KTX2 -> KTX2, and the
// content-hashed store. Pure functions over caller memory (HANDOFF §6.2): no
// global state, thread-safe, arena-backed temporaries, deterministic output.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/cook/settings.h"
#include "kiln/ktx2.h"
#include "kiln/result.h"

namespace kiln::cook {

/// Bump when cooked output changes for identical input and settings. Part of every store key.
inline constexpr u32 kCookerVersion = 1;

// ---------------------------------------------------------------------------
// Diagnostics (K1000-K1999: glTF import). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum GltfDiagCode : u32 {
    kDiagGltfParseFailed = 1001, ///< not a valid GLB / glTF (ParseError)
    kDiagGltfUnsupportedExt =
        1002, ///< required extension not supported: Draco, EXT_meshopt_compression (Unsupported)
    kDiagGltfSparseAccessor  = 1003, ///< sparse accessors are rejected (Unsupported)
    kDiagGltfBadAccessor     = 1004, ///< accessor/buffer view out of range or wrong type (ValidationFailed)
    kDiagGltfExternalMissing = 1005, ///< external buffer/image URI could not be resolved (NotFound)
    kDiagGltfNoScene =
        1006, ///< no scene / no mesh nodes after applying naming conventions (ValidationFailed)
    kDiagGltfPrimitiveSkipped = 1007, ///< non-triangle primitive skipped (Warning)
    kDiagGltfNoNormals        = 1008, ///< normals missing, generated (Info)
    kDiagGltfNoTangentSource  = 1009, ///< tangents requested but no UV0/normals: skipped (Warning)
    kDiagGltfLodConvention    = 1010, ///< `_lodN` node without a base part, gaps in N, or LOD with different
                                      ///< material set (Warning/Error)
    kDiagGltfScaleBaked      = 1011,  ///< non-uniform or negative node scale baked into vertices (Info)
    kDiagGltfMaterialRenamed = 1012,  ///< `.NNN` suffix stripped from a material name (Info)
    kDiagGltfImageUnresolvable =
        1013,              ///< texture references an image with no usable source (Warning; binding dropped)
    kDiagGltfLimit = 1014, ///< too many streams/attributes/vertices for the format (Unsupported)
    kDiagGltfExtrasDropped =
        1015, ///< mount extras pair with `;`/`=` in key or value, or non-scalar, dropped (Warning)
    kDiagGltfEmptyMesh     = 1016, ///< a part ended up with zero triangles (Warning)
    kDiagGltfUsageConflict = 1017, ///< the same image is bound to slots implying different usages (Warning)
    kDiagGltfQuantFallback = 1018, ///< positions/UVs fell back to the precise profile (Info)
};

// ---------------------------------------------------------------------------
// Mesh cooking
// ---------------------------------------------------------------------------

/// Resolves an external URI referenced by a .gltf (buffers, images). Returns the
/// bytes (allocated from `alloc`, Tag::Cook) or a failed Status. Not needed for .glb
/// with embedded buffers and images.
struct UriResolver {
    Status (*fn)(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) = nullptr;
    void* user                                                                  = nullptr;
};

struct MeshSource {
    Span<u8 const> bytes; ///< .glb (or .gltf JSON) contents
    StrView assetPath;    ///< cooked asset path, e.g. "meshes/ship_hauler_a" (no extension)
    StrView sourcePath;   ///< for diagnostics and relative URIs, e.g. "assets/ship_hauler_a.glb"
    u64 sourceHash = 0;   ///< xxh64 of bytes; 0 = computed by cook_mesh
    UriResolver resolver; ///< optional
};

/// A texture the mesh references. The cooker does not cook it; the caller (store,
/// cook-on-miss provider, kiln-cook) does, using `usage` as the SlotHint-derived
/// inference. Views point into CookedMesh::strings / the source bytes.
struct TextureRef {
    StrView assetPath;       ///< cooked texture asset path: "<mesh assetPath>/<image stem>"
    StrView uri;             ///< source-relative URI for external images; empty when embedded
    Span<u8 const> embedded; ///< image bytes when embedded in the GLB (points into MeshSource::bytes)
    StrView mimeType;        ///< "image/png", "image/ktx2", ... (may be empty for external)
    SlotHint slot;           ///< first slot that referenced it (usage inference)
    bool srgb;               ///< as inferred from the slot
};

struct CookedMesh {
    Vec<u8> file;             ///< the .mesh bytes
    Vec<TextureRef> textures; ///< distinct textures referenced, in first-reference order
    Vec<char> strings;        ///< backing store for TextureRef views (stable after cook)
    u64 sourceHash = 0;
    u32 partCount = 0, lodCount = 0;
    u32 triangleCount = 0, vertexCount = 0; ///< totals across all LOD records, not LOD0 only
};

/// Cook a glTF/GLB into a .mesh. `settings` must be resolved (resolve_mesh).
/// Failure: Status per error-model.md, one or more K1xxx diagnostics. Warnings and
/// infos never fail the cook.
KILN_API Result<CookedMesh> cook_mesh(MeshSource const& src, MeshCookSettings const& settings,
                                      TargetProfile const& target, Allocator const* alloc,
                                      DiagSink const* diag = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Texture cooking
// ---------------------------------------------------------------------------

struct TextureSource {
    Span<u8 const> bytes; ///< PNG or KTX2
    StrView assetPath;    ///< e.g. "meshes/ship_hauler_a/hull_albedo"
    StrView sourcePath;   ///< for diagnostics
    u64 sourceHash = 0;   ///< 0 = computed
};

struct CookedTexture {
    Vec<u8> file; ///< the KTX2 bytes
    ktx2::TextureDesc desc;
    bool passthrough = false; ///< input was a KTX2 already suitable for the target
    u64 sourceHash   = 0;
};

/// Cook a PNG (decode, convert per usage, mips) or pass a suitable KTX2 through.
/// `settings` must be resolved (resolve_texture).
KILN_API Result<CookedTexture> cook_texture(TextureSource const& src, TextureCookSettings const& settings,
                                            TargetProfile const& target, Allocator const* alloc,
                                            DiagSink const* diag = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Store (HANDOFF §4.4): content-hashed files, atomic writes, no index in v0.5
// ---------------------------------------------------------------------------

/// Store key = hash_combine chain of (source bytes hash, resolved settings hash,
/// target hash, cooker version).
[[nodiscard]] KILN_API u64 store_key(u64 sourceHash, u64 settingsHash, u64 targetHash,
                                     u32 cookerVersion = kCookerVersion) noexcept;

/// "<16 lowercase hex digits>.<ext>" into `out` (needs 16 + 1 + ext.size + 1 bytes).
/// Returns the length written.
KILN_API usize store_file_name(u64 key, StrView ext, char* out, usize cap) noexcept;

/// Write `bytes` to `<dir>/<name>` atomically (temp file + rename). Creates `dir`
/// if missing (one level). An existing file with the same name is left untouched
/// (content-addressed: same key means same bytes) and Status is Ok.
KILN_API Status store_write(StrView dir, StrView name, Span<u8 const> bytes,
                            DiagSink const* diag = nullptr) noexcept;

/// True if `<dir>/<name>` exists.
[[nodiscard]] KILN_API bool store_exists(StrView dir, StrView name) noexcept;

} // namespace kiln::cook
