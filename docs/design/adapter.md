# Adapter interface (the renderer boundary)

**Status:** Proposed (awaiting owner sign-off). Implemented in M3: `include/kiln/adapter.h`,
`include/kiln/formats.h`, the null adapter in `include/kiln/null_adapter.h`; M4 adds the Vulkan
example (`viewer.md`).
**Decides:** The `Format` enum and the shape, calls, binding models and threading contract of the
renderer adapter.

## Decision

The adapter is a **POD struct of function pointers plus `void* user`**, the same pattern as
`Allocator`, `LogSink` and `DiagSink`. No virtual interfaces.

### `Format`: compact enum, values equal to `VkFormat`

- `enum class Format : u32` in `formats.h`. It is not a full `VkFormat` mirror. It lists the
  uncompressed formats kiln cooks or reads, plus the BC, ETC2/EAC and ASTC LDR block formats, so
  the KTX2 reader can describe them. No encoder writes block formats yet (v0.6+).
- The numeric values **equal** `VkFormat`. KTX2 stores `vkFormat` numerically and the `.mesh` spec
  stores vertex formats as `VkFormat`, so both on-disk formats agree with the enum, and the Vulkan
  adapter is a `static_cast<VkFormat>`.
- kiln never includes `vulkan.h`. The numbers are written by hand and checked by `static_assert`
  against `VK_FORMAT_*` in `examples/viewer/vk_adapter.cpp`.
- Other backends (sokol, bgfx, D3D12) map through their own table, keyed by `Format`.
- Properties come from a constexpr table (`FormatInfo`, `format_info()`, which returns nullptr for
  an unknown format), checked by `static_assert(format_table_ok())`.

Formats the v0.5 cooker writes:

| Format | VkFormat | Texture (KTX2) | Vertex (`.mesh`) |
|---|---|---|---|
| `R8_UNORM` | 9 | 1-channel mask, height, LUT (8-bit) | |
| `R8G8_UNORM` | 16 | 2-channel mask, height, LUT | |
| `R8G8B8A8_UNORM` | 37 | linear color, ORM, normal, LUT | Color0 |
| `R8G8B8A8_SRGB` | 43 | sRGB color (HDR is cooked as color in v0.5) | |
| `R16_UNORM` | 70 | 1-channel mask, height (16-bit) | |
| `R16G16_SNORM` | 78 | | octahedral normal |
| `R16G16_SFLOAT` | 83 | | UV (default profile) |
| `R16G16B16A16_UNORM` | 91 | | quantized position |
| `R16G16B16A16_SNORM` | 92 | | tangent |
| `R32G32_SFLOAT` | 103 | | UV (precise and float profiles) |
| `R32G32B32_SFLOAT` | 106 | | position (precise and float profiles), normal (float profile) |
| `R32G32B32A32_SFLOAT` | 109 | | tangent (float profile) |

### `GpuObject`

`{ u64 native; u32 slot = kInvalid; u32 kind; }`, all adapter-defined.

- All three values are **opaque to kiln**. kiln stores, copies and returns them.
- `native == 0` and `slot == kInvalid` is **null** (`is_null()`); `kind` does not matter.
- A slot-only object (`native == 0`, valid `slot`) is legal: a bindless adapter returns it from
  `acquire()` before the real image exists.
- The object may name something that does not exist yet. `begin_upload` runs on a worker thread,
  where a GL or sokol adapter cannot create its texture; it returns an index into its own table in
  `native` and creates the real object in `flush` (`kiln-gl` does this).

### Adapter calls

The structs (`CopyConstraints`, `TextureDesc`, `MeshPayloadDesc`, `UploadDesc`, `UploadTarget`,
`Adapter`) are in `adapter.h`. `adapter_is_valid()` requires every entry point except `acquire`
and `publish`, a null `reserved[4]` and no unknown `caps` bits; `create()` rejects anything else.

