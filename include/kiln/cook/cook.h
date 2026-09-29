// kiln/cook/cook.h — the cooker: glTF/GLB -> .mesh, PNG/JPEG/WebP/KTX2 -> KTX2, and the
// content-hashed store. Pure functions over caller memory: no global state,
// thread-safe, deterministic output.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/cook/settings.h"
#include "kiln/ktx2.h"
#include "kiln/result.h"

namespace kiln {
struct JobSystem; // kiln/io.h
} // namespace kiln

namespace kiln::cook {

/// Bump when cooked output changes for identical input and settings. Part of every store key.
inline constexpr u32 kCookerVersion =
    4; // 4: Zstd kept only when it saves kZstdMinSaving of a texture file
       // 3: .mesh 0.4; embedded images named "<mesh>#<name>", external images as kTextureExternal
       // 2: KTX2 outputs carry kiln.sourceHash / kiln.cookHash key/value entries

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
    kDiagGltfEmptyMesh     = 1016,  ///< a part ended up with zero triangles (Warning)
    kDiagGltfUsageConflict = 1017,  ///< the same image is bound to slots implying different usages (Warning)
    kDiagGltfQuantFallback = 1018,  ///< positions/UVs fell back to the precise profile (Info)
    kDiagGltfImageName     = 1019,  ///< two embedded images share a name, or `<mesh>#<name>` is not a valid
                                    ///< asset name (ValidationFailed)
    kDiagGltfUriOutsideRoot = 1020, ///< an external URI is absolute, leaves the root or gives an invalid
                                    ///< asset name (ValidationFailed)
    kDiagMeshCookNotBuilt = 1021,   ///< kiln_cook built with KILN_MESH=OFF: no mesh cooking (Unsupported)
};

// ---------------------------------------------------------------------------
// Cook statistics (rollout step 1, docs/design/cook-kernels.md): per-stage wall time
// for manual profiling, filled in by cook_mesh / cook_texture. Never affects cooked
// bytes. A field stays 0 when its stage did not run (e.g. tangentsUs with no UV0, or
// the mesh fields on a CookedTexture).
// ---------------------------------------------------------------------------

struct CookStats {
    // Texture: decode_image, prepare_image (convert + flip green + renormalize),
    // build_mip_chain, BC encoding, ktx2::write.
    u64 decodeUs  = 0;
    u64 prepareUs = 0;
    u64 mipsUs    = 0;
    u64 encodeUs  = 0; ///< 0 for uncompressed outputs

    // Mesh: accumulated across every (part, LOD).
    // Summed task time: with a job pool the stages run in parallel and can add up to more than totalUs.
    u64 importUs   = 0; ///< glTF parse/import
    u64 buildUs    = 0; ///< build_lod: expand, bake, weld, normals
    u64 tangentsUs = 0;
    u64 optimizeUs = 0;
    u64 packUs     = 0; ///< quantize/pack + submesh records

    // Shared: the file write and the whole cook_mesh / cook_texture call.
    u64 writeUs = 0;
    u64 totalUs = 0;
};

// ---------------------------------------------------------------------------
// Cook environment: the services a cook may use. Never affects cooked bytes.
// ---------------------------------------------------------------------------

struct CookEnv {
    Allocator const* alloc = nullptr; ///< nullptr = default allocator
    DiagSink const* diag   = nullptr; ///< optional
    /// Optional. Splits the heavy passes across its workers; the calling thread works
    /// too, so this is safe from inside a job of the same pool.
    JobSystem const* jobs = nullptr;
    /// Threads one cook may occupy, the caller included: 1 = inline, 0 = no cap. The
    /// default leaves the rest of the pool to the host (see docs/design/cook-kernels.md).
    u32 maxThreads = 3;
};

// ---------------------------------------------------------------------------
// Mesh cooking
// ---------------------------------------------------------------------------

/// Resolves an external URI referenced by a .gltf (buffers, images). `uri` is already
/// percent-decoded. Returns the bytes (allocated from `alloc`, Tag::Cook) or a failed
/// Status. Not needed for .glb with embedded buffers and images.
struct UriResolver {
    Status (*fn)(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) = nullptr;
    void* user                                                                  = nullptr;
};

struct MeshSource {
    Span<u8 const> bytes = {}; ///< .glb (or .gltf JSON) contents
    StrView assetPath  = {}; ///< a valid asset name, e.g. "meshes/ship_hauler_a.glb"; URIs resolve against it
    StrView sourcePath = {}; ///< for diagnostics and relative URIs, e.g. "assets/ship_hauler_a.glb"
    u64 sourceHash     = 0;  ///< xxh64 of bytes; 0 = computed by cook_mesh
    UriResolver resolver;    ///< optional
};

