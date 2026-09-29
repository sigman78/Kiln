# API friction log

**Purpose.** Every finding about kiln's API from real integrations lives here: the owner's external
project and the integration examples (`design/integration-examples.md`). The table holds each place
where the API is awkward, surprising, missing something, or forces a workaround. The notes after
it record, per integration, how the API mapped and what the implementation needed. Both are the
input for the API review before each milestone closes and before v0.5 is tagged.

Add one row per friction point. Keep "Friction" to what happened, and "Proposed change" to one
concrete suggestion. Status is one of: Predicted (expected, not yet met in code), Open, Accepted,
Rejected (with reason), Done (with version). The integration examples
(`design/integration-examples.md`) turn each Predicted row into Open or Rejected.

| Date | Reporter | Area | Friction | Proposed change | Status |
|---|---|---|---|---|---|
| 2026-09-28 | agent | adapter threading | GL and sokol calls must run on the context's thread, but `begin_upload` / `commit_upload` may run on workers. The adapter can only queue there, and nothing tells it when to do the GPU work. | Optional `Adapter::flush`, called on the pump thread in `pump()` and `wait()` | Done (unreleased): `Adapter::flush`; `kiln-gl` sets it and its host loop no longer flushes |
| 2026-09-28 | agent | adapter / `wait()` | An adapter that is not self-submitting cannot use `wait()` (K5007), even when its work would complete on the pump thread. | `wait()` accepts `kSelfSubmitting` or `flush` | Done (unreleased): `wait()` and `create()`'s placeholder spin accept either (`Runtime.AdapterFlush`) |
| 2026-09-28 | agent | `UploadTarget::object` | The GPU object must exist at `begin_upload`, before the data. GL cannot create names on a worker thread. | Document that `GpuObject` may be an adapter table index filled in later | Rejected as an API change: `kiln-gl` puts its table index in `native` and creates the GL name at flush; this works. Documented in `adapter.md` (`GpuObject`) |
| 2026-09-28 | agent | `publish` without bindless | `publish` gives an `AssetId`; a host with descriptor sets needs its own id-to-material map to know what to rebuild. | Measure in `vk-basic`; maybe a per-frame "changed since" query | Rejected as an API change: `kiln-vk-basic` takes the events (Ready, Changed, Failed carry the handle), maps each texture handle to the materials that use it (about 20 lines) and rewrites those sets per frame slot; `publish` is not needed. `kiln-gl` calls `gpu()` per draw and needs nothing |
| 2026-09-28 | agent | formats | A format the adapter rejects fails the asset (K5004); there is no fallback, so each API needs a cook target that matches it. | Named `TargetProfile` presets per API family | Open (2026-09-29): met with BC. NoGraphicsAPI has no BC1 or BC4, and sokol no BC1 RGB, so a 1-channel mask (BC4 in the desktop table) fails to load on `kiln-nga`. A preset per API family, or a per-target format exclusion list, would let a host cook what its adapter samples |
| 2026-09-29 | agent | store invalidation | After the default target switched to BC, the examples kept loading their old uncompressed store files: a named store file is trusted while it exists, whatever its stored `cookHash` says (open-questions R9). | The provider checks the stored `cookHash` and re-cooks on a mismatch | Open |
| 2026-09-28 | agent | `.mesh` vertex data | Quantized streams need shader decoding and part dequantization in every renderer. | `VertexProfile::Float` for simple renderers; decode snippets for the quantized profile | Accepted: `VertexProfile::Float` is done (unreleased) and `kiln-gl`'s shaders read plain floats; the snippets stay open |
| 2026-09-28 | agent | `destroy_deferred` | GL and sokol can destroy at once; check the contract does not force frame counting on them. | Clarify in `adapter.md` | Rejected: `kiln-gl` deletes at once and GL keeps objects alive for issued commands; no frame counting needed. Documented in `adapter.md` |
| 2026-09-28 | agent (`kiln-gl`) | texture layout | `UploadDesc` gives the `TextureDesc` but not the level offsets, so every adapter re-implements kiln's layout rule (`vk_adapter.cpp` and `gl_adapter.cpp` each copy `texture_layout`). | A public `texture_level_layout(TextureDesc, CopyConstraints, offsets, pitches)` in `adapter.h`, or the offsets in `UploadDesc` | Done (unreleased): `texture_level_layout()`; both example adapters use it |
| 2026-09-28 | agent (`kiln-gl`) | `destroy_deferred` during an upload | Unloading an asset whose upload is in flight calls `destroy_deferred` on its object, and kiln never polls that token again. The adapter must notice and retire the upload itself, or it leaks staging space. Not in the contract. | Document it in `adapter.md` (the token is abandoned; the adapter retires it) | Done: `adapter.h` and `adapter.md`; superseded (unreleased) by `adapter-frames-slots.md`: kiln keeps polling an abandoned upload and destroys its object itself |
| 2026-09-28 | agent (`kiln-gl`) | cube maps | Faces arrive as authored (+X -X +Y -Y +Z -Z, a left-handed frame); a right-handed renderer must flip Z when it samples. `kiln-viewer` and `kiln-gl` both found this by hand. | State the convention and the flip in `texture-shapes.md` | Done: `texture-shapes.md`, "Handedness" |
| 2026-09-28 | agent (`kiln-gl`) | material bindings | A `TextureBinding` with `kTextureExternal` has `textureId` 0, so the host resolves the name itself (`resolve_asset_name`) and hashes it to find the handle; embedded ones could use `find_texture(ctx, textureId)` directly. | A helper that returns the texture asset name of a binding | Done (unreleased): `texture_asset_name(meshName, view, binding, out, cap)`; the six examples dropped their copies |
| 2026-09-28 | agent (`kiln-gl-bindless`) | bindless slots | `publish` and `begin_upload` give only the `AssetId`, so every bindless adapter keeps its own id-to-slot map under a mutex (`vk_adapter.cpp`, `gl_adapter.cpp`), although kiln stores the object `acquire` returned for each asset. | Pass the acquired `GpuObject` in `UploadDesc` and to `publish` | Done (unreleased), differently: kiln numbers the slots; `bind(slot, obj, shape)` replaces `acquire` and `publish`, and no adapter keeps an id map (`design/adapter-frames-slots.md`) |
| 2026-09-28 | agent (`kiln-gl-bindless`) | `destroy_deferred` | Bindless GL handles must stay resident until earlier frames finish, so "GL may free at once" holds only for bound textures. The adapter uses `flush` as its frame boundary to fence retired handles and slots. | Documented in `adapter.md` | Rejected as an API change: `flush` gives the adapter the frame boundary it needs |
| 2026-09-28 | agent | host loop | sokol_app owns the main loop (callbacks); `pump()` and `wait()` assume the host owns it. | Measure in `sokol` | Rejected: `kiln-sokol` calls `create()` in the init callback and `pump()` in the frame callback; nothing needs `wait()` or a loop of its own |
| 2026-09-28 | agent (`kiln-sokol`) | placeholders | sokol validates that every texture slot a shader declares is bound, so the host needs a texture for slots a material leaves empty. kiln holds a placeholder per kind and shape, but a host can reach one only through a handle (`gpu()` of a null handle gives the 2D base-color one; there is no cube one). The example makes its own white and black textures. `kiln-vk-basic` meets the same wall (every binding of a descriptor set written) and avoids it with `descriptorBindingPartiallyBound`, which a plainer Vulkan host may not want. | `placeholder_object(ctx, TextureKind, TextureShape)` returning the placeholder's `GpuObject` | Done (unreleased): `kiln-sokol` binds kiln's placeholders instead of its own white and black textures; `kiln-vk-basic` writes them into bindings a material lacks and no longer needs `descriptorBindingPartiallyBound` |
| 2026-09-28 | agent (`kiln-vk-basic`) | events | A host that caches GPU objects (descriptor sets) must know which events change what `gpu()` returns; the header did not say. | State it on `EventKind` | Done: `assets.h` (`EventKind`): Ready, Changed, Failed may change it; MetaReady does not |
| 2026-09-28 | agent (`kiln-nga`) | bindless slots | The adapter contract says a bindless adapter "overwrites the slot it acquired", but NoGraphicsAPI (like any API without update-after-bind semantics) forbids rewriting a descriptor that in-flight frames may read. | Allow a slot to be an indirection the host resolves per frame; say so in `adapter.md` | Done: documented in `adapter.md`; `kiln-nga` keeps a CPU slot table |
| 2026-09-28 | agent (`kiln-nga`) | frame boundary | Every adapter that frees GPU objects late needs to know when the host's frames finish, and each invents its own hook: `adapter_retire(completedFrame)` (Vulkan), fences set in `flush` (bindless GL), `nga_adapter_retire` (NoGraphicsAPI). kiln has no frame concept; `flush` runs per `pump()`, which is per frame only by habit. | An optional `Adapter::frame_completed(user, frame)` the host calls through kiln, or a frame counter in `PumpOptions` passed to `flush` | Done (unreleased): `PumpOptions::frame` / `completedFrame`; kiln calls `Adapter::destroy` (replacing `destroy_deferred`) after the frames, and the adapters' retire lists are gone (`design/adapter-frames-slots.md`) |
| 2026-09-28 | agent (`kiln-nga`) | vertex formats | With vertex pulling the host's shaders, not the API, decide which vertex formats work, yet `supports_format(VertexBuffer)` is the adapter's answer; `kiln-nga`'s adapter hard-codes what its example shader decodes. | Let the host restrict vertex formats (e.g. a `ContextDesc` filter), or document that the adapter answers for the host's shaders | Done (unreleased), as documentation: `supports_format` answers for the host's shaders (`adapter.h`, `adapter.md` "Vertex formats"); no filter, since the adapter is host code |
| 2026-09-28 | agent (`kiln-nga`) | naming | kiln's free function `gpu(ctx, handle)` collides with NoGraphicsAPI's `namespace gpu` in any file with `using namespace kiln`; the host writes `kiln::gpu(...)`. Other APIs may use the name too. | Rename to `gpu_object()` (breaking, pre-1.0) or leave it and note the qualification | Done (unreleased): `gpu_object()`; `kiln-nga` drops the `kiln::` qualification |
| 2026-09-28 | agent (`kiln-sokol`) | index types | The `.mesh` format allows 8-bit indices; sokol (and D3D11, Metal, WebGPU) has none. The cooker never writes them, but a host cannot know that from the API. | State in `mesh-format-spec.md` / `mesh.h` that the cooker emits 16- or 32-bit indices only | Done (unreleased): `IndexType` in `mesh.h`, `mesh-format-spec.md` §5.5, `adapter.md` ("Vertex formats") |
| 2026-09-28 | agent (`kiln-sokol`) | mesh payload | One payload buffer holds vertices and indices. sokol takes that (one buffer with both usages) everywhere but WebGL2, which needs separate buffers (`sg_features.separate_buffer_types`). | Nothing now; revisit with a WebGL/WebGPU target (two uploads, or index offset metadata the host can split on) | Predicted |
| 2026-09-28 | agent (`kiln-sokol`) | upload memory | sokol creates immutable images from data it copies, so the adapter hands kiln CPU memory it allocates per upload and frees at flush: kiln decodes into it, sokol copies it again. | None needed: per-upload memory is the natural shape for such APIs; note it in `adapter.md` | Rejected as an API change: documented in `adapter.md` (`begin_upload`) |
| 2026-09-29 | review (`kiln-sokol`) | upload failure | An adapter can fail an upload only in `begin_upload`. sokol creates the image in `flush`, after `commit_upload`, and can still reject it (a full pool, an invalid description); `is_upload_complete` has only "not yet" and "done", so the asset becomes Ready with an empty object. The example sizes sokol's pools so this does not happen and logs the rest. | `is_upload_complete` returns a tri-state (pending, done, failed), failed ending in `Failed` with K5004 | Done (unreleased): `upload_status` returns `UploadStatus { Pending, Complete, Failed }`. sokol, GL (out of memory) and NoGraphicsAPI (a texture larger than its heap, which it used to retry forever) report failures |
| 2026-09-29 | review (all examples) | events | A reload emits no MetaReady, so a model that goes Failed -> Ready (repaired while hot reload runs) announces itself only with Ready. Five examples set up on MetaReady / Changed and missed it. | Nothing in the API: track the content version the host set up for (the examples now do); `EventKind` docs could name the case | Done (unreleased): examples fixed, and `EventKind` names the case; see below |
| 2026-09-29 | review (LeakSanitizer) | cook provider lifetime | `destroy(ctx)` with a cook provider still installed leaks the provider, and the provider registry keeps the dead `Context*` as a key, so a later context at the same address would find it. The header says to call `uninstall_provider` first; 20 tests did not. | `kiln_runtime` cannot call into `kiln_cook`; give `CookProvider` a `release` callback that `destroy()` calls, so forgetting is harmless | Open (tests fixed) |
| 2026-09-29 | review (adapters) | `Busy` | `Busy` is not defined as temporary. `kiln-nga` returns it for a texture larger than its whole staging ring or a mesh larger than its whole mesh heap, which can never succeed, and kiln retries `Busy` every pump without limit, so the asset stays Pending forever. GL and Vulkan reject oversized uploads; for exhausted object tables GL, sokol and NGA say `Busy` but Vulkan says `OutOfMemory`. | Define `Busy` as "retry can succeed" in `adapter.h`; fix `kiln-nga`; align the object-table case; optionally a retry cap in kiln | Done (unreleased): `adapter.h` / `adapter.md` define it; `kiln-nga` returns `Unsupported` for an upload larger than its ring or mesh heap, GL `Unsupported` instead of `OutOfMemory`; a full object table is `OutOfMemory` in every example adapter. No retry cap: `Busy` is now only returned when retrying can succeed |
| 2026-09-29 | review (adapters) | terminal upload status | `upload_status` says "polled until not Pending" but not that kiln never asks again, and adapters rely on it: GL, sokol and NGA recycle the ticket at the first terminal answer (a second poll would now say `Failed`), Vulkan keeps it with the object. `destroy`'s doc still says "its upload has completed", although kiln now also destroys `Failed` objects, which may be half made. | State both in `adapter.h`: kiln stops polling at the first terminal status; `Failed` means no work on the upload remains; `destroy` takes objects of failed uploads | Done (unreleased): `adapter.h`, `adapter.md` |
| 2026-09-29 | review (adapters) | failure reason | `UploadStatus::Failed` carries no reason, so K5004 says only "the adapter failed the upload"; the adapters log the cause themselves. | A completion result with a `Status` (out of memory, unsupported, device error) that reaches the diagnostic | Done (unreleased): `upload_status(user, token, Status* failure)`; sokol tells a full pool (`OutOfMemory`) from a refused resource (`Unsupported`), GL reports `OutOfMemory`, NGA both |
| 2026-09-29 | review (adapters) | discard | kiln calls `commit_upload` after every successful `begin_upload`, even when reading or decoding the payload failed, so the adapter creates and copies an object nobody will use, which kiln then orphans and destroys. | An optional `discard_upload(token)`, called exactly once instead of `commit_upload` when the CPU side failed | Done (unreleased): `Adapter::discard_upload`, in every example adapter; `kiln-vk-smoke` checks the Vulkan one on a GPU. A truncated store file does not reach it: the meta stage already checks every level's range against the file size |
| 2026-09-29 | review (adapters) | `bind` timing | `bind`'s "slot shows `obj` from now on" does not say which frames see the change. `kiln-gl-bindless` copies its table per frame so frames in flight keep the old handle; `kiln-nga` resolves slots when it records a frame. | Say in `adapter.h`: frames recorded after the pump see the new object; frames already in flight may see either, and the old object stays alive until they complete | Done (unreleased): `adapter.h`, `adapter.md` |

