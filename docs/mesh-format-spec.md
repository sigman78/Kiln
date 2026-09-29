# kiln `.mesh` — Cooked Runtime Mesh Format

**Status:** draft v0.4 · **Target:** Vulkan 1.4, C++23, little-endian only
**Produced by:** `kiln-cook` from glTF 2.0 sources (`.glb`, or `.gltf` with external or data-URI buffers and images). **Never** hand-authored, never edited.

The format originated as Orbital's `.mesh` (magic `OMSH`); kiln adopts it under its own magic and namespace, and no `OMSH` files need to be read.

> **v0.2 changes:**
> - The GPU payload is now described by a **blob table** (`BLOB`, §5.9), which separates encoded file ranges from decoded GPU-payload ranges and allows optional per-range compression (Zstd, meshopt codecs, byte-shuffle filtering).
> - The header grows to 80 bytes and records both encoded and decoded payload sizes.
> - Draw-facing offsets (`MeshLod`) now refer to the **decoded** payload.
> - The v0.5 cooker emits uncompressed blobs only, and loaders must support at least codec `None`.

> **v0.4 changes:** `TextureBinding.flags` bit1 `External` (§5.7): the binding names an image the source references by URI, which the cooker does not cook. Embedded images are named `<mesh asset name>#<image name>`, where the mesh asset name includes its extension (e.g. `meshes/ship.glb#hull_albedo`). `kVersionMinor` is 4.

> **v0.3 changes:** (resolves `docs/open-questions.md` B1-B27)
> - Magic `KMSH`, namespace `kiln::mesh`, producer `kiln-cook` (B1). `kVersionMinor` is 3.
> - Version rule: while `versionMajor == 0` a loader requires an exact `versionMinor` match and reports `VersionMismatch` otherwise; the additive minor rule applies from 1.0 (B26).
> - Vertex formats are stored as `kiln::Format`, numerically equal to `VkFormat`; the bytes are unchanged (B2).
> - `sourceHash` and `cookHash` defined precisely (B3). Readers use a strided accessor when `stride != sizeof(T)` (B4).
> - `static_assert` on every on-disk record, now also `Bounds`, `VertexAttrib`, `ModelInfo`, `Submesh`, `MaterialSlot`, `TextureBinding`, `Mount` (B12).
> - `Mount.extrasStr` grammar: no `;` or `=` inside keys or values, scalar extras only (B5). Authored LODs get `geometricError = 0` (B6). The `vertexBase` limit covers every index size; the v0.5 cooker never emits U8 (B7). The sRGB bit of `TextureBinding.flags` comes from slot inference (B11). Sparse accessors and Draco input are rejected; `.gltf` with external files is accepted (B9, B10).
> - §8 draw call uses `lod.indexOffset / indexSize + sm.indexFirst` (B8).
> - `BLOB`: one filter per blob, chained-filter sentence removed (B13); filters apply before the codec on encode and are undone after it on decode, with an allowed codec/filter table (B14); `kBlobOuterZstd` only with `Meshopt*` codecs (B15).
> - `BLOB`: split rules deferred to v0.6, the v0.5 cooker never splits (B16); index blob size, alignment and U8 rules (B17); `elementSize` is always set (follows from B16, B17).
> - `BLOB`: encoded ranges never overlap, the table is sorted by `encodedOffset` with non-decreasing `lodRank`, decoded order is free (B18); `lodRank = lodCount(part) - 1 - lodIndex`, capped at 255 (B22).
> - `gpuDataSize == GPUD size == fileSize - gpuDataOffset`, padding included; with `kPayloadRaw` also `== payloadDecodedSize` (B19). `BLOB` is always required; exact per-blob conditions for `kPayloadRaw`, validated on load (B20).
> - Decoder output must be exactly `decodedSize`; short output is `Corrupt` (B21). Checksum 0 means "not stored" even if the real xxh32 is 0 (B23, B27). Encoded and decoded payloads are each limited to 4 GiB (B24).
> - An unsupported codec or filter sends the mesh to `Failed` with `Unsupported`; there is no mesh placeholder (B25).
> - §7 loading and validation updated to match; answered §10 questions removed.

## 1. Goals and non-goals

**Goals**

- **Load = 2 reads.** One read gets the header plus all metadata. A second read streams the GPU payload into a staging or ReBAR buffer. Uncompressed payloads land there directly with no per-vertex CPU work; compressed ones are decoded straight into it.
- **Optional compression without changing draw data.** Encoding is described per payload range (§5.9), so compressed and uncompressed files produce identical decoded payloads and identical draw records.
- **Flat.** Fixed-size POD records, index-based references, offsets instead of pointers, one string table.
- **Flexible where it matters:**
  - multiple vertex layouts (quantized or float, optional streams and UV sets)
  - multiple UV mappings per material
  - named parts and mounts
  - LOD chains
