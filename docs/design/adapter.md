# Adapter interface (the renderer boundary)

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** The `Format` enum and the concrete shape, calls, binding models and threading contract of the renderer adapter.

Aligned to HANDOFF v2 (2026-09-26): `acquire`, `publish`, `caps` / `kSelfSubmitting`,
`GpuObject`, `kiln::gpu()`, two binding models, placeholders created through the adapter.

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

### `GpuObject`

```cpp
struct GpuObject {
    u64 native = 0;          // adapter-defined: image/buffer pointer, handle or index
    u32 slot   = kInvalid;   // adapter-defined: bindless descriptor slot, if any
    u32 kind   = 0;          // adapter-defined tag; kiln never interprets it
};
```

- All three values are **opaque to kiln**. kiln stores, copies and returns them; it never reads
  their meaning.
- A `GpuObject` with `native == 0` and `slot == kInvalid` is **null**. `kind` does not matter for
  nullness.
- A slot-only object (`native == 0`, valid `slot`) is legal: it is what a bindless adapter returns
  from `acquire()` before the real image exists.

### Adapter calls

```cpp
enum class FormatUsage : u8 { Sampled = 1, Vertex = 2 };      // bit flags
enum class UploadKind  : u8 { MeshPayload, TextureLevels };

enum AdapterCaps : u32 {
    kSelfSubmitting = 1u << 0,   // see below
    // bits 1..31 reserved, must be 0
};

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
    u64 payloadDecodedSize;     // .mesh header: bytes of the decoded GPU payload
    u32 payloadAlignment;       // .mesh header: required base alignment (>= 256)
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
    void*     dst;             // mapped staging / ReBAR / host-image-copy scratch, >= size bytes
    u64       rowPitchAlign;   // row pitch kiln must honor for this upload
    u64       token;           // renderer's opaque ticket, never 0
    GpuObject object;          // the object that holds the data once the upload completes
};

struct Adapter {
    // capability negotiation
    bool   (*supports_format)(void* user, Format f, FormatUsage u);
    void   (*copy_constraints)(void* user, CopyConstraints* out);
    // binding: request time (may be null)
    Status (*acquire)(void* user, AssetId id, UploadKind kind, TextureKind texKind, GpuObject* out);
    // placement (may return Busy, i.e. back-pressure)
    Status (*begin_upload)(void* user, UploadDesc const& desc, UploadTarget* out);
    void   (*commit_upload)(void* user, u64 token);
    bool   (*is_upload_complete)(void* user, u64 token);
    // visibility: during pump(), when an asset becomes Ready or is hot-reloaded (may be null)
    void   (*publish)(void* user, AssetId id, GpuObject obj, u32 version);
    // lifetime
    void   (*destroy_deferred)(void* user, GpuObject obj);
    u32    caps;           // AdapterCaps bits
    void*  reserved[4];    // residency hooks (budget, eviction), v0.8; must be null
    void*  user;
};
```

| Call | Contract |
|---|---|
| `supports_format` | Called at context creation and when cooking for the host (cook-on-miss picks a supported format). Must be cheap and pure. |
| `copy_constraints` | Called once at context creation. Values must be powers of two. |
| `acquire` | Called once per asset, on the first `request()` of its id, on the requesting thread. A bindless adapter allocates a descriptor slot, writes the placeholder for `texKind` into it, and returns a slot-bound object. A per-frame-lookup adapter may return a null object (or leave `acquire` null). Non-Ok moves the asset to `Failed` on the next `pump()`. `Busy` is not allowed. `texKind` is ignored for meshes. |
| `begin_upload` | Renderer returns destination memory and the `GpuObject` it will hold. `Code::Busy` means "not now" (staging full): kiln keeps the job queued and retries on a later `pump()`. Any other failure moves the asset to `Failed`. |
| `commit_upload` | kiln has finished writing `dst`. The renderer records and submits the copy (itself if `kSelfSubmitting`, else with its next frame). |
| `is_upload_complete` | Polled in `pump()` (and in the spin loops of `create()` and `wait()` for self-submitting adapters). When true, the asset becomes `Ready` in the same pump. |
| `publish` | Called during `pump()` when an asset becomes `Ready` or a hot reload swaps in a new payload. `version` is the asset's **content version** (see `handles-and-states.md`), not the handle generation. A bindless adapter overwrites the slot it acquired for `id` with `obj`. kiln passes the replaced object to `destroy_deferred`. |
| `destroy_deferred` | kiln no longer references the object. The renderer frees it after its frames in flight. |
| `caps` | Constant for the life of the context. Unknown bits must be 0. |

