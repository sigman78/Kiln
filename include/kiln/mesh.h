// kiln/mesh.h — .mesh runtime format (docs/mesh-format-spec.md, v0.3): on-disk records,
// zero-copy reader, validation, payload decode. The writer is kiln/cook/mesh_writer.h.
#pragma once

#include "kiln/alloc.h" // Arena (decode scratch)
#include "kiln/formats.h"
#include "kiln/hash.h"
#include "kiln/result.h"

namespace kiln::mesh {

// ===========================================================================
// On-disk format (spec §4-§5). Every struct is POD and little-endian.
// ===========================================================================

inline constexpr u32 kMagic            = fourcc('K', 'M', 'S', 'H');
inline constexpr u16 kVersionMajor     = 0; ///< mismatch = VersionMismatch
inline constexpr u16 kVersionMinor     = 4; ///< 0.x: exact match required; from 1.0: additive
inline constexpr u32 kMaxStreams       = 4;
inline constexpr u32 kMaxAttribs       = 12;
inline constexpr u32 kPayloadBaseAlign = 256; ///< minimum payloadAlignment / gpuDataOffset alignment
inline constexpr u32 kBlobAlign        = 16;  ///< alignment of every encoded and decoded blob range

enum HeaderFlags : u32 {
    kPayloadRaw = 1u << 0, ///< encoded layout == decoded layout; per-blob conditions in spec §5.9
};

struct FileHeader {
    u32 magic;
    u16 versionMajor;
    u16 versionMinor;
    u32 flags;        ///< HeaderFlags
    u32 sectionCount; ///< includes BLOB and GPUD
    u64 fileSize;
    u64 sectionTableOffset; ///< → SectionEntry[sectionCount]
    u64 sourceHash;         ///< xxh64 of the source file bytes
    u64 cookHash;           ///< hash_combine(settings hash, target hash, cooker version)
    u64 gpuDataOffset;      ///< == GPUD section offset
    u64 gpuDataSize;        ///< == GPUD section size == fileSize - gpuDataOffset
    u64 payloadDecodedSize; ///< bytes the loader must allocate for the GPU payload
    u32 payloadAlignment;   ///< required alignment of the decoded payload base (>= 256)
    u32 _reserved;
};
static_assert(sizeof(FileHeader) == 80);

struct SectionEntry {
    u32 id;     ///< fourcc
    u32 flags;  ///< reserved, 0
    u64 offset; ///< from file start
    u64 size;
    u32 count;  ///< element count (0 for blobs: STRS, GPUD)
    u32 stride; ///< element size AS WRITTEN
};
static_assert(sizeof(SectionEntry) == 32);

inline constexpr u32 kSecModel     = fourcc('M', 'O', 'D', 'L');
inline constexpr u32 kSecStrings   = fourcc('S', 'T', 'R', 'S');
inline constexpr u32 kSecLayouts   = fourcc('L', 'A', 'Y', 'T');
inline constexpr u32 kSecParts     = fourcc('P', 'A', 'R', 'T');
inline constexpr u32 kSecLods      = fourcc('L', 'O', 'D', 'S');
inline constexpr u32 kSecSubmeshes = fourcc('S', 'U', 'B', 'M');
inline constexpr u32 kSecMaterials = fourcc('M', 'A', 'T', 'L');
inline constexpr u32 kSecTextures  = fourcc('M', 'T', 'E', 'X');
inline constexpr u32 kSecMounts    = fourcc('M', 'N', 'T', 'S');
inline constexpr u32 kSecBlobs     = fourcc('B', 'L', 'O', 'B');
inline constexpr u32 kSecGpuData   = fourcc('G', 'P', 'U', 'D');
// Reserved (spec §9): SKIN MORF MLET COLL XTRA

struct Bounds {
    f32 center[3];
    f32 radius;         ///< bounding sphere around center
    f32 halfExtents[3]; ///< AABB around center
    f32 _pad;
};
static_assert(sizeof(Bounds) == 32);

struct ModelInfo {
    Bounds bounds; ///< model space, all parts at rest pose, LOD0
    u32 nameStr;   ///< asset name, e.g. "ship_hauler_a"
    u32 flags;     ///< reserved
    u64 assetId;   ///< FNV-1a 64 of cooked asset path
};
static_assert(sizeof(ModelInfo) == 48);

enum class Semantic : u8 {
    Position = 0,
    Normal   = 1, ///< 2-comp format => octahedral; 3/4-comp => raw xyz
    Tangent  = 2, ///< 4-comp: xyz + w = bitangent sign
    TexCoord = 3, ///< semanticIndex = UV set
    Color    = 4, ///< semanticIndex = COLOR_n
    Joints   = 5, ///< reserved
    Weights  = 6, ///< reserved
    Custom   = 7, ///< semanticIndex = user channel
};

struct VertexAttrib {
    u8 semantic; ///< Semantic
    u8 semanticIndex;
    u8 stream; ///< 0..streamCount-1
    u8 _pad;
    u32 format; ///< kiln::Format (== VkFormat value)
    u16 offset; ///< byte offset within stream element
    u16 _pad2;
};
static_assert(sizeof(VertexAttrib) == 12);

struct VertexLayout {
    u8 streamCount; ///< 1..kMaxStreams
    u8 attribCount; ///< 1..kMaxAttribs
    u16 flags;      ///< reserved
    u16 strides[kMaxStreams];
    VertexAttrib attribs[kMaxAttribs];
    u32 _reserved;
};
static_assert(sizeof(VertexLayout) == 160);

struct MeshPart {
    u32 nameStr;
    u32 parent; ///< PART index or kInvalid (model root)
    u64 nameHash;
    f32 translation[3]; ///< local, relative to parent
    f32 rotation[4];    ///< local quaternion xyzw
    Bounds bounds;      ///< part local space, LOD0
    f32 posScale[3];    ///< position = decoded * posScale + posBias
    f32 posBias[3];
    u32 lodFirst; ///< into LODS
    u32 lodCount; ///< 0 for pure hierarchy nodes
    u32 flags;    ///< reserved
};
static_assert(sizeof(MeshPart) == 112);

/// kiln's cooker writes U16 or U32, never U8. U8 exists for other writers; a host whose API has no
/// 8-bit indices (D3D11, Metal, WebGPU, sokol) may reject it.
enum class IndexType : u8 { U16 = 0, U32 = 1, U8 = 2 };

[[nodiscard]] constexpr u32 index_size(IndexType t) noexcept {
    return t == IndexType::U16 ? 2u : t == IndexType::U32 ? 4u : t == IndexType::U8 ? 1u : 0u;
}

struct MeshLod {
    u32 layout; ///< LAYT index
    u32 vertexCount;
    u32 streamOffset[kMaxStreams]; ///< into DECODED payload; unused streams = kInvalid
    u32 indexOffset;               ///< into DECODED payload, multiple of index size
    u32 indexCount;                ///< total for this LOD
    u8 indexType;                  ///< IndexType
    u8 _pad[3];
    u32 submeshFirst; ///< into SUBM
    u32 submeshCount;
    f32 geometricError; ///< meters; 0 for LOD0 and authored LODs
};
static_assert(sizeof(MeshLod) == 48);

struct Submesh {
    u32 material;   ///< MATL index
    u32 indexFirst; ///< relative to the LOD's index range
    u32 indexCount;
    i32 vertexBase; ///< vertexOffset for vkCmdDrawIndexed
    Bounds bounds;  ///< part local space
};
static_assert(sizeof(Submesh) == 48);

enum class AlphaMode : u8 { Opaque = 0, Mask = 1, Blend = 2 };

enum MaterialFlags : u32 {
    kMaterialVertexColor = 1u << 0,
    kMaterialDoubleSided = 1u << 1,
};

struct MaterialSlot {
    u32 nameStr;
    u32 flags;        ///< MaterialFlags
    u64 nameHash;     ///< key into engine material library
    u32 textureFirst; ///< into MTEX
    u32 textureCount;
    u8 alphaMode; ///< AlphaMode
    u8 _pad[3];
    f32 alphaCutoff;
};
static_assert(sizeof(MaterialSlot) == 32);

enum class TextureSlot : u8 {
    BaseColor  = 0,
    Normal     = 1,
    MetalRough = 2,
    Occlusion  = 3,
    Emissive   = 4,
    // 5..15 reserved for engine-defined slots
};

enum TextureBindingFlags : u16 {
    kTextureSrgb = 1u << 0,
    /// pathStr is an external image's URI, percent-decoded and relative to the source file;
    /// textureId is 0. kiln does not cook or name it: the host maps it to a texture.
    kTextureExternal = 1u << 1,
};

struct TextureBinding {
    u64 textureId; ///< FNV-1a 64 of the texture asset path; 0 with kTextureExternal
    u32 pathStr;   ///< the texture asset path, or the URI with kTextureExternal
    u8 slot;       ///< TextureSlot
    u8 uvSet;      ///< which TexCoord semanticIndex to sample
    u16 flags;     ///< TextureBindingFlags
};
static_assert(sizeof(TextureBinding) == 16);

struct Mount {
    u32 nameStr;
    u32 parentPart; ///< PART index or kInvalid (model root)
    u64 nameHash;
    f32 translation[3]; ///< relative to parent part
    f32 rotation[4];    ///< xyzw; +Z = slot forward, +Y = slot up
    u32 extrasStr;      ///< "key=value;key=value" or kInvalid
};
static_assert(sizeof(Mount) == 48);

enum class Codec : u8 {
    None            = 0,
    Zstd            = 1,
    MeshoptVertex   = 2,
    MeshoptIndex    = 3,
    MeshoptIndexSeq = 4,
    // 5..127 reserved; 128..255 vendor/experimental
};

enum class Filter : u8 {
    None        = 0,
    ByteShuffle = 1,
    Delta       = 2, ///< reserved: lane width unspecified
    MeshoptOct  = 3, ///< reserved
    MeshoptQuat = 4, ///< reserved
    MeshoptExp  = 5, ///< reserved
};

enum BlobFlags : u16 {
    kBlobOuterZstd = 1u << 0, ///< encoded bytes are Zstd(codec output); Meshopt* codecs only
};

struct PayloadBlob {
    u32 encodedOffset; ///< into GPUD, 16-B aligned; ranges never overlap
    u32 encodedSize;
    u32 decodedOffset; ///< into decoded payload, 16-B aligned; ranges never overlap
    u32 decodedSize;
    u16 elementSize; ///< vertex stride or index size; never 0
    u8 codec;        ///< Codec
    u8 filter;       ///< Filter
    u16 flags;       ///< BlobFlags
    u8 lodRank;      ///< lodCount(part) - 1 - lodIndex, capped at 255
    u8 _pad;
    u32 checksum; ///< xxh32 of decoded bytes (0 = not stored)
    u32 _reserved;
};
static_assert(sizeof(PayloadBlob) == 32);

// Enum <-> string (tools / diagnostics)
[[nodiscard]] KILN_API char const* semantic_name(Semantic s) noexcept;
[[nodiscard]] KILN_API char const* index_type_name(IndexType t) noexcept;
[[nodiscard]] KILN_API char const* alpha_mode_name(AlphaMode m) noexcept;
[[nodiscard]] KILN_API char const* texture_slot_name(TextureSlot s) noexcept;
[[nodiscard]] KILN_API char const* codec_name(Codec c) noexcept;
[[nodiscard]] KILN_API char const* filter_name(Filter f) noexcept;

/// True if the codec/filter pair is permitted by spec §5.9.
[[nodiscard]] constexpr bool is_allowed_blob_encoding(Codec c, Filter f) noexcept {
    switch (c) {
    case Codec::None:
    case Codec::Zstd: return f == Filter::None || f == Filter::ByteShuffle || f == Filter::Delta;
    case Codec::MeshoptVertex:
        return f == Filter::None || f == Filter::MeshoptOct || f == Filter::MeshoptQuat ||
               f == Filter::MeshoptExp;
    case Codec::MeshoptIndex:
    case Codec::MeshoptIndexSeq: return f == Filter::None;
    }
    return false;
}
[[nodiscard]] constexpr bool is_meshopt_codec(Codec c) noexcept {
    return c == Codec::MeshoptVertex || c == Codec::MeshoptIndex || c == Codec::MeshoptIndexSeq;
}

// ===========================================================================
// Diagnostic codes (K4000-K4999: .mesh / KTX2 validation). See docs/diagnostics.md.
// ===========================================================================

enum DiagCode : u32 {
    kDiagBadMagic       = 4001, ///< magic != KMSH
    kDiagVersion        = 4002, ///< major mismatch, or minor mismatch while 0.x
    kDiagHeaderSizes    = 4003, ///< fileSize / gpuDataOffset / gpuDataSize / payloadDecodedSize inconsistent
    kDiagSectionTable   = 4004, ///< section table out of bounds or malformed
    kDiagSectionMissing = 4005, ///< a required section is absent or has count 0 where >= 1 is required
    kDiagSectionBounds =
        4006, ///< section range outside the file / CPU region, overlapping GPUD, or misaligned
    kDiagRecordStride = 4007, ///< stride smaller than the record, not 4-aligned, or count*stride != size
    kDiagStringOffset = 4008, ///< string offset outside STRS or not null-terminated
    kDiagIndexRange   = 4009, ///< cross-reference index out of range (part->lod, lod->submesh, ...)
    kDiagLayoutRule   = 4010, ///< layout: stream 0 not position-only, strides not 4-aligned, attrib out of
                              ///< stream, unknown format
    kDiagPartOrder = 4011,    ///< parent >= self, or lodCount without lods
    kDiagLodRange  = 4012,    ///< stream/index range outside the decoded payload or not covered by blobs
    kDiagBlobTable = 4013,    ///< BLOB unsorted, overlapping or outside GPUD (Corrupt)
    kDiagBlobEncoding =
        4014, ///< codec/filter pair, outer Zstd, U8 with meshopt, elementSize rules (ValidationFailed)
    kDiagBlobUnsupported = 4015, ///< codec or filter not built into this runtime (Unsupported)
    kDiagPayloadRaw      = 4016, ///< kPayloadRaw set but identity conditions violated (Corrupt)
    kDiagChecksum        = 4017, ///< decoded bytes do not match blob checksum (Corrupt)
    kDiagIndexAlign      = 4018, ///< indexOffset not a multiple of the index size
    kDiagMountOrder      = 4019, ///< mounts not sorted by nameHash
    kDiagIndexValue      = 4020, ///< an index value >= vertexCount (tools check)
    kDiagDecodeSize      = 4021, ///< decoder produced a size != decodedSize (Corrupt)
    kDiagTruncated       = 4022, ///< buffer shorter than the header says
    kDiagBufferAlignment = 4023, ///< caller buffer not 8-byte aligned (InvalidArgument)
};

// ===========================================================================
// Reader
// ===========================================================================

/// Strided accessor over a record section (spec §4). `operator[]` returns a reference
/// and needs stride >= sizeof(T), which open() guarantees. `get()` copies
/// min(stride, sizeof(T)) bytes and zero-fills the rest (records from a newer minor).
template <class T> class Records {
public:
    constexpr Records() noexcept = default;
    constexpr Records(u8 const* base, u32 count, u32 stride) noexcept
        : base_(base), count_(count), stride_(stride) {}

    [[nodiscard]] constexpr u32 size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr u32 stride() const noexcept { return stride_; }
    [[nodiscard]] constexpr bool contiguous() const noexcept { return stride_ == sizeof(T); }

    [[nodiscard]] T const& operator[](u32 i) const noexcept {
        KILN_ASSERT(i < count_ && stride_ >= sizeof(T));
        return *reinterpret_cast<T const*>(base_ + usize(i) * stride_);
    }
    [[nodiscard]] T get(u32 i) const noexcept {
        KILN_ASSERT(i < count_);
        T out{};
        u32 n = stride_ < sizeof(T) ? stride_ : u32(sizeof(T));
        std::memcpy(&out, base_ + usize(i) * stride_, n);
        return out;
    }
    /// Contiguous view; only valid when contiguous().
    [[nodiscard]] Span<T const> span() const noexcept {
        KILN_ASSERT(contiguous());
        return {reinterpret_cast<T const*>(base_), count_};
    }

    struct Iter {
        u8 const* p;
        u32 stride;
        T const& operator*() const noexcept { return *reinterpret_cast<T const*>(p); }
        Iter& operator++() noexcept {
            p += stride;
            return *this;
        }
        bool operator!=(Iter const& o) const noexcept { return p != o.p; }
    };
    [[nodiscard]] Iter begin() const noexcept { return {base_, stride_}; }
    [[nodiscard]] Iter end() const noexcept { return {base_ + usize(count_) * stride_, stride_}; }

private:
    u8 const* base_ = nullptr;
    u32 count_      = 0;
    u32 stride_     = 0;
};

struct OpenOptions {
    /// Full structural validation (cross references, strings, layouts, lod ranges,
    /// mounts). Header, section and BLOB checks always run. Default on; hosts may
    /// turn it off for trusted, pre-validated stores.
    bool validate = true;
};

/// Zero-copy view over the CPU region of a .mesh file ([0, gpuDataOffset)). The
/// bytes must outlive the view. If the span also contains GPUD, `encoded()` is set.
class KILN_API MeshView {
public:
    /// Parse and validate. `bytes` must hold at least the CPU region. On failure a
    /// diagnostic (code K40xx) is emitted and the Status carries the error class.
    static Result<MeshView> open(Span<u8 const> bytes, OpenOptions const& opt = {},
                                 DiagSink const* diag = nullptr, StrView assetName = {}) noexcept;