Review this table at the end of every milestone; resolved rows that change the API get a
`CHANGELOG.md` entry with migration notes.

## Integration notes

What each integration example showed about the API: how it mapped, what the adapter and host
needed, and quirks of the graphics API that are not kiln's (kept so the next example does not
rediscover them). The design and the mapping tables are in `design/integration-examples.md`.

### `kiln-gl` (OpenGL 4.6, textures bound per draw)

- The host-side `gl_adapter_flush()` before `pump()` was the only line a GL host needed beyond a
  Vulkan host, and forgetting it stalled every upload silently. `Adapter::flush` removed it.
- `begin_upload` runs on a worker, where GL cannot create objects: `GpuObject::native` holds the
  adapter's own table index, and the GL texture or buffer is created in `flush`.
- Uploads complete one to two frames after they are committed: the flush issues the copies and a
  fence; `is_upload_complete` polls the fence on the pump thread.
- `wait()` was unusable for GL (it required `kSelfSubmitting`); it now accepts `flush`.
- The adapter copied kiln's level layout rule until `texture_level_layout()` existed.
- `gpu()` per draw means placeholder to real texture and hot reload need no host bookkeeping.
- `VertexProfile::Float`: the shaders read plain float attributes; the vertex arrays come straight
  from `mesh_view()` layouts, built at `MetaReady` before the payload arrives.