HANDOFF v2 §6.4 names the last `publish` parameter `generation`. kiln names it `version` to make
clear it is the content version.

### `kSelfSubmitting`

An adapter sets `kSelfSubmitting` only if both hold:

1. `commit_upload` **submits the copy by itself** (for example on a dedicated transfer queue).
   It does not wait for the host to record or submit a frame.
2. `is_upload_complete` **makes progress without the host recording a frame**: polling it in a
   loop (fence or timeline semaphore) eventually returns true, with no other host action.

What depends on it:

- `wait()` requires it and panics without it.
- `create()` spins on `is_upload_complete` for the placeholders when it is set (see below).

An adapter that submits uploads only inside the frame's command buffers must not set it. Its hosts
use the keep-rendering pattern: call `pump()` every frame and show `progress()`.

### Two binding models

The renderer picks one; the library supports both through the same query.

```cpp
GpuObject gpu(Context*, Handle<T>);   // allocation-free table lookup
```

| | Per-frame lookup | Stable bindless slot (recommended for Vulkan 1.4) |
|---|---|---|
| `acquire` | null, or returns a null object | allocates a slot, writes the kind placeholder into it, returns `{ 0, slot, kind }` |
| `publish` | null, or ignores the call | overwrites the same slot with the real image |
| Material stores | nothing; looks up `gpu(ctx, h)` each frame | the slot index, once, at request time |
| Arrival, hot reload | next `gpu()` returns the new object | nothing to do: the slot already shows it |
| Fits | sokol / bgfx-style binding, any renderer | descriptor-indexing renderers |

`gpu(ctx, h)` returns:

| Asset state | Texture | Mesh |
|---|---|---|
| `Pending`, `MetaReady` | the acquired object if non-null, else the kind placeholder | null |
| `Ready` | the published (real) object; after a reload, the new version | the payload object |
| `Failed` | Failed placeholder (dev) or kind placeholder; bindless: the acquired slot, rewritten by `publish` | null |
| stale or null handle | Failed placeholder | null |

For a bindless adapter `gpu()` keeps returning the same `slot` through every state change, so both
models share one query.

### Placeholders through the adapter

- At `create()`, kiln uploads each texture placeholder (built-in or host-supplied, see
  `handles-and-states.md`) through the normal `begin_upload` / `commit_upload` path. Placeholder
  ids are `1 + TextureKind` (1..4) and 5 for Failed; 6..15 are reserved.
- When a placeholder upload completes, kiln calls `publish(placeholderId, obj, 1)`. A bindless
  adapter records the object per kind, so `acquire()` can write it into new slots.
- When a texture fails and `devPlaceholders` is on, kiln calls `publish(id, failedPlaceholder,
  version)`, so a bindless slot shows the magenta checker.
- kiln never passes a placeholder object to `destroy_deferred`, except at `destroy()`.
- Readiness: with `kSelfSubmitting`, `create()` spins on `is_upload_complete` until all
  placeholders are complete. Without it, `gpu()` returns null for textures until the first `pump()`
  that sees them complete.

### Upload layouts

**Texture layout.** kiln writes mip levels (and layers) tightly packed in level order, each row
padded to `rowPitchAlign` and each level start aligned to `optimalOffsetAlign`. The resulting
per-level offsets and row pitches are handed back in `TextureView`, so the renderer builds its
copy regions from the view without recomputing anything.

**Mesh layout.** kiln writes the **decoded** payload: `payloadDecodedSize` bytes, base aligned to
`max(payloadAlignment, bufferOffsetAlign)`. The adapter always gets `payloadDecodedSize`, never
`gpuDataSize` (the encoded size in the file, padding included). With `kPayloadRaw` the two are equal
and this is one read of `GPUD` straight into `dst`. Otherwise kiln decodes each `BLOB` entry into
`dst` at its `decodedOffset` and zero-fills the gaps (mesh-format-spec §5.9, §7). In v0.5 only codec `None` is supported. All stream
and index offsets in the view are relative to the upload start, so the renderer can sub-allocate
from a larger buffer.

### Null adapter

Shipped in `examples/null_adapter` (and used by tests):

- `caps = kSelfSubmitting`. Uploads complete immediately.
- `supports_format`: true for every v0.5 format.
- `acquire`: returns a null object by default (per-frame lookup). A test option switches it to slot
  mode (incrementing slot numbers), so tests cover both binding models.
- `begin_upload`: allocates `size` bytes from the context `Allocator` (`Tag::Payload`), returns
  an incrementing token and `object.native` = that pointer. Can be configured to return `Busy`
  every Nth call for back-pressure tests.
