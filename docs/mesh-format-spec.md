# Orbital `.mesh` — Cooked Runtime Mesh Format

**Status:** draft v0.1 · **Target:** Vulkan 1.4, C++20, little-endian only
**Produced by:** `orb-cook` from canonical `.glb` sources. **Never** hand-authored, never edited.

## 1. Goals and non-goals

**Goals**

- **Load = 2 reads.** One read gets the header plus all metadata. A second read streams the GPU payload straight into a staging or ReBAR buffer, with no per-vertex CPU work.
- **Flat.** Fixed-size POD records, index-based references, offsets instead of pointers, one string table.
- **Flexible where it matters:**
  - multiple vertex layouts (quantized or float, optional streams and UV sets)
  - multiple UV mappings per material
  - named parts and mounts
  - LOD chains
- **Forward-compatible.** A section directory with per-section element stride lets new sections and appended fields be added without breaking old loaders.
- **Works on pre-mesh-shader hardware.** Plain indexed draws. Meshlets are a reserved optional section.

**Non-goals (for now)**

- skinning and animation
- morph targets
- collision
- meshlets
- compression beyond quantization

Section IDs for these are reserved (§9).

---

## 2. File layout

```
+--------------------------+  0
| FileHeader (64 B)        |
+--------------------------+
| SectionEntry[N]          |  sectionTableOffset
+--------------------------+
| metadata sections        |  MODL, STRS, LAYT, PART, LODS, SUBM, MATL, MTEX, MNTS …
| (each 16-B aligned)      |  contiguous: [0, gpuDataOffset) is "CPU region"
+--------------------------+
| GPUD payload             |  gpuDataOffset (256-B aligned), gpuDataSize
|  vertex streams, indices |  every blob 16-B aligned inside
+--------------------------+  fileSize
```

**Invariants the cooker guarantees:**

1. All metadata sections lie in `[0, gpuDataOffset)`. `GPUD` is the last section.
2. `gpuDataOffset % 256 == 0`. Every blob inside `GPUD` is 16-byte aligned relative to the GPUD start.
3. All in-payload offsets (streams, indices) are **relative to the GPUD start**, so the loader can place the payload anywhere in a larger GPU buffer and just add a base offset or address.
4. Index blob offsets are multiples of the index size (a Vulkan requirement).
5. Every cross-reference index is in range, and every string offset points at a null-terminated string inside `STRS`.

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
| Asset / texture IDs | FNV-1a 64 of the cooked asset path, e.g. `"meshes/ship_hauler_a/hull_albedo"` |
| Invalid index | `0xFFFFFFFF` (`kInvalid`) |
| Formats | Vertex formats are stored as raw `VkFormat` values. Vulkan-only engine, so there's no translation layer. |

---

## 4. Header and section directory

```cpp
namespace orb::mesh {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 |
           uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t kMagic        = fourcc('O','M','S','H');
constexpr uint16_t kVersionMajor = 0;   // bump = incompatible, loader rejects
constexpr uint16_t kVersionMinor = 1;   // bump = additive, loader tolerates
constexpr uint32_t kInvalid      = 0xFFFFFFFFu;
constexpr uint32_t kMaxStreams   = 4;
constexpr uint32_t kMaxAttribs   = 12;

struct FileHeader {                 // 64 bytes
    uint32_t magic;                 // kMagic
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t flags;                 // reserved, 0
    uint32_t sectionCount;
    uint64_t fileSize;
    uint64_t sectionTableOffset;    // → SectionEntry[sectionCount]
    uint64_t sourceHash;            // xxh64 of source .glb (rebuild detection)
    uint64_t cookHash;              // xxh64 of cooker version + cook settings
    uint64_t gpuDataOffset;         // == GPUD section offset (fast path)
    uint64_t gpuDataSize;
};
static_assert(sizeof(FileHeader) == 64);

struct SectionEntry {               // 32 bytes
    uint32_t id;                    // fourcc
    uint32_t flags;                 // reserved, 0
    uint64_t offset;                // from file start
    uint64_t size;                  // bytes
    uint32_t count;                 // element count (0 for blobs: STRS, GPUD)
    uint32_t stride;                // element size AS WRITTEN (≥ what old loaders know)
};
static_assert(sizeof(SectionEntry) == 32);

} // namespace orb::mesh
```

**Forward compatibility rules:**

- A loader skips unknown section IDs.
- For known sections, the loader reads element `i` at `offset + i * stride` and uses `min(stride, sizeof(T))` bytes, zero-filling the rest. This means new fields may only be **appended** to records, and only with a minor version bump.
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
| `GPUD` | — | blob | ✔ | Vertex streams and index buffers |

### 5.1 Common types

```cpp
struct Bounds {                     // 32 bytes
    float center[3];
    float radius;                   // bounding sphere around center
    float halfExtents[3];           // AABB around center
    float _pad;
};
```

### 5.2 `MODL` — model info

```cpp
struct ModelInfo {                  // 48 bytes
    Bounds   bounds;                // model space, all parts at rest pose, LOD0
    uint32_t nameStr;               // asset name, e.g. "ship_hauler_a"
    uint32_t flags;                 // reserved
    uint64_t assetId;               // FNV-1a 64 of cooked asset path
};
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
    uint32_t format;                // VkFormat
    uint16_t offset;                // byte offset within stream element
    uint16_t _pad2;
};

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
    uint32_t streamOffset[kMaxStreams]; // into GPUD; unused streams = kInvalid
    uint32_t indexOffset;           // into GPUD, multiple of index size
    uint32_t indexCount;            // total for this LOD
    uint8_t  indexType;             // IndexType
    uint8_t  _pad[3];
    uint32_t submeshFirst;          // into SUBM
    uint32_t submeshCount;
    float    geometricError;        // meters; 0 for LOD0. LOD select = project to screen px
};
static_assert(sizeof(MeshLod) == 48);
```

