# Adapter interface (the renderer boundary)

**Status:** Proposed (awaiting owner sign-off). Implemented in M3: `include/kiln/adapter.h`,
`include/kiln/formats.h`, the null adapter in `include/kiln/null_adapter.h`; M4 adds the Vulkan
example (`viewer.md`). Frames, `bind` and `destroy` replace `acquire`, `publish` and
`destroy_deferred` (`adapter-frames-slots.md`, decided 2026-09-28).
**Decides:** The `Format` enum and the shape, calls, binding models and threading contract of the
renderer adapter.

## Decision

The adapter is a **POD struct of function pointers plus `void* user`**, the same pattern as
`Allocator`, `LogSink` and `DiagSink`. No virtual interfaces.

### `Format`: compact enum, values equal to `VkFormat`

- `enum class Format : u32` in `formats.h`. It is not a full `VkFormat` mirror. It lists the
  uncompressed formats kiln cooks or reads, plus the BC, ETC2/EAC and ASTC LDR block formats, so
  the KTX2 reader can describe them. The cooker writes BC1/3/4/5/6H/7 (`bcn-encoding.md`); no
  ETC2 or ASTC encoder yet.
- The numeric values **equal** `VkFormat`. KTX2 stores `vkFormat` numerically and the `.mesh` spec
  stores vertex formats as `VkFormat`, so both on-disk formats agree with the enum, and the Vulkan
  adapter is a `static_cast<VkFormat>`.
- kiln never includes `vulkan.h`. The numbers are written by hand and checked by `static_assert`
  against `VK_FORMAT_*` in `examples/viewer/vk_adapter.cpp`.
- Other backends (sokol, bgfx, D3D12) map through their own table, keyed by `Format`.
- Properties come from a constexpr table (`FormatInfo`, `format_info()`, which returns nullptr for
  an unknown format), checked by `static_assert(format_table_ok())`.

Formats the v0.5 cooker writes (textures on a target with `blockFamily = BC`, the default, are
block-compressed; the uncompressed rows apply to `blockFamily = None` and to usages BC does not
cover):

| Format | VkFormat | Texture (KTX2) | Vertex (`.mesh`) |
|---|---|---|---|
| `R8_UNORM` | 9 | 1-channel mask, height, LUT (8-bit) | |
| `R8G8_UNORM` | 16 | 2-channel mask, height, LUT | |
| `R8G8B8A8_UNORM` | 37 | linear color, ORM, normal, LUT | Color0 |
| `R8G8B8A8_SRGB` | 43 | sRGB color | |
| `R16_UNORM` | 70 | 1-channel mask, height (16-bit) | |
| `R16G16B16A16_SFLOAT` | 97 | HDR | |
| `BC1_RGB_UNORM` / `_SRGB` | 131 / 132 | color, by explicit `encoding = bc1` | |
| `BC3_UNORM` / `_SRGB` | 137 / 138 | color with alpha, by explicit `encoding = bc3` | |
| `BC4_UNORM` | 139 | 1-channel mask | |
| `BC5_UNORM` | 141 | normal (X, Y; Z rebuilt in the shader), 2-channel mask | |
| `BC6H_UFLOAT` | 143 | HDR | |
| `BC7_UNORM` / `_SRGB` | 145 / 146 | ORM, linear color / color, UI | |
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
- kiln sets `slot` itself for a texture of a bindless adapter (`gpu_object()`); the adapter's own `slot`
  in `UploadTarget::object` is ignored there.
- The object may name something that does not exist yet. `begin_upload` runs on a worker thread,
  where a GL or sokol adapter cannot create its texture; it returns an index into its own table in
  `native` and creates the real object in `flush` (`kiln-gl` does this).

### Adapter calls

The structs (`CopyConstraints`, `TextureDesc`, `MeshPayloadDesc`, `UploadDesc`, `UploadTarget`,
`Adapter`) are in `adapter.h`. `adapter_is_valid()` requires every entry point except `discard_upload`, `bind` and
`flush`, `bind` set exactly when `bindlessSlots` is non-zero, a null `reserved[4]` and no unknown
`caps` bits; `create()` rejects anything else.