/// An image embedded in the source, which the mesh cook outputs as a texture of its own.
/// cook_mesh does not cook it; the caller does, using `slot` for usage inference. Views
/// point into CookedMesh::strings or the source bytes. External images are not outputs:
/// the .mesh records them as kTextureExternal bindings.
struct TextureRef {
    StrView assetPath       = {}; ///< "<mesh assetPath>#<image name>"
    Span<u8 const> embedded = {}; ///< the image bytes (points into MeshSource::bytes when possible)
    StrView mimeType        = {}; ///< "image/png", "image/jpeg", ... (may be empty)
    SlotHint slot;                ///< first slot that referenced it
    bool srgb;                    ///< as inferred from the slot
};

struct CookedMesh {
    Vec<u8> file;             ///< the .mesh bytes
    Vec<TextureRef> textures; ///< embedded images referenced, in first-reference order
    Vec<char> strings;        ///< backing store for TextureRef views (stable after cook)
    u64 sourceHash = 0;
    u32 partCount = 0, lodCount = 0;
    u32 triangleCount = 0, vertexCount = 0; ///< totals across all LOD records, not LOD0 only
    CookStats stats;
};

/// Cook a glTF/GLB into a .mesh. `settings` must be resolved (resolve_mesh).
/// Failure: Status per error-model.md, one or more K1xxx diagnostics. Warnings and
/// infos never fail the cook. In a KILN_MESH=OFF build it always fails: Unsupported, K1021.
KILN_API Result<CookedMesh> cook_mesh(MeshSource const& src, MeshCookSettings const& settings,
                                      TargetProfile const& target, CookEnv const& env = {}) noexcept;

// ---------------------------------------------------------------------------
// Texture cooking
// ---------------------------------------------------------------------------

struct TextureSource {
    Span<u8 const> bytes = {}; ///< PNG, JPEG, WebP or KTX2
    StrView assetPath    = {}; ///< e.g. "textures/hull_albedo.png"
    StrView sourcePath   = {}; ///< for diagnostics
    u64 sourceHash       = 0;  ///< 0 = computed
};

struct CookedTexture {
    Vec<u8> file; ///< the KTX2 bytes
    ktx2::TextureDesc desc;
    bool passthrough = false; ///< input was a KTX2 already suitable for the target
    u64 sourceHash   = 0;
    CookStats stats;
};

/// Cook a PNG, JPEG or WebP (decode, convert per usage, mips) or pass a suitable KTX2 through.
/// `settings` must be resolved (resolve_texture).
KILN_API Result<CookedTexture> cook_texture(TextureSource const& src, TextureCookSettings const& settings,
                                            TargetProfile const& target, CookEnv const& env = {}) noexcept;

// ---------------------------------------------------------------------------
// Store: content-hashed files, atomic writes, no index in v0.5
// ---------------------------------------------------------------------------

/// Store key = hash_combine chain of (source bytes hash, resolved settings hash,
/// target hash, cooker version).
[[nodiscard]] KILN_API u64 store_key(u64 sourceHash, u64 settingsHash, u64 targetHash,
                                     u32 cookerVersion = kCookerVersion) noexcept;

/// "<16 lowercase hex digits>.<ext>" into `out` (needs 16 + 1 + ext.size + 1 bytes).
/// Returns the length written.
KILN_API usize store_file_name(u64 key, StrView ext, char* out, usize cap) noexcept;

/// Write `bytes` to `<dir>/<name>` atomically (temp file + rename). Creates `dir`
/// if missing (one level). Without `overwrite`, an existing file with the same name is
/// left untouched (content-addressed: same key means same bytes) and Status is Ok. With
/// `overwrite` (hot-reload re-cooks, whose names are not content-addressed) the rename
/// replaces it; if the rename fails, for example because a reader on Windows holds the
/// file open without FILE_SHARE_DELETE, the old file stays and the result is IoError, so
/// the caller can retry.
KILN_API Status store_write(StrView dir, StrView name, Span<u8 const> bytes, DiagSink const* diag = nullptr,
                            bool overwrite = false) noexcept;

/// True if `<dir>/<name>` exists.
[[nodiscard]] KILN_API bool store_exists(StrView dir, StrView name) noexcept;

} // namespace kiln::cook