    [[nodiscard]] FileHeader const& header() const noexcept { return *header_; }
    [[nodiscard]] Span<SectionEntry const> sections() const noexcept { return sections_; }
    [[nodiscard]] SectionEntry const* find_section(u32 id) const noexcept;

    [[nodiscard]] ModelInfo const& model() const noexcept { return *model_; }
    [[nodiscard]] StrView name() const noexcept { return str(model_->nameStr); }
    [[nodiscard]] u64 asset_id() const noexcept { return model_->assetId; }

    [[nodiscard]] Records<VertexLayout> const& layouts() const noexcept { return layouts_; }
    [[nodiscard]] Records<MeshPart> const& parts() const noexcept { return parts_; }
    [[nodiscard]] Records<MeshLod> const& lods() const noexcept { return lods_; }
    [[nodiscard]] Records<Submesh> const& submeshes() const noexcept { return submeshes_; }
    [[nodiscard]] Records<MaterialSlot> const& materials() const noexcept { return materials_; }
    [[nodiscard]] Records<TextureBinding> const& textures() const noexcept { return textures_; }
    [[nodiscard]] Records<Mount> const& mounts() const noexcept { return mounts_; }
    [[nodiscard]] Records<PayloadBlob> const& blobs() const noexcept { return blobs_; }