**Sizes:**

- Stream `s` occupies `vertexCount * layout.strides[s]` bytes.
- The index blob occupies `indexCount * sizeof(index)` bytes.

### 5.6 `SUBM` — submeshes (one draw each)

```cpp
struct Submesh {                    // 48 bytes
    uint32_t material;              // MATL index
    uint32_t indexFirst;            // relative to the LOD's index blob
    uint32_t indexCount;
    int32_t  vertexBase;            // vertexOffset for vkCmdDrawIndexed
    Bounds   bounds;                // part local space
};
```

`vertexBase` lets the cooker keep U16 indices for LODs with more than 65 535 vertices, as long as each submesh stays under the limit.

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

enum class TextureSlot : uint8_t {
    BaseColor = 0, Normal = 1, MetalRough = 2, Occlusion = 3, Emissive = 4,
    // 5..15 reserved for engine-defined slots (detail, mask, …)
};

struct TextureBinding {             // 16 bytes
    uint64_t textureId;             // FNV-1a 64 of cooked texture asset path
    uint32_t pathStr;               // same path, for tools/debug
    uint8_t  slot;                  // TextureSlot
    uint8_t  uvSet;                 // which TexCoord semanticIndex to sample
    uint16_t flags;                 // bit0: sRGB
};
```

Textures are **separate cooked assets** (KTX2 / BCn), referenced by ID. They are never embedded in `.mesh`, so they stream, dedupe and hot-reload independently.

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
```

Mounts are sorted by `nameHash`, so a lookup is a binary search with no hash map needed.

---

## 6. Vertex encoding profiles (cooker settings)

The format allows any `VkFormat` with `VERTEX_BUFFER` support. The cooker ships two profiles:

| Attribute | `default` (quantized) | `precise` | Bytes (default) |
|---|---|---|---|
| Position (stream 0) | `R16G16B16A16_UNORM` + part dequant | `R32G32B32_SFLOAT` | 8 |
| Normal | `R16G16_SNORM` octahedral | `R16G16_SNORM` oct | 4 |
| Tangent | `R16G16B16A16_SNORM` (w = sign) | same | 8 |
| UV0 / UV1 | `R16G16_SFLOAT` | `R32G32_SFLOAT` | 4 each |
| Color0 | `R8G8B8A8_UNORM` | same | 4 |

A typical `default` mesh uses stream 0 (8 B) and stream 1 with normal, tangent and UV0 (16 B), for **24 B/vertex**.

**Automatic fallback to `precise`:**

- Positions fall back when the part's extent / 65535 is larger than a tolerance (default 0.1 mm). Large structures sometimes need this.
- UVs fall back when they fall outside ±2048 or would lose precision (heavily tiled UVs).

Quantized positions decode as `pos = unorm.xyz * posScale + posBias`. Fold this into the model matrix on the CPU, so shaders never see it.

---

## 7. Loading

```
1. read(0, 64)                     → FileHeader; check magic, versionMajor, fileSize
2. read(0, gpuDataOffset)          → CPU region (header + section table + all metadata)
                                     one allocation; validate section bounds; build views
3. allocate gpuDataSize in mesh arena (device-local buffer, 256-B aligned sub-allocation)
4. read(gpuDataOffset, gpuDataSize) → directly into staging / ReBAR mapping
5. copy to device (transfer queue), signal timeline semaphore; mesh becomes drawable
```

In practice steps 1 and 2 can be merged by reading the first 64 KB speculatively.

**Metadata access:** the metadata stays as the loaded blob. Views are `std::span<const T>` over it, and there is no per-record deserialization. Wherever the stride is larger than `sizeof(T)` (newer file, older loader), use a strided accessor instead.

**Validation:** do it in debug and tools builds, and optionally on load for mod or user content. Check:

- all indices are in range
- string offsets are inside `STRS`
- stream and index ranges are inside `GPUD`
- index values are `< vertexCount` (this one only in tools)

---

## 8. Drawing (Vulkan 1.4)

Two supported binding models. Pick per renderer; the file supports both.

**A. Classic vertex input.**

- Derive `VkVertexInputBindingDescription` / `VkVertexInputAttributeDescription` directly from `VertexLayout`. Formats are already `VkFormat`.
- Create one pipeline variant per distinct layout. Cooker profiles keep this to a handful.
- Bind with `vkCmdBindVertexBuffers2` and `vkCmdBindIndexBuffer2` (core in 1.4 via maintenance5, which takes an explicit size).

**B. Vertex pulling via buffer device address.**

- Pass `{payloadAddress + streamOffset[s], stride}` and a layout ID in push constants or a per-draw record. The shader decodes the attributes itself.
- One pipeline serves all layouts, which pairs well with multi-draw-indirect batching.
- Index buffers still go through `vkCmdBindIndexBuffer2`.

**Per submesh:**

```cpp
vkCmdDrawIndexed(cmd, sm.indexCount, instanceCount,
                 lod.indexFirstElement + sm.indexFirst,   // indexOffset / indexSize
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
- **Material library tag:** should material remap data live in the `.mesh`, or only in the library? Right now `MTEX` is "authored defaults".
- **Hot reload:** keyed by `assetId` plus `cookHash`. Old GPU ranges are released after frames-in-flight.
- **Per-LOD LOD-switch hysteresis:** stored in the file or engine-global?