- Quirk (not kiln): cube maps sample in a left-handed frame; the first render was mirrored until the
  shaders flipped Z, as `kiln-viewer` does.

### `kiln-gl-bindless` (OpenGL 4.6 + `ARB_bindless_texture`)

- `acquire` runs on the requesting thread, where GL cannot be called. It works because the handle
  table is persistently mapped memory and the placeholders are already resident: with `flush`,
  `create()` waits for them. Before `flush`, a slot could have held a null handle.
- `publish` gives an `AssetId`, so the adapter keeps an id-to-slot map under a mutex, exactly as the
  Vulkan adapter does, although kiln stores the acquired object of every asset (table row).
- A resident handle may not be made non-resident while earlier frames can sample it, so bindless GL
  needs deferred destruction after all; the adapter fences a batch of retired slots and textures in
  `flush`, which runs once per frame.
- The materials store slot numbers once, when the textures are requested (`gpu(ctx, h).slot` right
  after `request_texture`); the only per-frame `gpu()` call left is the mesh buffer.
- Quirk (not kiln): on NVIDIA the global `GL_TEXTURE_CUBE_MAP_SEAMLESS` does not apply to bindless
  handles; the cube sampler sets it itself. After that the frame matches `kiln-gl` pixel for pixel.

### `kiln-sokol` (sokol_gfx + sokol_app; D3D11 on Windows, GL on Linux, Metal on macOS)