    /// String by STRS offset. Offsets are validated by open() when `validate` is on;
    /// an out-of-range offset returns an empty view. `kInvalid` returns empty.
    [[nodiscard]] StrView str(u32 offset) const noexcept;
    [[nodiscard]] Span<u8 const> strings() const noexcept { return strings_; }

    /// GPUD bytes if they were part of the span given to open(), else empty.
    [[nodiscard]] Span<u8 const> encoded() const noexcept { return encoded_; }
    [[nodiscard]] bool payload_raw() const noexcept { return (header_->flags & kPayloadRaw) != 0; }
    [[nodiscard]] u64 decoded_size() const noexcept { return header_->payloadDecodedSize; }
    [[nodiscard]] u32 payload_alignment() const noexcept { return header_->payloadAlignment; }

    /// Binary search over mounts (sorted by nameHash). nullptr if absent.
    [[nodiscard]] Mount const* find_mount(u64 nameHash) const noexcept;
    /// Linear search over parts. nullptr if absent.
    [[nodiscard]] MeshPart const* find_part(u64 nameHash) const noexcept;

    /// Byte size of stream `s` / the index range of a LOD (0 for unused streams).
    [[nodiscard]] u64 stream_bytes(MeshLod const& lod, u32 s) const noexcept;
    [[nodiscard]] static u64 index_bytes(MeshLod const& lod) noexcept {
        return u64(lod.indexCount) * index_size(IndexType(lod.indexType));
    }

private:
    FileHeader const* header_ = nullptr;
    Span<SectionEntry const> sections_;
    ModelInfo const* model_ = nullptr;
    Span<u8 const> strings_;
    Span<u8 const> encoded_;
    Records<VertexLayout> layouts_;
    Records<MeshPart> parts_;
    Records<MeshLod> lods_;
    Records<Submesh> submeshes_;
    Records<MaterialSlot> materials_;
    Records<TextureBinding> textures_;
    Records<Mount> mounts_;
    Records<PayloadBlob> blobs_;
};

// ===========================================================================
// Payload decode (spec §5.9, §7)
// ===========================================================================

struct DecodeOptions {
    bool verifyChecksums = KILN_DEBUG != 0; ///< check PayloadBlob.checksum when non-zero
};

/// Decode one blob: `encoded` is exactly the blob's encoded range, `dst` exactly
/// blob.decodedSize bytes. v0.5 supports Codec::None / Filter::None; anything else
/// returns Unsupported. Compressed codecs will use `scratch` for intermediates.
KILN_API Status decode_blob(PayloadBlob const& blob, Span<u8 const> encoded, Span<u8> dst,
                            DecodeOptions const& opt = {}, DiagSink const* diag = nullptr,
                            Arena* scratch = nullptr, StrView assetName = {}) noexcept;

/// Decodes every blob of `v` from `gpud` (the GPUD bytes, gpuDataSize long) into `dst`
/// (>= payloadDecodedSize) and zero-fills the gaps. kPayloadRaw files take a single
/// memcpy. Loaders that read blobs one by one call decode_blob() instead.
KILN_API Status decode_payload(MeshView const& v, Span<u8 const> gpud, Span<u8> dst,
                               DecodeOptions const& opt = {}, DiagSink const* diag = nullptr,
                               Arena* scratch = nullptr, StrView assetName = {}) noexcept;

/// Tools-only check: every index value < vertexCount and every submesh's indices
/// (plus vertexBase) address existing vertices. Needs the decoded payload.
KILN_API Status check_indices(MeshView const& v, Span<u8 const> decoded, DiagSink const* diag = nullptr,
                              StrView assetName = {}) noexcept;

} // namespace kiln::mesh