| Call | Contract |
|---|---|
| `supports_format` | Called on the pump thread when an asset reaches metadata: the texture format with `SampledImage`, every vertex attribute format with `VertexBuffer`. False fails the asset (K5004). Must be cheap and pure. |
| `copy_constraints` | Called once in `create()`. kiln rounds each value up to a power of two. |
| `acquire` | Called once per asset, on the first `request()` of its id, on the requesting thread. A bindless adapter allocates a descriptor slot of the type of `shape`, writes the placeholder for `texKind` and `shape` into it, and returns a slot-bound object. A per-frame-lookup adapter returns a null object or leaves `acquire` null. Non-Ok moves the asset to `Failed` on the next `pump()`. `Busy` is not allowed. `texKind` is ignored for meshes. |
| `begin_upload` | Returns destination memory and the `GpuObject` it will hold. `Code::Busy` means "not now" (staging full): kiln retries on a later `pump()`. Any other failure moves the asset to `Failed` (K5004). |
| `commit_upload` | kiln has finished writing `dst`. Always called after a successful `begin_upload`, even when the load then fails. The renderer records and submits the copy (itself if `kSelfSubmitting`, else with its next frame). |
| `is_upload_complete` | Polled in `pump()` (and in `create()`'s placeholder spin). When true, the asset becomes `Ready` in the same pump. |
| `publish` | Called during `pump()` when an asset becomes `Ready` (or, from M5, a hot reload swaps its payload). `version` is the **content version** (`handles-and-states.md`), not the handle generation. A bindless adapter overwrites the slot it acquired for `id`. At unload and at `destroy()` kiln calls `publish(id, null, version)`, and a bindless adapter frees the slot after its frames in flight. |
| `destroy_deferred` | kiln no longer references the object. The renderer frees it after its frames in flight; an API that keeps objects alive for issued commands (GL, sokol) may free it at once, but a bindless GL handle must stay resident until the frames that may sample it have finished (`kiln-gl-bindless` fences it). The object's upload may still be in flight when an asset is unloaded: kiln then never polls that token again, and the adapter retires the upload itself (frees its staging space when the GPU is done). |
| `flush` | Optional. Called at the start of every `pump()` (so in every `wait()` loop) and in `create()`'s placeholder spin, on that thread. An adapter whose API must be called on the graphics context's thread (GL, sokol) does its GPU work here: create the objects, record the copies, insert fences. |
| `caps` | Constant for the life of the context. Unknown bits must be 0. |

HANDOFF names the last `publish` parameter `generation`. kiln names it `version` to make clear it
is the content version.

### `kSelfSubmitting`

An adapter sets `kSelfSubmitting` only if both hold:

1. `commit_upload` **submits the copy by itself** (for example on a dedicated transfer queue).
2. `is_upload_complete` **makes progress without the host recording a frame**.

`wait()` panics without it or `flush` (K5007), and `create()` spins on the placeholder uploads only
when one of them is set. An adapter that submits uploads inside the frame's command buffers must
not set it. Its hosts call `pump()` every frame and show `progress()`.

### `flush`

For APIs whose calls must run on one thread. `begin_upload` and `commit_upload` run on kiln workers,
so such an adapter only writes memory and queues there; `flush` does the rest on the pump thread,
and `is_upload_complete` polls the result (a GL fence, for example). The host calls `create()` and
`pump()` on the thread that owns the graphics context, which is where hosts call them anyway.
With `flush` set, `wait()` works and `create()` waits for the placeholders, as with
`kSelfSubmitting`. An upload committed during one `pump()` is flushed at the start of the next.

### `kCubeTextures`, `kArrayTextures`

The adapter accepts textures of `TextureShape::Cube` or `Array` (`texture-shapes.md`). kiln then
uploads the placeholders of that shape at `create()`, and `TextureDesc::shape` tells the adapter
which image view to create. Without the bit, kiln sends no texture of that shape: a request for it
fails with K5004. The example Vulkan adapter sets both: one bindless binding per shape.

### `kMeshes`

The adapter accepts `UploadKind::MeshPayload`. Without the bit, kiln never calls `acquire` or
`begin_upload` for a mesh, and `request_mesh` / `register_mesh` give a handle that fails with
K5004. A texture-only adapter leaves it clear and can ignore `bufferOffsetAlign`
(`texture-only.md`). The null adapter and the example Vulkan adapter set it.

### Two binding models

The renderer picks one; `gpu(ctx, handle)` (an allocation-free table lookup) serves both.

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
| `Ready` | the published (real) object | the payload object |
| `Failed` | the acquired object if non-null (its slot shows the checker when `devPlaceholders` is on), else the Failed placeholder (`devPlaceholders`) or the kind placeholder | null |
| stale or null handle | the Failed placeholder (`devPlaceholders`), else the `BaseColor` placeholder | null |

### Placeholders through the adapter

- At `create()`, kiln uploads each texture placeholder (built-in or host-supplied RGBA8, see
  `handles-and-states.md`) through `begin_upload` / `commit_upload`. Kind placeholder ids are
  `1 + TextureKind` (1..4). The Failed placeholder is id 15 (`kFailedPlaceholderId`,
  `placeholders.h`) and is uploaded only when `devPlaceholders` is on. `Busy` is retried for up to
  10 s (R5j).
- When a placeholder upload completes, kiln calls `publish(placeholderId, obj, 1)`. A bindless
  adapter records the object per id, so `acquire()` can write it into new slots.
- When a texture with an acquired slot fails and `devPlaceholders` is on, kiln calls
  `publish(id, failedPlaceholder, version)`, so the slot shows the magenta checker.
- kiln passes placeholder objects to `destroy_deferred` only at `destroy()`.
- Readiness: with `kSelfSubmitting`, `create()` spins until all placeholders are complete (panics
  after 10 s). Without it, `gpu()` returns null for textures until the first `pump()` that sees
  them complete.

### Upload layouts

**Texture layout.** kiln writes levels (and layers; a cube's 6 faces count as layers, in the order
+X, −X, +Y, −Y, +Z, −Z, and `TextureDesc::shape` is `Cube`) in ascending level
order, each level starting at `optimalOffsetAlign`, rows padded to `optimalRowPitchAlign`.
`texture_level_layout(TextureDesc, CopyConstraints, offsets, pitches)` computes the same offsets
and row pitches from what `begin_upload` receives, so an adapter builds its copy regions without
copying kiln's rule; `texture_info()` returns them too, once the metadata is loaded. If `UploadTarget::rowPitchAlign` does not divide the planned
pitches, the texture fails with K5004 (R5e).

**Mesh layout.** kiln writes the **decoded** payload: `payloadDecodedSize` bytes, base aligned to
`max(payloadAlignment, bufferOffsetAlign)`. The adapter never sees `gpuDataSize`. With
`kPayloadRaw` this is one read of `GPUD` straight into `dst`. Otherwise kiln decodes each `BLOB`
entry into `dst` at its `decodedOffset` and zero-fills the gaps (mesh-format-spec §5.9, §7); v0.5
decodes codec `None` only. All stream and index offsets are relative to the upload start, so the
renderer can sub-allocate from a larger buffer.

### Null adapter

Part of `kiln_runtime` (`null_adapter.h`, `src/runtime/null_adapter.cpp`), used by tests, tools and
`kiln-headless`:

- `caps = kSelfSubmitting`; uploads complete immediately.
- `supports_format`: true for every known format, except block formats as vertex formats.
- `NullAdapterDesc::bindless` (default true): `acquire` hands out slot numbers and `publish` binds
  them. Off, `acquire` returns null objects, so tests cover both binding models.
- `begin_upload` allocates `size` bytes (`Tag::Payload`) and returns a 1-based object index as
  token and `native`. `busyEveryN` / `failEveryN` inject `Busy` / `Unsupported`.
- `destroy_deferred` keeps objects until `null_adapter_flush_deferred()` (simulated frames in
  flight).
- `null_adapter_stats()` counts every call; `null_adapter_payload()` and `null_adapter_slot()`
  expose what kiln wrote and bound.

### Threading contract

| Call | Thread |
|---|---|
| `copy_constraints` | the thread calling `create()` |
| `supports_format` | the pump thread |
| `acquire` | the thread calling `request()` |
| `begin_upload`, `commit_upload` | **kiln worker threads** (and the thread calling `create()` for placeholders), so a worker decodes straight into staging memory; must be thread-safe |
| `is_upload_complete`, `flush` | the pump thread (`pump()`, `wait()`), and the thread calling `create()` |
| `publish`, `destroy_deferred` | the pump thread, and the threads calling `create()` / `destroy()` |

Alternative, not taken: every call on the pump thread. The decode would go to a kiln-owned buffer
first, and the copy into staging would serialize on the pump thread. A `workerUploads` flag (or an
`AdapterCaps` bit) could support both; add it only if the external project needs it.

## Rationale

- Function pointers + `user` match the rest of kiln and need no RTTI or vtables across a library
  boundary.
- A VkFormat-valued enum removes a translation table for the primary backend and keeps on-disk data
  and API values identical, while staying a kiln-owned type.
- `Busy` as a normal return gives back-pressure without a separate query.
- `acquire` at request time lets a bindless material store its slot before the asset arrives, so
  arrival and hot reload need no material rebuild.
- A capability bit is cheaper and clearer than a runtime probe for "can uploads finish without a
  frame".

## Alternatives considered

Full `VkFormat` mirror, dense own enum plus mapping table, virtual interface class,
callback-based completion (no callbacks from workers), slot allocation in `publish` only, and
per-frame lookup only: all rejected for the reasons above.

## Consequences / what this constrains later

- Encoders for BC/ASTC need no enum or layout change; their values and table rows exist.
- `reserved[4]` must be null and is the only place residency hooks may go (v0.8) without breaking
  the struct layout. `caps` bits 1..31 are for new capability flags.
- `TextureDesc.firstLevel` is the hook for partial mip loads (v0.8). Progressive mips then arrive
  through further `publish` calls on the same slot.
- Adapter implementations need thread-safe staging, because uploads come from workers.
- A `.mesh` codec changes nothing at the adapter: it always receives decoded bytes.

## Open points for the owner

- Confirm the VkFormat-valued compact `Format` enum (open-questions A5), including block formats
  the v0.5 cooker never writes.
- Confirm `begin_upload` / `commit_upload` on worker threads.
- Confirm `acquire` on the requesting thread (the hook itself is decided, R3).
- Confirm the `reserved[4]` tail plus `caps` (vs a versioned `structSize` field).
- Confirm `UploadTarget.object` as the way kiln learns the `GpuObject` of an upload (HANDOFF does
  not say).
- Confirm slot release at unload through `publish(id, null, version)`.
