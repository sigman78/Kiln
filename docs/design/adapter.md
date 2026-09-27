# Adapter interface (the renderer boundary)

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** The `Format` enum and the concrete shape, calls and threading contract of the renderer adapter.

## Decision

The adapter is a **POD struct of function pointers plus `void* user`**, the same pattern as
`Allocator`, `LogSink` and `DiagSink`. No virtual interfaces. It lives in `include/kiln/adapter.h`
(M3) and `Format` lives in `include/kiln/formats.h` (M1).

### `Format`: compact enum, values equal to `VkFormat`

```cpp
enum class Format : u32 {
    Undefined           = 0,
    R8_UNORM            = 9,
    R8G8_UNORM          = 16,
    R8G8B8A8_UNORM      = 37,
    R8G8B8A8_SRGB       = 43,
    R16_UNORM           = 70,
    R16G16_UNORM        = 77,
    R16G16_SNORM        = 78,
    R16G16_SFLOAT       = 83,
    R16G16B16A16_UNORM  = 91,
    R16G16B16A16_SNORM  = 92,
    R16G16B16A16_SFLOAT = 97,
    R32G32_SFLOAT       = 103,
    R32G32B32_SFLOAT    = 106,
    R32G32B32A32_SFLOAT = 109,
    // reserved, added when encoders land (v0.6+):
    // BC1_RGB_UNORM_BLOCK = 131 ... BC7_UNORM_BLOCK = 145, BC7_SRGB_BLOCK = 146,
    // ETC2_R8G8B8_UNORM_BLOCK = 147 ..., ASTC_4x4_UNORM_BLOCK = 157 ...
};
```

- Only the formats kiln actually supports are listed. The enum is not a full `VkFormat` mirror.
- The numeric values **equal** `VkFormat`. KTX2 already stores `vkFormat` numerically, and the
  `.mesh` spec stores vertex formats as `VkFormat`. So both on-disk formats agree with the enum,
  and the Vulkan adapter is a `static_cast<VkFormat>`.
- kiln never includes `vulkan.h`. The numbers are written by hand and verified by a test in the
  viewer build (which does have Vulkan headers): `static_assert(u32(Format::X) == VK_FORMAT_X)`.
- Other backends (sokol, bgfx, D3D12) map through their own table, keyed by `Format`.

v0.5 usage:

| Format | VkFormat | Texture (KTX2) | Vertex (`.mesh`) |
|---|---|---|---|
| `R8_UNORM` | 9 | mask, height (8-bit) | |
| `R8G8_UNORM` | 16 | 2-channel normal (optional) | |
| `R8G8B8A8_UNORM` | 37 | linear color, ORM, normal | Color0 |
| `R8G8B8A8_SRGB` | 43 | sRGB color | |
| `R16_UNORM` | 70 | 16-bit height | |
| `R16G16_UNORM` | 77 | 16-bit 2-channel normal (optional) | |
| `R16G16_SNORM` | 78 | | octahedral normal |
| `R16G16_SFLOAT` | 83 | | UV (default profile) |
| `R16G16B16A16_UNORM` | 91 | 16-bit color | quantized position |
| `R16G16B16A16_SNORM` | 92 | | tangent |
| `R16G16B16A16_SFLOAT` | 97 | HDR | |
| `R32G32_SFLOAT` | 103 | | UV (precise profile) |
| `R32G32B32_SFLOAT` | 106 | | position (precise profile) |
| `R32G32B32A32_SFLOAT` | 109 | reserved (HDR full precision) | |
| `BC1_RGB_UNORM_BLOCK` .. `BC7_SRGB_BLOCK` | 131 .. 146 | reserved (v0.6) | |
| `ETC2_*`, `EAC_*` | 147 .. 156 | reserved (v0.9 mobile) | |
| `ASTC_*` | 157 .. 184 | reserved (v0.9 mobile) | |

Format properties come from a constexpr table:

```cpp
struct FormatInfo {
    u8     bytesPerBlock;   // bytes per texel for uncompressed formats
    u8     blockW, blockH;  // 1x1 for uncompressed
    u8     channelCount;
    bool   isSrgb;
    bool   isCompressed;
    bool   isDepth;
    Format srgbPair;        // UNORM <-> SRGB counterpart, or Undefined
};
constexpr FormatInfo format_info(Format f);
```

The table is checked with `static_assert` (HANDOFF §2: constexpr format tables).

### Adapter calls