- **Forward-compatible.** A section directory with per-section element stride lets new sections and appended fields be added without breaking old loaders (from 1.0; see the version rules in §4).
- **Works on pre-mesh-shader hardware.** Plain indexed draws. Meshlets are a reserved optional section.

**Non-goals (for now)**

- skinning and animation
- morph targets
- collision
- meshlets
- deciding the default compression scheme. The format supports codecs (§5.9), but which ones the cooker uses by default is to be decided with measurements.
- glTF sparse accessors and Draco (`KHR_draco_mesh_compression`) input. The cooker rejects both with a K1xxx diagnostic.

Section IDs for these are reserved (§9).

---

## 2. File layout

```
+--------------------------+  0
| FileHeader (80 B)        |
+--------------------------+
| SectionEntry[N]          |  sectionTableOffset
+--------------------------+
| metadata sections        |  MODL, STRS, LAYT, PART, LODS, SUBM, MATL, MTEX, MNTS, BLOB …
| (each 16-B aligned)      |  contiguous: [0, gpuDataOffset) is "CPU region"
+--------------------------+
| GPUD encoded payload     |  gpuDataOffset (256-B aligned), gpuDataSize (encoded bytes, padding included)
|  blobs (maybe compressed)|  every blob 16-B aligned inside
+--------------------------+  fileSize
```

**Two address spaces:**

- **Encoded payload:** the bytes stored in `GPUD`. Blob file ranges (`BLOB.encodedOffset`) point here.
- **Decoded payload:** the GPU-ready buffer the loader produces, `payloadDecodedSize` bytes. Everything draw-facing (`MeshLod.streamOffset`, `MeshLod.indexOffset`) points here.
- When the header flag `kPayloadRaw` is set (conditions in §5.9), the two are byte-identical. The loader can then read `GPUD` straight into GPU-visible memory in one read.

**Invariants the cooker guarantees:**

1. All metadata sections lie in `[0, gpuDataOffset)`. `GPUD` is the last section, and `gpuDataSize == GPUD SectionEntry.size == fileSize - gpuDataOffset` (padding between blobs and at the tail included).
2. `gpuDataOffset % 256 == 0`. Every blob's encoded range starts 16-byte aligned relative to the GPUD start.
3. Decoded offsets (streams, indices) are **relative to the decoded payload start**, so the loader can place the payload anywhere in a larger GPU buffer and just add a base offset or address. Every decoded range, including every split blob, starts 16-byte aligned (split rule in §5.9).
4. Index ranges in the decoded payload are multiples of the index size (a Vulkan requirement). This holds per LOD and per index blob (§5.9).
5. The `BLOB` table covers every decoded byte range that `LODS` references. Neither decoded nor encoded ranges overlap. Decoded gaps (alignment padding) are zero.
6. `BLOB` entries are sorted by `encodedOffset`, and `lodRank` is non-decreasing along the table, so blobs are stored coarsest LOD first. A coarse LOD of every part can be range-read on its own for progressive loading.
7. Every cross-reference index is in range, and every string offset points at a null-terminated string inside `STRS`.
8. The encoded payload and the decoded payload are each at most 4 GiB (§5.9, size limits).

---

## 3. Conventions

| Item | Rule |
|---|---|
| Endianness | Little-endian |
| Coordinates | Right-handed, +Y up, +Z forward, meters (same as glTF / artist guide) |
| Winding | Counter-clockwise front faces |
| Quaternions | `x, y, z, w` |
| Names | UTF-8 in `STRS`, plus a 64-bit hash stored next to each name |
| Name hash | FNV-1a 64 over the exact name bytes (case-sensitive, no terminator) |
| Asset / texture IDs | FNV-1a 64 of the asset name, e.g. `"meshes/ship_hauler_a.glb#hull_albedo"` (an embedded image) |
| Invalid index | `0xFFFFFFFF` (`kInvalid`) |
| Formats | Vertex formats are stored as `kiln::Format` values, which are numerically equal to `VkFormat`. A Vulkan renderer casts them directly; other renderers map them in their adapter. |

---

## 4. Header and section directory