- The host loop fits sokol_app's callbacks: `create()` in init (after `sg_setup()`, because
  `create()` runs `flush` while it waits for the placeholders), `pump()` in frame, `destroy()` in
  cleanup. No thread of its own, no `wait()`.
- The adapter is the simplest so far: workers write into per-upload CPU memory; `flush` makes the
  images and buffers (`sg_make_image` with all levels, `sg_make_buffer`) and frees the memory;
  an upload is complete once flushed, because sokol orders resource use itself. `destroy_deferred`
  destroys at once: sokol defers the release where the backend needs it.
- `texture_level_layout()` with copy constraints of 1 (row pitch and offset) gives exactly sokol's
  image data: one tightly packed range per level, cube faces in kiln's order (+X −X +Y −Y +Z −Z).
- One payload buffer serves as vertex and index buffer; the bindings take the stream and index
  offsets from the LOD record. Pipelines bake the vertex layout and the index type, so the host
  builds one per (layout, index type) at `MetaReady`.
- sokol wants every declared texture slot bound: the host keeps a white 2D and a black cube texture
  for empty slots (table row: placeholders).
- The D3D11 frame differs from the GL one only along cube-seam reflections and at grazing angles
  (1.7% of pixels, 0.2% by more than 4 levels): API filtering differences, not the mapping.