- `commit_upload`: no-op. `is_upload_complete`: always true.
- `publish`: records the call; in slot mode it stores the object in its slot table.
- `destroy_deferred`: frees immediately.
- Records every call (kind, id, size, token, version) into a log so tests can assert call order
  and counts.

### Threading contract (owner decision)

**Proposed default:**

| Call | Thread |
|---|---|
| `supports_format`, `copy_constraints` | the thread calling `create()`, and the pump thread |
| `acquire` | the thread calling `request()`; must be thread-safe if the host requests from several threads |
| `begin_upload`, `commit_upload` | **may be called from worker threads**, so a worker can decode straight into staging memory without an extra copy; must be thread-safe (typically a lock-free or mutex-protected staging ring) |
| `is_upload_complete` | the pump thread (inside `pump()` and `wait()`), and the thread calling `create()` during its placeholder spin |
| `publish` | **only the pump thread**, inside `pump()` or `wait()` (placeholders: also inside `create()` when self-submitting) |
| `destroy_deferred` | only the pump thread, and `destroy()` |

**Alternative:** every adapter call on the pumping thread. Simpler for adapter authors, but the
decode must go to a kiln-owned CPU buffer first, and the memcpy into staging is then serialized on
the pump thread inside the upload budget. `acquire` would then be deferred to the next `pump()`,
and a bindless slot would not be available right after `request()`.

A flag in the adapter descriptor (`bool workerUploads`, or an `AdapterCaps` bit) could support
both. Proposed: start with the default above; add the flag only if the external project needs it.

## Rationale

- Function pointers + `user` match the rest of kiln and need no RTTI or vtables across a library
  boundary.
- VkFormat-valued enum removes a translation table for the primary backend and keeps on-disk data
  and API values identical, while still being a kiln-owned type.
- `Busy` as a normal return gives back-pressure without a separate query call.
- `acquire` at request time lets a bindless material store its slot before the asset arrives, so
  arrival and hot reload need no material rebuild.
- `publish` gives bindless adapters one place to update slots; per-frame-lookup adapters ignore it
  and use `gpu()`.
- A capability bit is cheaper and clearer than a runtime probe for "can uploads finish without a
  frame".

## Alternatives considered

- **Full numeric `VkFormat` mirror**: hundreds of values we do not support; invites misuse.
- **Dense own enum (0, 1, 2...) + mapping table**: needs a translation on load for both KTX2 and
  `.mesh`, and a table even for Vulkan.
- **Virtual interface class**: vtable ABI across DLLs, inconsistent with other kiln interfaces.
- **Callback-based completion**: violates "no callbacks from workers"; polling is simpler.
- **Slot allocation inside `publish` only (no `acquire`)**: the slot would not exist until `Ready`,
  so materials would need rebuilding on arrival, which the bindless model is meant to avoid.
- **Only per-frame lookup**: works everywhere, but costs a lookup per binding per frame and gives
  no path to bindless without churn.

## Consequences / what this constrains later

- Adding BCn/ASTC means adding enum values with their VkFormat numbers and table rows; no layout
  change.
- `reserved[4]` must be null in v0.5 and is the only place residency hooks may go without
  breaking the struct layout. `caps` bits 1..31 are the place for new capability flags.
- `TextureDesc.firstLevel` is the hook for partial mip loads (v0.8). Progressive mips then arrive
  through further `publish` calls on the same slot.
- If uploads come from workers, adapter implementations need thread-safe staging from day one.
- Adding a codec to `.mesh` changes nothing at the adapter boundary: the adapter always receives
  decoded bytes.

## Open points for the owner

- Confirm VkFormat-valued compact `Format` enum (HANDOFF §13 Q5).
- Confirm `begin_upload` / `commit_upload` may be called from worker threads.
- Confirm `acquire` on the requesting thread (owner agreed to add the hook; thread still to
  confirm).
- Confirm `reserved[4]` tail plus `caps` (vs a versioned `structSize` field).
- `UploadTarget.object`: proposed as the way kiln learns the `GpuObject` of a finished upload
  (HANDOFF v2 does not say). Confirm.
- Slot release at unload: `destroy_deferred` frees `native` objects, but on hot reload the old and
  new objects share a slot, so it cannot also free slots. Proposed: at unload kiln calls
  `publish(id, null GpuObject, version)`, and a bindless adapter frees the slot for `id` after its
  frames in flight.
- `R16G16_UNORM` and `R32G32B32A32_SFLOAT` are in the enum but not produced by v0.5 cook defaults;
  keep or drop?