```cpp
namespace kiln::mesh {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 |
           uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t kMagic        = fourcc('K','M','S','H');
constexpr uint16_t kVersionMajor = 0;   // mismatch = VersionMismatch
constexpr uint16_t kVersionMinor = 4;   // 0.x: exact match required; from 1.0: additive, loader tolerates newer
constexpr uint32_t kInvalid      = 0xFFFFFFFFu;
constexpr uint32_t kMaxStreams   = 4;
constexpr uint32_t kMaxAttribs   = 12;

enum HeaderFlags : uint32_t {
    kPayloadRaw = 1u << 0,          // encoded layout == decoded layout; per-blob conditions in §5.9
};

struct FileHeader {                 // 80 bytes
    uint32_t magic;                 // kMagic
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t flags;                 // HeaderFlags
    uint32_t sectionCount;          // includes BLOB and GPUD
    uint64_t fileSize;
    uint64_t sectionTableOffset;    // → SectionEntry[sectionCount]
    uint64_t sourceHash;            // xxh64 of the source file bytes (rebuild detection)
    uint64_t cookHash;              // hash_combine(settings hash, target hash, cooker version)
    uint64_t gpuDataOffset;         // == GPUD section offset
    uint64_t gpuDataSize;           // == GPUD section size == fileSize - gpuDataOffset (padding included)
    uint64_t payloadDecodedSize;    // bytes the loader must allocate for the GPU payload
    uint32_t payloadAlignment;      // required alignment of the decoded payload base (≥ 256)
    uint32_t _reserved;
};
static_assert(sizeof(FileHeader) == 80);

struct SectionEntry {               // 32 bytes
    uint32_t id;                    // fourcc
    uint32_t flags;                 // reserved, 0
    uint64_t offset;                // from file start
    uint64_t size;                  // bytes
    uint32_t count;                 // element count (0 for blobs: STRS, GPUD)
    uint32_t stride;                // element size AS WRITTEN (≥ what old loaders know)
};
static_assert(sizeof(SectionEntry) == 32);

} // namespace kiln::mesh
```

**Hashes:**

- `sourceHash` is `xxh64` of the source file bytes.
- `cookHash` is `hash_combine` of the settings hash, the target hash and the cooker version, that is the store key without the source part (see `design/settings.md`).

**Version rules:**

- A loader rejects a file whose `magic` is not `kMagic` (`Corrupt`) or whose `versionMajor` differs from its own (`VersionMismatch`).
- While `versionMajor == 0`, any minor bump may be breaking. A loader requires `versionMinor == kVersionMinor` exactly and reports `VersionMismatch` otherwise. Old files are re-cooked, because the cooker version is part of the store key.
- From 1.0, minor bumps are additive, and a loader tolerates a newer minor under the forward compatibility rules below.

**Forward compatibility rules:**

- A loader skips unknown section IDs.
- For known sections, the loader reads element `i` at `offset + i * stride` and uses `min(stride, sizeof(T))` bytes, zero-filling the rest. This means new fields may only be **appended** to records, and only with a minor version bump.
- Readers expose records through a strided accessor that does exactly this, with a plain contiguous-span fast path when `stride == sizeof(T)` (§7, metadata access).
- A **required** new feature, i.e. one old loaders cannot safely ignore, needs a major version bump.

---

## 5. Sections

| ID | Count | Record | Required | Purpose |
|---|---|---|---|---|
| `MODL` | 1 | `ModelInfo` | ✔ | Asset-level bounds, name |
| `STRS` | — | blob | ✔ | Null-terminated UTF-8 strings |
| `LAYT` | n | `VertexLayout` | ✔ | Vertex formats used in this file |
| `PART` | n | `MeshPart` | ✔ | Named parts, hierarchy, dequantization |
| `LODS` | n | `MeshLod` | ✔ | Vertex/index ranges per part LOD |
| `SUBM` | n | `Submesh` | ✔ | Index range + material per LOD |
| `MATL` | n | `MaterialSlot` | ✔ | Material names (engine remaps) |
| `MTEX` | n | `TextureBinding` | | Texture + UV-set mappings per material |
| `MNTS` | n | `Mount` | | Named mount slots |
| `BLOB` | n | `PayloadBlob` | ✔ | Encoded file range → decoded payload range, codec, filter (required also with `kPayloadRaw`) |
| `GPUD` | — | blob | ✔ | Encoded vertex streams and index buffers |

### 5.1 Common types

```cpp
struct Bounds {                     // 32 bytes
    float center[3];
    float radius;                   // bounding sphere around center
    float halfExtents[3];           // AABB around center
    float _pad;
};
static_assert(sizeof(Bounds) == 32);
```

### 5.2 `MODL` — model info

```cpp
struct ModelInfo {                  // 48 bytes
    Bounds   bounds;                // model space, all parts at rest pose, LOD0
    uint32_t nameStr;               // asset name, e.g. "ship_hauler_a"
    uint32_t flags;                 // reserved
    uint64_t assetId;               // FNV-1a 64 of asset name
};
static_assert(sizeof(ModelInfo) == 48);
```

### 5.3 `LAYT` — vertex layouts

Layouts are shared by LODs; a typical file has one or two. Each layout describes up to 4 **streams** (separate GPU ranges) and up to 12 attributes.