| Call | Contract |
|---|---|
| `supports_format` | Called on the pump thread when an asset reaches metadata: the texture format with `SampledImage`, every vertex attribute format with `VertexBuffer`. False fails the asset (K5004). Must be cheap and pure. The adapter answers for its whole host, not only for the API: see "Vertex formats" below. |
| `copy_constraints` | Called once in `create()`. kiln rounds each value up to a power of two. |
| `begin_upload` | Returns destination memory and the `GpuObject` it will hold. The memory may be plain CPU memory that the adapter hands to its API later (sokol copies it at image creation). `Code::Busy` means "not now, a retry can succeed": staging space or upload records are held by uploads in flight. kiln retries on a later `pump()`, without limit, so an adapter must not return `Busy` for what can never succeed. An upload larger than the whole staging ring or heap returns `Unsupported`, a full object table (objects held by live assets) `OutOfMemory`; any failure other than `Busy` moves the asset to `Failed` (K5004). |
| `commit_upload` | kiln has finished writing `dst`. After a successful `begin_upload` kiln calls exactly one of `commit_upload` and `discard_upload`. The renderer records and submits the copy (itself if `kSelfSubmitting`, else with its next frame). |
| `discard_upload` | Optional. kiln could not write the bytes (a read or decode failed after `begin_upload`): the adapter frees the reservation and the object at once, with no GPU work. kiln never polls the token nor passes the object to `destroy`. Without it, kiln commits the upload anyway and destroys the result once it is done. May run on a worker, like `commit_upload`. |
| `upload_status` | Polled in `pump()` (and in `create()`'s placeholder spin) until not `Pending`, then never again for that token: the adapter may recycle the token with its terminal answer. `Complete`: the asset becomes `Ready` in the same pump. `Failed` (the adapter could not make the object after `commit_upload`: a full pool, out of GPU memory, a texture larger than its heap): the adapter writes the reason to `*failure` (kiln passes `Code::Unknown` in), which becomes the diagnostic's and the Failed event's `Status`; a first load fails with K5004 and shows its placeholder, a reload keeps the current version (K5010), a placeholder fails `create()` (K5009); `Failed` also promises that no GPU work on the upload remains. kiln destroys the object at once, since no frame has used it. kiln also polls the uploads it abandoned (an unload or a failure while in flight) until they are done, then destroys their objects. |
| `bind` | Bindless adapters only (`bindlessSlots > 0`). Slot `slot` shows `obj` in every frame the host records after this pump; frames already in flight may keep the previous object, which kiln destroys only after they complete (Frames, below). An API whose descriptors in-flight frames still read keeps a copy per frame (`kiln-gl-bindless`) or resolves slots when it records a frame (`kiln-nga`). kiln numbers the slots, `[0, bindlessSlots)`, one per texture asset, and calls `bind` at the request (the placeholder of the texture's kind and shape, or as soon as that placeholder's upload completes), when the texture is `Ready`, after each reload, and with the Failed checker (`devPlaceholders`). Pump thread. |
| `destroy` | No frame the host reported can use the object any more (see Frames below): free it now. Its upload reported `Complete` or `Failed`; the object of a failed upload may be partly made, and `destroy` frees whatever exists. |
| `flush` | Optional. Called at the start of every `pump()` (so in every `wait()` loop) and in `create()`'s placeholder spin, on that thread. An adapter whose API must be called on the graphics context's thread (GL, sokol) does its GPU work here: create the objects, record the copies, insert fences. |
| `caps` | Constant for the life of the context. Unknown bits must be 0. |

### Frames

The host reports its frames in `PumpOptions` (`adapter-frames-slots.md`): `frame` is the frame it
records after this pump, `completedFrame` the last one the GPU finished; 0 keeps the last value. An
object or slot number kiln drops during a pump with `frame` F is released once `completedFrame >= F`.
A host that never reports gets immediate release, which suits APIs that keep objects alive for
issued commands (GL with bound textures, sokol). A bindless GL handle, a Vulkan image or a
NoGraphicsAPI descriptor needs the frames. `destroy(ctx)` releases everything at once: the host
waits for its GPU to go idle first.

### `kSelfSubmitting`

An adapter sets `kSelfSubmitting` only if both hold:

1. `commit_upload` **submits the copy by itself** (for example on a dedicated transfer queue).
2. `upload_status` **makes progress without the host recording a frame**.

`wait()` panics without it or `flush` (K5007), and `create()` spins on the placeholder uploads only
when one of them is set. An adapter that submits uploads inside the frame's command buffers must
not set it. Its hosts call `pump()` every frame and show `progress()`.

### `flush`

For APIs whose calls must run on one thread. `begin_upload` and `commit_upload` run on kiln workers,
so such an adapter only writes memory and queues there; `flush` does the rest on the pump thread,
and `upload_status` polls the result (a GL fence, for example). The host calls `create()` and
`pump()` on the thread that owns the graphics context, which is where hosts call them anyway.
With `flush` set, `wait()` works and `create()` waits for the placeholders, as with
`kSelfSubmitting`. An upload committed during one `pump()` is flushed at the start of the next.

### `kCubeTextures`, `kArrayTextures`

The adapter accepts textures of `TextureShape::Cube` or `Array` (`texture-shapes.md`). kiln then
uploads the placeholders of that shape at `create()`, and `TextureDesc::shape` tells the adapter
which image view to create. Without the bit, kiln sends no texture of that shape: a request for it
fails with K5004. The example Vulkan adapter sets both: one bindless binding per shape.

### `kMeshes`

The adapter accepts `UploadKind::MeshPayload`. Without the bit, kiln never calls `begin_upload` for
a mesh, and `request_mesh` / `register_mesh` give a handle that fails with
K5004. A texture-only adapter leaves it clear and can ignore `bufferOffsetAlign`
(`texture-only.md`). The null adapter and the example Vulkan adapter set it.

A slot need not be a descriptor. NoGraphicsAPI forbids rewriting a descriptor while earlier frames may
read it, so `kiln-nga`'s adapter keeps a CPU table: `bind` points kiln's slot at the object's own
descriptor, and the host resolves slot to descriptor index when it writes each frame's root data.
Materials still store the slot once.

### Vertex formats

With fixed-function vertex fetch, the graphics API decides which vertex formats work and the
adapter asks it (`vkGetPhysicalDeviceFormatProperties`). With vertex pulling, the host's shaders
read the payload themselves, so only they decide: the adapter answers with what its host's shaders
decode (`kiln-nga` accepts the float streams its shaders read). kiln has no separate host filter,
because the adapter is host code and knows the shaders. The cook side should match: a host whose
shaders decode nothing cooks with `VertexProfile::Float` (`mesh-format-spec.md` §6), which every
integration example uses.

Indices are U16 or U32 from kiln's cooker, never U8 (`IndexType`), so APIs without 8-bit indices
need no check for kiln's own content.

### Adapter types

The integration examples (`integration-examples.md`) settled into three ways to get bytes onto
the GPU. An adapter picks one per upload kind; `kiln-nga` uses the second for textures and the
third for meshes.

| | Self-submitting | GPU work in `flush` | Written in place |
|---|---|---|---|
| Examples | `vk_adapter.cpp` (`kiln-viewer`, `kiln-vk-basic`) | `kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`, `kiln-nga` textures | `kiln-nga` meshes |
| `begin_upload` (worker) | carves a staging ring | carves a mapped ring or allocates CPU memory; no API call | returns CPU-visible GPU memory (ReBAR); the object is that memory |
| `commit_upload` (worker) | records and submits the copy on a transfer queue | queues the upload | nothing to copy |
| GPU work | on commit | in `flush`, on the pump thread: create the object, copy, fence | none |
| `upload_status` Complete | timeline value reached | fence signaled (GL, NoGraphicsAPI), or flushed (sokol) | flushed: later submissions see the writes |
| `upload_status` Failed | never (a submitted copy cannot fail) | GL out of memory; sokol rejected the image, view or buffer; NoGraphicsAPI texture larger than its heap | never |
| Caps | `kSelfSubmitting` | `flush` set | either |
| Host rules | `pump()` on one thread | `create()` and `pump()` on the graphics thread | as its textures |

Two choices are independent of the type:

- **Binding model:** per-frame lookup or bindless slots (next section).
- **Frames:** a host whose API frees objects that in-flight frames use (Vulkan, NoGraphicsAPI,
  bindless GL handles) reports frames in `PumpOptions`; GL with bound textures and sokol keep
  objects alive for issued commands and report nothing (Frames, above).

### Two binding models

The renderer picks one; `gpu_object(ctx, handle)` (an allocation-free table lookup) serves both.

| | Per-frame lookup | Stable bindless slot (recommended for Vulkan 1.4) |
|---|---|---|
| `bind`, `bindlessSlots` | null, 0 | writes kiln's slot: the kind placeholder at the request, the real image later |
| Material stores | nothing; looks up `gpu_object(ctx, h)` each frame | the slot index, once, at request time |
| Arrival, hot reload | next `gpu_object()` returns the new object | nothing to do: the slot already shows it |
| Fits | sokol / bgfx-style binding, any renderer | descriptor-indexing renderers |

`gpu_object(ctx, h)` returns:

| Asset state | Texture | Mesh |
|---|---|---|
| `Pending`, `MetaReady` | the kind placeholder | null |
| `Ready` | the real object | the payload object |
| `Failed` | the Failed placeholder (`devPlaceholders`), else the kind placeholder | null |
| stale or null handle | the Failed placeholder (`devPlaceholders`), else the `BaseColor` placeholder | null |

With a bindless adapter, a texture's object carries kiln's slot number in `slot` from the request
until the release. A request when all `bindlessSlots` are in use fails with K5004.

### Placeholders through the adapter

- At `create()`, kiln uploads each texture placeholder (built-in or host-supplied RGBA8, see
  `handles-and-states.md`) through `begin_upload` / `commit_upload`. Kind placeholder ids are
  `1 + TextureKind` (1..4). The Failed placeholder of a shape is `failed_placeholder_id(shape)`
  (13..15, `placeholders.h`) and is uploaded only when `devPlaceholders` is on. `Busy` is retried for up to
  10 s (R5j).
- The adapter needs no placeholder ids: `bind` passes the placeholder object like any other.
- When a bindless texture fails and `devPlaceholders` is on, kiln binds its slot to the Failed
  placeholder, so the slot shows the magenta checker.
- kiln passes placeholder objects to `destroy` only at `destroy()`.
- Readiness: with `kSelfSubmitting`, `create()` spins until all placeholders are complete (panics
  after 10 s). Without it, `gpu_object()` returns null for textures until the first `pump()` that sees
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
- `NullAdapterDesc::bindlessSlots` (default 4096): `bind` records what each slot shows. 0 turns
  bindless off, so tests cover both binding models.
- `begin_upload` allocates `size` bytes (`Tag::Payload`) and returns a 1-based object index as
  token and `native`. `busyEveryN` / `failEveryN` inject `Busy` / `Unsupported`.
- `destroy` frees at once and panics on an object whose upload has not completed or that was
  destroyed already. Tests simulate frames through `PumpOptions`.
- `null_adapter_stats()` counts every call; `null_adapter_payload()` and `null_adapter_slot()`
  expose what kiln wrote and bound.

### Threading contract

| Call | Thread |
|---|---|
| `copy_constraints` | the thread calling `create()` |
| `supports_format` | the pump thread |
| `begin_upload`, `commit_upload`, `discard_upload` | **kiln worker threads** (and the thread calling `create()` for placeholders), so a worker decodes straight into staging memory; must be thread-safe |
| `upload_status`, `flush` | the pump thread (`pump()`, `wait()`), and the thread calling `create()` |
| `bind`, `destroy` | the pump thread (requests are pump-thread calls), and the threads calling `create()` / `destroy()` |

Alternative, not taken: every call on the pump thread. The decode would go to a kiln-owned buffer
first, and the copy into staging would serialize on the pump thread. A `workerUploads` flag (or an
`AdapterCaps` bit) could support both; add it only if the external project needs it.

## Rationale

- Function pointers + `user` match the rest of kiln and need no RTTI or vtables across a library
  boundary.
- A VkFormat-valued enum removes a translation table for the primary backend and keeps on-disk data
  and API values identical, while staying a kiln-owned type.
- `Busy` as a normal return gives back-pressure without a separate query.
- A slot number at request time lets a bindless material store it before the asset arrives, so
  arrival and hot reload need no material rebuild. kiln numbers the slots because it already tracks
  every asset's lifetime (`adapter-frames-slots.md`).
- A capability bit is cheaper and clearer than a runtime probe for "can uploads finish without a
  frame".

## Alternatives considered

Full `VkFormat` mirror, dense own enum plus mapping table, virtual interface class,
callback-based completion (no callbacks from workers), slot allocation at arrival only, and
per-frame lookup only: all rejected for the reasons above.

## Consequences / what this constrains later

- Encoders for BC/ASTC need no enum or layout change; their values and table rows exist.
- `reserved[4]` must be null and is the only place residency hooks may go (v0.8) without breaking
  the struct layout. `caps` bits 1..31 are for new capability flags.
- `TextureDesc.firstLevel` is the hook for partial mip loads (v0.8). Progressive mips then arrive
  through further `bind` calls on the same slot.
- Adapter implementations need thread-safe staging, because uploads come from workers.
- A `.mesh` codec changes nothing at the adapter: it always receives decoded bytes.

## Open points for the owner

- Confirm the VkFormat-valued compact `Format` enum (open-questions A5), including block formats
  the v0.5 cooker never writes.
- Confirm `begin_upload` / `commit_upload` on worker threads.
- Confirm the `reserved[4]` tail plus `caps` (vs a versioned `structSize` field).
- Confirm `UploadTarget.object` as the way kiln learns the `GpuObject` of an upload (HANDOFF does
  not say).
- Slot release at unload: decided, kiln owns slot numbers (`adapter-frames-slots.md`).