- Quirks (not kiln): sokol has no readback, so `--dump` reaches into D3D11 or GL. `sapp_color_format()`
  did not match `sg_pixel_format` at the pinned commit; `sglue_environment().defaults` does.
  sokol_app has no hidden window, so `--offscreen` shows one until the scene settles. A sokol
  validation panic aborts, which on Windows opens a dialog and hangs a scripted run.

### `kiln-vk-basic` (Vulkan 1.4, a descriptor set per material)

- The adapter is the viewer's with bindless off (`AdapterDesc::bindless = false`): no `acquire`, so
  no slots; `publish` stays, only to advance the upload watermark the frame submit waits on. The
  host turns a `GpuObject` into an image view with `adapter_texture()`. Nothing else changed.
- Each material has one descriptor set per frame in flight and a stamp. A texture event (Ready,
  Changed, Failed) bumps the stamp of every material that uses the texture; each frame rewrites
  only the current slot's sets whose stamp moved, after `renderer_wait_frame()` freed that slot.
  WaterBottle with its sky: 6 invalidations and 15 set writes for the whole load (2 frame slots).
- The first write of a set happens before the texture arrives: `gpu()` gives the placeholder of the
  texture's kind (and shape: the sky's cube placeholder), which the self-submitting adapter had
  ready when `create()` returned.