```cpp
enum class Semantic : uint8_t {
    Position = 0,   // see quantization rules §6
    Normal   = 1,   // 2-comp format ⇒ octahedral; 3/4-comp ⇒ raw xyz
    Tangent  = 2,   // 4-comp: xyz + w = bitangent sign (±1)
    TexCoord = 3,   // semanticIndex = UV set (TEXCOORD_n)
    Color    = 4,   // semanticIndex = COLOR_n
    Joints   = 5,   // reserved (skinning)
    Weights  = 6,   // reserved (skinning)
    Custom   = 7,   // semanticIndex = user channel; meaning defined by material
};

struct VertexAttrib {               // 12 bytes
    uint8_t  semantic;              // Semantic
    uint8_t  semanticIndex;
    uint8_t  stream;                // 0..streamCount-1
    uint8_t  _pad;
    uint32_t format;                // kiln::Format (== VkFormat value)
    uint16_t offset;                // byte offset within stream element
    uint16_t _pad2;
};
static_assert(sizeof(VertexAttrib) == 12);

struct VertexLayout {               // 160 bytes
    uint8_t      streamCount;       // 1..kMaxStreams
    uint8_t      attribCount;       // 1..kMaxAttribs
    uint16_t     flags;             // reserved
    uint16_t     strides[kMaxStreams];
    VertexAttrib attribs[kMaxAttribs];
    uint32_t     _reserved;
};
static_assert(sizeof(VertexLayout) == 160);
```

**Rules:**

- Stream 0 **always** contains Position and nothing else, so depth and shadow passes bind one tightly packed stream.
- Attributes within a stream are interleaved.
- Strides are multiples of 4.

### 5.4 `PART` — mesh parts

```cpp
struct MeshPart {                   // 112 bytes
    uint32_t nameStr;
    uint32_t parent;                // PART index or kInvalid (model root)
    uint64_t nameHash;
    float    translation[3];        // local, relative to parent
    float    rotation[4];           // local quaternion xyzw (scale is baked; always 1)
    Bounds   bounds;                // part local space, LOD0
    float    posScale[3];           // position = decoded * posScale + posBias
    float    posBias[3];            //   (1,1,1)/(0,0,0) for float positions
    uint32_t lodFirst;              // into LODS
    uint32_t lodCount;              // ≥ 1, LOD0 first
    uint32_t flags;                 // reserved
};
static_assert(sizeof(MeshPart) == 112);
```

**Rules:**

- Parts are ordered so that `parent < self` (topological order). World transforms can then be computed in a single forward loop.
- Parts with no geometry (pure hierarchy nodes) have `lodCount == 0`.

### 5.5 `LODS` — per-LOD geometry

```cpp
enum class IndexType : uint8_t { U16 = 0, U32 = 1, U8 = 2 };   // U8: core in Vulkan 1.4

struct MeshLod {                    // 48 bytes
    uint32_t layout;                // LAYT index
    uint32_t vertexCount;
    uint32_t streamOffset[kMaxStreams]; // into DECODED payload; unused streams = kInvalid
    uint32_t indexOffset;           // into DECODED payload, multiple of index size
    uint32_t indexCount;            // total for this LOD
    uint8_t  indexType;             // IndexType
    uint8_t  _pad[3];
    uint32_t submeshFirst;          // into SUBM
    uint32_t submeshCount;
    float    geometricError;        // meters; 0 for LOD0 and authored LODs. LOD select = project to screen px
};
static_assert(sizeof(MeshLod) == 48);
```

**Sizes:**

- Stream `s` occupies `vertexCount * layout.strides[s]` bytes.
- The index blob occupies `indexCount * sizeof(index)` bytes.

**Rules:**

- `geometricError` is 0 unless the cooker computed it. Authored `_lodN` LODs have no error metric, so the cooker writes 0 for them and the host chooses switch distances itself. Simplifier-generated LODs (post-v0.5) fill it in.
- kiln's cooker emits U16 or U32 indices only, and will keep doing so (`IndexType` in `mesh.h` says it too). U8 is valid in the format for other writers, and readers support it. Hosts for APIs without 8-bit indices (D3D11, Metal, WebGPU, sokol) may reject U8.

### 5.6 `SUBM` — submeshes (one draw each)

```cpp
struct Submesh {                    // 48 bytes
    uint32_t material;              // MATL index
    uint32_t indexFirst;            // relative to the LOD's index blob
    uint32_t indexCount;
    int32_t  vertexBase;            // vertexOffset for vkCmdDrawIndexed
    Bounds   bounds;                // part local space
};
static_assert(sizeof(Submesh) == 48);
```

`vertexBase` lets the cooker keep narrow indices for LODs with more vertices than the index type can address. For every index type, each submesh must reference fewer than `2^(8 * indexSize)` vertices relative to its `vertexBase` (for U16: fewer than 65 536).

### 5.7 `MATL` / `MTEX` — materials and texture mappings

The engine remaps materials **by name** (`nameHash`) through its material library. The texture bindings record what the source asset authored, so the library can use them as defaults or override them.

