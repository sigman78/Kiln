// kiln/cook/mesh_writer.h — .mesh writer: resolved records plus raw stream/index bytes
// in, complete file out (layout per mesh-format-spec §5.9). Deterministic: no timestamps,
// zeroed padding, strings deduplicated in first-seen order, mounts sorted by nameHash.
#pragma once

#include "kiln/containers.h"
#include "kiln/mesh.h"

namespace kiln::mesh {

/// Input record for PART. Strings are given as views; the writer builds STRS.
struct PartDesc {
    StrView name;
    u32 parent         = kInvalid;
    f32 translation[3] = {0, 0, 0};
    f32 rotation[4]    = {0, 0, 0, 1};
    Bounds bounds      = {};
    f32 posScale[3]    = {1, 1, 1};
    f32 posBias[3]     = {0, 0, 0};
    u32 lodFirst       = 0;
    u32 lodCount       = 0; ///< 0 for pure hierarchy nodes
    u32 flags          = 0;
};

/// Input record for LODS. The writer assigns streamOffset / indexOffset.
struct LodDesc {
    u32 layout      = 0; ///< LAYT index
    u32 vertexCount = 0;
    Span<u8 const>
        streams[kMaxStreams]; ///< stream s bytes: vertexCount * strides[s]; empty for s >= streamCount
    Span<u8 const> indices;   ///< indexCount * index_size(indexType) bytes; may be empty
    u32 indexCount      = 0;
    IndexType indexType = IndexType::U16;
    u32 submeshFirst    = 0;
    u32 submeshCount    = 0;
    f32 geometricError  = 0;
};

struct MaterialDesc {
    StrView name;
    u32 flags           = 0; ///< MaterialFlags
    u32 textureFirst    = 0;
    u32 textureCount    = 0;
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff     = 0.5f;
};

struct TextureBindingDesc {
    StrView path;         ///< cooked texture asset path (also stored as pathStr)
    u64 textureId    = 0; ///< 0 = hash_name(path)
    TextureSlot slot = TextureSlot::BaseColor;
    u8 uvSet         = 0;
    u16 flags        = 0; ///< TextureBindingFlags
};

struct MountDesc {
    StrView name;
    u32 parentPart     = kInvalid;
    f32 translation[3] = {0, 0, 0};
    f32 rotation[4]    = {0, 0, 0, 1};
    StrView extras; ///< "key=value;key=value"; empty = none (kInvalid)
};

struct WriteDesc {
    StrView name;        ///< MODL nameStr
    u64 assetId    = 0;  ///< 0 = hash_name(name)
    Bounds bounds  = {}; ///< MODL bounds
    u32 modelFlags = 0;
    u64 sourceHash = 0;
    u64 cookHash   = 0;

    Span<VertexLayout const> layouts; ///< on-disk form, validated by the writer
    Span<PartDesc const> parts;       ///< topological order (parent < self)
    Span<LodDesc const> lods;
    Span<Submesh const> submeshes; ///< on-disk form (no strings)
    Span<MaterialDesc const> materials;
    Span<TextureBindingDesc const> textures;
    Span<MountDesc const> mounts; ///< any order; the writer sorts by nameHash
};

struct WriteOptions {
    u32 payloadAlignment = kPayloadBaseAlign; ///< FileHeader::payloadAlignment (>= 256, power of two)
    bool checksums       = true;              ///< fill PayloadBlob::checksum (xxh32 of decoded bytes)
    /// 0 = one blob per stream per LOD and one per index range. Otherwise split
    /// ranges into blobs of at most this many decoded bytes, rounded down to a
    /// multiple of split_unit(unit) = lcm(unit, 16) (spec §5.9), never below one split_unit.
    u32 splitBytes = 0;
    /// Testing aid for the non-raw decode path: zero bytes inserted before every
    /// encoded blob (rounded up to a multiple of 16). Non-zero clears kPayloadRaw
    /// because encoded offsets no longer equal decoded offsets.
    u32 encodedPadding = 0;
};

struct WriteStats {
    u32 blobCount   = 0;
    u64 fileSize    = 0;
    u64 decodedSize = 0;
    bool raw        = false;
};

/// Build the file. Layout: header, section table, MODL, STRS, LAYT, PART, LODS,
/// SUBM, MATL, MTEX (if any), MNTS (if any), BLOB, then GPUD at a 256-byte
/// boundary. Blobs (and therefore the decoded payload) are ordered coarsest LOD
/// first (ascending lodRank, ties in LOD-record order), streams before indices.
/// Input problems return InvalidArgument with a K40xx diagnostic. Output memory
/// comes from `alloc` (default allocator if null) under Tag::Cook.
KILN_API Result<Vec<u8>> write(WriteDesc const& desc, WriteOptions const& opt = {},
                               Allocator const* alloc = nullptr, DiagSink const* diag = nullptr,
                               WriteStats* stats = nullptr) noexcept;

/// lcm(unit, 16): the split granularity for a vertex stream (unit = stride) or an
/// index range (unit = 3 * index size), spec §5.9.
[[nodiscard]] constexpr u32 split_unit(u32 unit) noexcept {
    u32 a = unit, b = kBlobAlign;
    while (b) {
        u32 t = a % b;
        a     = b;
        b     = t;
    }
    return unit / a * kBlobAlign; // unit * 16 / gcd
}

} // namespace kiln::mesh