```cpp
enum class FormatUsage : u8 { Sampled = 1, Vertex = 2 };      // bit flags
enum class UploadKind  : u8 { MeshPayload, TextureLevels };

struct CopyConstraints {
    u64 optimalRowPitchAlign;   // e.g. 256 on D3D12-style copies, 1 or 4 on Vulkan
    u64 optimalOffsetAlign;     // per-level offset alignment inside the upload
    u64 bufferOffsetAlign;      // alignment of the upload start (vertex/index buffers)
};

struct TextureDesc {
    Format format;
    u32    width, height, depth, layers, levels;
    u32    firstLevel;          // reserved for partial loads; 0 in v0.5
};

struct MeshPayloadDesc {
    u64 gpuDataSize;            // .mesh GPUD size
    u32 indexSize;              // largest index size used (1, 2 or 4)
};

struct UploadDesc {
    AssetId                id;
    UploadKind             kind;
    u64                    size;
    u32                    alignment;
    TextureDesc const*     texture;   // non-null for TextureLevels
    MeshPayloadDesc const* mesh;      // non-null for MeshPayload
};

struct UploadTarget {
    void* dst;             // mapped staging / ReBAR / host-image-copy scratch, >= size bytes
    u64   rowPitchAlign;   // row pitch kiln must honor for this upload
    u64   token;           // renderer's opaque ticket, never 0
};

struct GpuObject { u64 token; UploadKind kind; };

struct Adapter {
    bool   (*supports_format)(void* user, Format f, FormatUsage u);
    void   (*copy_constraints)(void* user, CopyConstraints* out);
    Status (*begin_upload)(void* user, UploadDesc const& desc, UploadTarget* out);
    void   (*commit_upload)(void* user, u64 token);
    bool   (*is_upload_complete)(void* user, u64 token);
    void   (*destroy_deferred)(void* user, GpuObject obj);
    void*  reserved[4];    // residency hooks (budget, eviction), v0.8; must be null
    void*  user;
};
```

| Call | Contract |
|---|---|
| `supports_format` | Called at context creation and when cooking for the host (cook-on-miss picks a supported format). Must be cheap and pure. |
| `copy_constraints` | Called once at context creation. Values must be powers of two. |
| `begin_upload` | Renderer returns destination memory. `Code::Busy` means "not now" (staging full): kiln keeps the job queued and retries on a later `pump()`. Any other failure moves the asset to `Failed`. |
| `commit_upload` | kiln has finished writing `dst`. The renderer records and submits the copy. |
| `is_upload_complete` | Polled in `pump()`. When true, the asset becomes `Ready` in the same pump. |
| `destroy_deferred` | kiln no longer references the object. The renderer frees it after its frames in flight. |

**Texture layout.** kiln writes mip levels (and layers) tightly packed in level order, each row
padded to `rowPitchAlign` and each level start aligned to `optimalOffsetAlign`. The resulting
per-level offsets and row pitches are handed back in `TextureView`, so the renderer builds its
copy regions from the view without recomputing anything.

**Mesh layout.** kiln writes the `.mesh` GPUD blob as-is. All stream and index offsets in the view
are relative to the upload start, so the renderer can sub-allocate from a larger buffer.

### Null adapter

Shipped in `examples/null_adapter` (and used by tests):

- `supports_format`: true for every v0.5 format.
- `begin_upload`: allocates `size` bytes from the context `Allocator` (`Tag::Payload`), returns
  an incrementing token. Can be configured to return `Busy` every Nth call for back-pressure tests.
- `commit_upload`: no-op. `is_upload_complete`: always true.
- `destroy_deferred`: frees immediately.
- Records every call (kind, id, size, token) into a log so tests can assert call order and counts.

### Threading contract (owner decision)

**Proposed default:**

- `supports_format`, `copy_constraints`, `is_upload_complete`, `destroy_deferred`: called only on
  the pumping thread.
- `begin_upload` and `commit_upload`: kiln **may call them from worker threads**, so a worker can
  decode straight into staging memory without an extra copy. The renderer must make these two
  calls thread-safe (typically a lock-free or mutex-protected staging ring).

**Alternative:** every adapter call on the pumping thread. Simpler for adapter authors, but the
decode must go to a kiln-owned CPU buffer first, and the memcpy into staging is then serialized on
the pump thread inside the upload budget.

A flag in the adapter descriptor (`bool workerUploads`) could support both. Proposed: start with
the default above; add the flag only if the external project needs it.

## Rationale

- Function pointers + `user` match the rest of kiln and need no RTTI or vtables across a library
  boundary.
- VkFormat-valued enum removes a translation table for the primary backend and keeps on-disk data
  and API values identical, while still being a kiln-owned type.
- `Busy` as a normal return gives back-pressure without a separate query call.

## Alternatives considered

- **Full numeric `VkFormat` mirror**: hundreds of values we do not support; invites misuse.
- **Dense own enum (0, 1, 2...) + mapping table**: needs a translation on load for both KTX2 and
  `.mesh`, and a table even for Vulkan.
- **Virtual interface class**: vtable ABI across DLLs, inconsistent with other kiln interfaces.
- **Callback-based completion**: violates "no callbacks from workers"; polling is simpler.

## Consequences / what this constrains later

- Adding BCn/ASTC means adding enum values with their VkFormat numbers and table rows; no layout
  change.
- `reserved[4]` must be null in v0.5 and is the only place residency hooks may go without
  breaking the struct layout.
- `TextureDesc.firstLevel` is the hook for partial mip loads (v0.8).
- If uploads come from workers, adapter implementations need thread-safe staging from day one.

## Open points for the owner

- Confirm VkFormat-valued compact `Format` enum (HANDOFF §13 Q5).
- Confirm `begin_upload` / `commit_upload` may be called from worker threads.
- Confirm `reserved[4]` tail (vs a versioned `structSize` field).
- `R16G16_UNORM` and `R32G32B32A32_SFLOAT` are in the enum but not produced by v0.5 cook defaults;
  keep or drop?