```cpp
enum class AlphaMode : uint8_t { Opaque = 0, Mask = 1, Blend = 2 };

struct MaterialSlot {               // 32 bytes
    uint32_t nameStr;
    uint32_t flags;                 // bit0: uses vertex color, bit1: double-sided
    uint64_t nameHash;              // key into engine material library
    uint32_t textureFirst;          // into MTEX
    uint32_t textureCount;
    uint8_t  alphaMode;             // AlphaMode (from source; library may override)
    uint8_t  _pad[3];
    float    alphaCutoff;
};
static_assert(sizeof(MaterialSlot) == 32);

enum class TextureSlot : uint8_t {
    BaseColor = 0, Normal = 1, MetalRough = 2, Occlusion = 3, Emissive = 4,
    // 5..15 reserved for engine-defined slots (detail, mask, …)
};

struct TextureBinding {             // 16 bytes
    uint64_t textureId;             // FNV-1a 64 of the texture asset name; 0 with External
    uint32_t pathStr;               // the texture asset name, or the URI with External
    uint8_t  slot;                  // TextureSlot
    uint8_t  uvSet;                 // which TexCoord semanticIndex to sample
    uint16_t flags;                 // bit0: sRGB (slot inference), bit1: External
};
static_assert(sizeof(TextureBinding) == 16);
```

Textures are **separate cooked assets** (KTX2 / BCn). They are never embedded in `.mesh`, so they stream and hot-reload independently. A binding is one of two kinds:

- **Embedded image** (bit1 clear). The image is inside the glTF source. The cooker writes it as a texture of its own named `<mesh asset name>#<image name>` — the mesh asset name includes its extension, e.g. `meshes/ship.glb#hull_albedo` (an unnamed image is `image<N>`, N its glTF index) — and `textureId` is the FNV-1a 64 of that name. Two embedded images with one name, or a combined name that is not a valid asset name, are a cook error (K1019).
- **External image** (bit1 set). The source references the image by URI. `pathStr` holds the URI, percent-decoded and relative to the source file; `textureId` is 0, and a reader rejects a non-zero one. The cooker guarantees the URI resolves inside the source's root (K1020 otherwise), but does not cook, name or track the image itself: hosts map it to a texture asset name with `resolve_asset_name(meshName, uri, ...)`, or `texture_asset_name(meshName, view, binding, ...)` for any binding (`include/kiln/assets.h`).

The sRGB bit comes from the cooker's slot inference. For an embedded image it matches how the texture was cooked. For an external image it records the authored intent only; the texture's own cook decides its color space.

### 5.8 `MNTS` — mount slots

```cpp
struct Mount {                      // 48 bytes
    uint32_t nameStr;               // "mount_engine_01"
    uint32_t parentPart;            // PART index or kInvalid (model root)
    uint64_t nameHash;
    float    translation[3];        // relative to parent part
    float    rotation[4];           // xyzw; +Z = slot forward, +Y = slot up
    uint32_t extrasStr;             // "key=value;key=value" or kInvalid
};
static_assert(sizeof(Mount) == 48);
```

Mounts are sorted by `nameHash`, so a lookup is a binary search with no hash map needed.

`extrasStr` is a flat string of `key=value` pairs separated by `;`. Keys and values must not contain `;` or `=`; the cooker drops such pairs with a warning. Only scalar glTF extras are carried. Structured extras wait for the reserved `XTRA` section (§9).

### 5.9 `BLOB` — payload blobs and encoding

Each blob maps one **encoded range** in `GPUD` to one **decoded range** in the GPU payload, and says how to get from one to the other. Draw-facing records never reference blobs; they only use decoded offsets. Compression is therefore invisible to everything except the loader's decode step.

```cpp
enum class Codec : uint8_t {
    None           = 0,   // encoded bytes == decoded bytes
    Zstd           = 1,   // general-purpose; pairs well with Filter::ByteShuffle
    MeshoptVertex  = 2,   // meshopt_decodeVertexBuffer(count = decodedSize / elementSize, size = elementSize)
    MeshoptIndex   = 3,   // meshopt_decodeIndexBuffer   (triangle lists)
    MeshoptIndexSeq= 4,   // meshopt_decodeIndexSequence (non-triangle / arbitrary sequences)
    // 5..127 reserved; 128..255 vendor/experimental
};

enum class Filter : uint8_t {
    None        = 0,
    ByteShuffle = 1,      // bytes transposed by element (column-major per byte lane)
    Delta       = 2,      // per-lane delta over elements (integer lanes); reserved: lane width unspecified (§10)
    MeshoptOct  = 3,      // meshopt_decodeFilterOct   (reserved: float normals/tangents)
    MeshoptQuat = 4,      // meshopt_decodeFilterQuat  (reserved)
    MeshoptExp  = 5,      // meshopt_decodeFilterExp   (reserved: float positions/UVs)
};

enum BlobFlags : uint16_t {
    kBlobOuterZstd = 1u << 0,   // encoded bytes are Zstd(codec output); Meshopt* codecs only
};

struct PayloadBlob {                // 32 bytes
    uint32_t encodedOffset;         // into GPUD (encoded payload), 16-B aligned; ranges never overlap
    uint32_t encodedSize;
    uint32_t decodedOffset;         // into decoded payload, 16-B aligned; ranges never overlap
    uint32_t decodedSize;
    uint16_t elementSize;           // vertex stride (vertex blobs) or index size (index blobs); never 0
    uint8_t  codec;                 // Codec
    uint8_t  filter;                // Filter (one per blob)
    uint16_t flags;                 // BlobFlags
    uint8_t  lodRank;               // lodCount(part) - 1 - lodIndex, capped at 255; 0 = coarsest LOD of each part
    uint8_t  _pad;
    uint32_t checksum;              // xxh32 of decoded bytes (0 = not stored); for tools and debug validation
    uint32_t _reserved;
};
static_assert(sizeof(PayloadBlob) == 32);
```