- A changed mesh (hot reload) may bring other materials and layouts, so the host waits for the GPU
  and makes the sets again; the old payload goes through `destroy_deferred` as usual.
- Hot reload was run by hand (`--watch`, the source touched): the model and each texture reported
  Changed, and the sets followed. No Vulkan validation layer on the dev machine, so the descriptor
  usage is unvalidated.
- The frame plumbing is the viewer's (`kiln_example_vk`); it now takes a host's SPIR-V and material
  set layout. The viewer's own frame is byte-identical after that change.
- Against `kiln-gl`: 19.5% of pixels differ, 0.7% by more than 4 levels (hardware sRGB encode
  against the shader's; no anisotropic filtering here, 8x there).

### `kiln-nga` (NoGraphicsAPI on Vulkan; built, not yet run)

- Built on MSVC, clang-cl and the clang GNU driver on Windows (and Linux in CI); not run: the dev
  machine's GPU lacks `VK_EXT_descriptor_heap`. It fails at `create_device` with a clear message.
- Mesh payloads: `begin_upload` hands kiln CPU-visible GPU memory from a heap, so kiln writes the
  payload where the shaders read it (no staging copy), and the upload is complete at `commit_upload`.
  The first adapter to exercise that path of the contract.
- The shaders pull vertices through GPU pointers (`stream0`/`stream1` in the root data) and read the
  `.mesh` LOD records directly: stream offsets, strides and attribute offsets go into each draw's
  root, and the index range is a `GpuRange` into the same payload. No vertex-input state at all.
- Textures: staging ring, placement in a texture heap, `copy_memory_to_texture` per level (kiln's
  `texture_level_layout` gives the ranges), a descriptor at a fresh index, all recorded in `flush`
  and submitted on queue 0 with the adapter's own timeline semaphore.
- Slots: the descriptor-rewrite rule forced the CPU slot table (table row); the host resolves the
  six texture slots of a draw when it writes the root, which it rewrites every frame anyway.
- `create()` and `pump()` must run on the thread that submits to queue 0 (the adapter's `flush`
  submits there too): the same rule as GL and sokol, now for a Vulkan-based API.
- Not kiln: NoGraphicsAPI's CMake needs the Vulkan SDK package and exports install targets, so the
  example compiles its source file itself and builds the Vulkan loader from source; shaders need
  Slang (a 63 MB download on Windows). The root layout was checked against Slang's reflection
  (`-reflection-json`) before the C++ side was written.

### Frames and kiln-owned slots (`design/adapter-frames-slots.md`)

- The four example adapters lost their id → slot maps, placeholder id tables, retire lists and
  "destroyed while uploading" flags: about 340 lines net. `destroy` is an immediate free in each.
- Hosts report frames in one line at `pump()`: the Vulkan examples pass the frame numbers the fence
  wait already had, `kiln-nga` its timeline values, `kiln-gl-bindless` a small fence ring (it had
  none before; the adapter used to fence retired handles in `flush`). `kiln-gl` and `kiln-sokol`
  report nothing: their APIs keep objects alive for issued commands.
- The Vulkan upload watermark moved from `publish` into `is_upload_complete`, which returns true
  exactly when kiln starts to use the object.
- `destroy(ctx)` now frees everything at once, so the host must wait for its GPU to go idle first.
  Every example already did.

### Host helpers (review part B)

- `placeholder_object()` replaced `kiln-sokol`'s own white and black textures, and lets
  `kiln-vk-basic` write every binding of a material set; both frames are unchanged.
- `texture_asset_name()` replaced a 6-line helper each example had copied; `kiln-nga` calls
  `gpu_object()` without the `kiln::` qualification.

### Data contract and adapter types (review parts C, D)

- Index sizes and vertex-format ownership are now stated where a host looks: `IndexType` in
  `mesh.h`, `supports_format` in `adapter.h`, and "Vertex formats" in `adapter.md`.
- `adapter.md` has an "Adapter types" section: self-submitting, GPU work in `flush`, written in
  place, with the example behind each.
- Still left until they bite: per-API cook-target presets (no example hit a format rejection) and
  WebGL2's separate vertex and index buffers (the "mesh payload" row).

### Code review of the examples (2026-09-29)

- Bindless GL: `bind` rewrote the one mapped handle table while earlier frames could still read
  it (a data race in GL terms, though both handles stay resident). The adapter now keeps the
  handles on the CPU and `gl_handle_table(a, frame)` copies them into one of `tableFrames` tables
  per frame. The host's fence ring could overflow after a wait timed out; it now waits while full,
  and `GL_WAIT_FAILED` panics instead of counting as done (also in the adapter's upload fences).
- Failed -> Ready: every example records the model version it set up for and sets up again for any
  newer one (table row).
- sokol: pools default to 128 images while the adapter allowed 4096 objects. The host now sizes
  the pools from `SokolAdapterDesc`, the adapter checks them, and `make_object` checks every
  creation (table row: upload failure).
- nga: the offscreen color target had no barrier between frames, only the depth target; added.
- sokol on macOS: `LANGUAGE OBJC` needs `enable_language(OBJC)` first.

### Adapter review: shared helpers (2026-09-29)

- `examples/adapter_support` holds CPU-only helpers for the example adapters, apart from the
  GLFW-dependent `examples/common`. First: `ex::StagingRing` (reserve with alignment, release in
  any order, space back in reservation order, `can_fit()` to tell Unsupported from Busy). `kiln-gl`
  and `kiln-nga` dropped their identical copies (about 40 lines net each). The Vulkan adapter keeps its
  timeline-value ring for now.
- Second: `ex::UploadPool<T>`, the records behind upload tokens. One `UploadState` per upload
  replaced up to five bools per adapter (GL's `committed` was written and never read), and
  `advance()` asserts each transition; tokens are generation-checked. `kiln-gl`, `kiln-sokol` and
  `kiln-nga` use it. Vulkan keeps its `ObjectState`, since its upload and object records are one.
- Third: `ex::CommitQueue`, the handoff from `commit_upload` (workers) to `flush` (pump thread),
  with its own lock: push from any thread, `take()` swaps two reserved buffers, so the GPU work
  runs outside the lock and nothing allocates. `kiln-nga` pushes an index again to retry it.
  Replaces the `committed` / `flushing` vector pairs in GL, sokol and NGA.
- Fourth: every example adapter takes an `Allocator` and reports `ex::AdapterStats`, logged at exit.
  The four integration examples now report the same scene the same way (21 live objects, 89.9 MB
  committed); only `kiln-vk-basic` returns `Busy` (57 times), because its staging ring is 64 MB
  against GL's 96 MB. `kiln-nga`'s capacities moved from constants into its desc. Promote any of them
  into the library only after they prove themselves here.