**Encode and decode order.** Filters apply before the codec on encode and are undone after it on decode. Encoding a blob is: filter, then codec, then outer Zstd if `kBlobOuterZstd` is set.

**Decode order** for one blob:
1. Read the encoded range.
2. If `kBlobOuterZstd`, inflate with Zstd.
3. Apply `codec`.
4. Undo `filter`.
5. The result must be exactly `decodedSize` bytes, written at `decodedOffset` in the destination memory. Output that is shorter, or would be longer, is `Corrupt`.

A blob carries at most one filter. If Delta and ByteShuffle are ever needed together, a combined `Filter` value with a fixed decode order will be added.

**Allowed codec and filter combinations:**

| Codec | Allowed filters |
|---|---|
| `None`, `Zstd` | `None`, `ByteShuffle`, `Delta` |
| `MeshoptVertex` | `None`, `MeshoptOct`, `MeshoptQuat`, `MeshoptExp` |
| `MeshoptIndex`, `MeshoptIndexSeq` | `None` only |

Any other pair is a writer error, and a loader reports `ValidationFailed`. `kBlobOuterZstd` is valid only with the `Meshopt*` codecs: with `None` it would duplicate codec `Zstd`, and with `Zstd` it would compress twice. One canonical encoding per scheme also keeps writer output deterministic.

A loader that meets a codec or filter it does not support (unknown, or not built into this runtime) fails the asset: the mesh goes to `Failed` with `Unsupported` and one diagnostic; there is no mesh placeholder.

**Rules:**

- **Table.** `BLOB` is always present and counted in `sectionCount`, with or without `kPayloadRaw`. Entries are sorted by `encodedOffset` ascending, and `lodRank` is non-decreasing along the table. Encoded ranges never overlap (no deduplication in v0.x), and neither do decoded ranges. The order of decoded ranges is free, and loaders must not assume it. Violations are `Corrupt`.
- **`lodRank`.** `lodRank = lodCount(part) - 1 - lodIndex`, capped at 255, where `lodIndex` is the LOD's position within its part (LOD0 = 0). Every part's coarsest LOD has rank 0, so a prefix read by rank gets a coarse LOD of every part.
- **`elementSize`.** Vertex blobs store the stream stride and index blobs the index size. `decodedSize` is a multiple of it.
- **Granularity.** One blob per vertex stream per LOD and one per index buffer per LOD. The v0.5 cooker writes exactly this and never splits a range. The format allows a range to be covered by several blobs over consecutive decoded sub-ranges (parallel decode, GPU-decompression-friendly chunks; indices stay absolute, so there is no rebasing), but the split rules are **deferred** to the codec work (v0.6). Any future rule must keep splits on whole vertices and whole triangles; whether every split blob must also start 16-B aligned (which would make the split step `lcm(unit, 16)`) is an open question (§10). Readers only require what §7 lists: every blob starts 16-B aligned and `decodedSize` is a multiple of `elementSize`.
- **Index blobs.** Each index blob has `elementSize` = index size and starts 16-B aligned. Its `decodedSize` is a multiple of 3 × index size for `MeshoptIndex` (triangle lists) and of the index size otherwise. meshopt's index codecs support 2- and 4-byte indices only, so U8 indices are allowed only with codec `None` or `Zstd`. Other combinations are a cook error (K3xxx) and `ValidationFailed` on read.
- **Raw fast path.** The header sets `kPayloadRaw` only if every blob has codec `None`, filter `None`, flags 0, `encodedOffset == decodedOffset` and `encodedSize == decodedSize`, and every gap in `GPUD` is zero in the file. Then `gpuDataSize == payloadDecodedSize`, encoded and decoded layouts are identical, and the loader does one direct read of `GPUD` into GPU-visible memory. The loader still validates these conditions (cheap) and fails with `Corrupt` if they do not hold. A file with all-`None` blobs but no flag is valid and goes through the decode loop.
- **Size limits.** Blob offsets and sizes are `u32`, so the encoded payload and the decoded payload are each at most 4 GiB (`encodedOffset + encodedSize ≤ 2^32` and `decodedOffset + decodedSize ≤ 2^32`). The cooker fails above that (writer check, K4xxx). Header size fields stay `u64`.
- **Checksum.** `checksum` is xxh32 of the decoded bytes. 0 means "not stored", even when the real hash happens to be 0; that only skips a debug check.
- **Destination.** Compressed blobs are read into a worker-local scratch buffer (arena) and decoded **directly into the adapter-provided destination** (staging / ReBAR). There is never a second full-size CPU copy.
- **Quantization is not compression.** Vertex formats (§6) stay as described by `LAYT`, and the decoded payload is always exactly what the GPU consumes. Codecs and filters are lossless transforms of that data.

**Candidate schemes (to be decided with measurements, not part of v0.5 cooker output):**

| Scheme | Blob settings | Expected character |
|---|---|---|
| Basic | `Zstd` + `ByteShuffle` (vertex) / `Zstd` (index) | Simple, one dependency, good ratio on quantized data, very fast decode |
| meshopt | `MeshoptVertex` / `MeshoptIndex` | Best decode speed (GB/s range), good ratio, designed for this data |
| meshopt + Zstd | above + `kBlobOuterZstd` | Best ratio; costs an extra decode pass |

A cook setting selects the scheme per asset or per target (see the hand-off doc's cook settings). The runtime needs only the decoders for the schemes a project enables: Zstd decode and the meshopt decoder are both small.

---

## 6. Vertex encoding profiles (cooker settings)

The format allows any `kiln::Format` (`VkFormat` value) with `VERTEX_BUFFER` support. The cooker ships three profiles:

| Attribute | `default` (quantized) | `precise` | `float` | Bytes (default / float) |
|---|---|---|---|---|
| Position (stream 0) | `R16G16B16A16_UNORM` + part dequant | `R32G32B32_SFLOAT` | `R32G32B32_SFLOAT` | 8 / 12 |
| Normal | `R16G16_SNORM` octahedral | `R16G16_SNORM` oct | `R32G32B32_SFLOAT` | 4 / 12 |
| Tangent | `R16G16B16A16_SNORM` (w = sign) | same | `R32G32B32A32_SFLOAT` (w = ±1) | 8 / 16 |
| UV0 / UV1 | `R16G16_SFLOAT` | `R32G32_SFLOAT` | `R32G32_SFLOAT` | 4 / 8 each |
| Color0 | `R8G8B8A8_UNORM` | same | same | 4 |

A typical `default` mesh uses stream 0 (8 B) and stream 1 with normal, tangent and UV0 (16 B), for **24 B/vertex**.
The same mesh in `float` is 12 + 36 = **48 B/vertex**. `float` needs no decoding in a shader: every
attribute is a plain float vector (color is normalized by the vertex fetch). It is meant for simple
renderers and the integration examples; `default` stays the profile for shipping content.

**Automatic fallback to `precise`:**

- Positions fall back when the part's extent / 65535 is larger than a tolerance (default 0.1 mm). Large structures sometimes need this.
- UVs fall back when they fall outside ±2048 or would lose precision (heavily tiled UVs).

Quantized positions decode as `pos = unorm.xyz * posScale + posBias`. Fold this into the model matrix on the CPU, so shaders never see it.

---

## 7. Loading

```
1. read(0, 80)                     → FileHeader; check magic, versionMajor,
                                     versionMinor (exact match while 0.x), fileSize
2. read(0, gpuDataOffset)          → CPU region (header + section table + all metadata)
                                     one allocation; validate section bounds and BLOB; build views
3. allocate payloadDecodedSize in mesh arena (device-local buffer, payloadAlignment-aligned)
4a. kPayloadRaw set (identity conditions validated in step 2):
      read(gpuDataOffset, gpuDataSize) → directly into staging / ReBAR mapping
                                         (gpuDataSize == payloadDecodedSize)
4b. otherwise, per blob (in parallel across workers, any order):
      read encoded range → worker scratch
      decode (outer Zstd → codec → unfilter) → exactly decodedSize bytes
                                               into staging/ReBAR at decodedOffset
      zero-fill decoded gaps
5. copy to device (transfer queue), signal timeline semaphore; mesh becomes drawable
```

In practice steps 1 and 2 can be merged by reading the first 64 KB speculatively.

**Progressive LOD loading** (post-v0.5): read only the blobs with `lodRank` below a threshold. `BLOB` is sorted by `encodedOffset` with non-decreasing `lodRank`, so this is a single prefix read of `GPUD`, and it gets a coarse LOD of every part. Mark only those LODs drawable, then fetch the rest later.

**Metadata access:** the metadata stays as the loaded blob, and there is no per-record deserialization. Readers expose each section through a strided accessor: element `i` is at `offset + i * stride`, and reading it copies `min(stride, sizeof(T))` bytes and zero-fills the rest. When `stride == sizeof(T)` (the common case) the accessor is a plain contiguous span over the blob.

**Validation:** do it in debug and tools builds, and optionally on load for mod or user content. The header, `BLOB` and `kPayloadRaw` checks are cheap and always run. Check:

- header: `magic`, version (§4), `gpuDataSize == GPUD size == fileSize - gpuDataOffset`, and with `kPayloadRaw` also `gpuDataSize == payloadDecodedSize`
- all indices are in range
- string offsets are inside `STRS`
- stream and index ranges are inside the decoded payload (`payloadDecodedSize`)
- `BLOB` is present; entries are sorted by `encodedOffset` with non-decreasing `lodRank`; encoded ranges lie inside `GPUD` and don't overlap; decoded ranges don't overlap and cover every range `LODS` references (`Corrupt`)
- encoded and decoded offsets are 16-B aligned; `elementSize` is non-zero and `decodedSize` is a multiple of it (of 3 × index size for `MeshoptIndex`) (`ValidationFailed`)
- codec and filter IDs are known and supported (`Unsupported` otherwise); the codec/filter pair is allowed, `kBlobOuterZstd` appears only with `Meshopt*` codecs, and U8 index blobs use only `None` or `Zstd` (`ValidationFailed`)
- with `kPayloadRaw`, every blob meets the identity conditions of §5.9 (`Corrupt`)
- decoders produce exactly `decodedSize` bytes, never more or less (`Corrupt`)
- if `checksum != 0`, the decoded bytes match it (tools and debug builds)
- index values are `< vertexCount` (this one only in tools)

---

## 8. Drawing (Vulkan 1.4)

Two supported binding models. Pick per renderer; the file supports both.

**A. Classic vertex input.**

- Derive `VkVertexInputBindingDescription` / `VkVertexInputAttributeDescription` directly from `VertexLayout`. Format values are already `VkFormat` values.
- Create one pipeline variant per distinct layout. Cooker profiles keep this to a handful.
- Bind with `vkCmdBindVertexBuffers2` and `vkCmdBindIndexBuffer2` (core in 1.4 via maintenance5, which takes an explicit size).

**B. Vertex pulling via buffer device address.**

- Pass `{payloadAddress + streamOffset[s], stride}` and a layout ID in push constants or a per-draw record. The shader decodes the attributes itself.
- One pipeline serves all layouts, which pairs well with multi-draw-indirect batching.
- Index buffers still go through `vkCmdBindIndexBuffer2`.

**Per submesh:**

```cpp
// indexSize = 1, 2 or 4 bytes, from lod.indexType
vkCmdDrawIndexed(cmd, sm.indexCount, instanceCount,
                 lod.indexOffset / indexSize + sm.indexFirst,
                 sm.vertexBase, firstInstance);
```

---

## 9. Reserved section IDs

| ID | Purpose |
|---|---|
| `SKIN` | Joints, inverse bind matrices, skin → part binding |
| `MORF` | Morph target deltas |
| `MLET` | Meshlets and cluster bounds (optional, for mesh-shader hardware) |
| `COLL` | Collision proxies (`col_*` nodes) |
| `XTRA` | Structured extras (if `key=value` strings become insufficient) |

---

## 10. Open questions

- **Part scale:** does any asset need non-uniform part scale? Currently it's baked into vertices and forbidden on parts.
- **Material library tag:** should material remap data live in the `.mesh`, or only in the library? Right now `MTEX` is "authored defaults" (proposed: library only, `docs/open-questions.md` A6).
- **Hot reload:** keyed by `assetId` plus `cookHash`. Old GPU ranges are released after frames-in-flight (see `design/handles-and-states.md`).
- **Per-LOD LOD-switch hysteresis:** stored in the file or engine-global?
- **Default compression scheme:** Basic (Zstd + ByteShuffle), meshopt, or meshopt + Zstd. Decide by measuring ratio and decode MB/s on the real asset set; the format supports all three.
- **Delta filter semantics:** per-lane delta over which lane width (element-size lanes vs 2-byte vs 4-byte)? Specify precisely before the first cooker emits it, or drop it if measurements show no gain over ByteShuffle alone. Until then writers must not emit `Delta`.
- **Split blobs (v0.6):** chunk size fixed (e.g. 64 KB decoded) or cooker-chosen per range? Must split blobs start 16-B aligned (split step `lcm(unit, 16)`, e.g. 48 B for a 24-B stride or U16 triangles), or is whole-element alignment enough? Decide with the codec work; until then the cooker writes one blob per range.
- **meshopt filters on float data:** only relevant for the `precise` profile. Possibly never needed if quantized profiles dominate.
