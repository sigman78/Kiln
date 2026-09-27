# Example Vulkan adapter and viewer (M4)

Status: **Proposed** (2026-09-27; adapter and smoke test landed the same day). Owner decisions taken at M4: Vulkan headers and volk through
FetchContent (no SDK required), GLFW for the window, SPIR-V committed next to the GLSL and
regenerated when a compiler is found, CI compiles the viewer but does not run it.

## Decision

`examples/viewer/` holds two things with a hard line between them:

- **`vk_adapter.{h,cpp}`: the example adapter.** A `kiln::Adapter` on raw Vulkan 1.4 that is
  self-submitting (dedicated transfer queue, one timeline semaphore) and bindless (one sampled-image
  descriptor array; `acquire` writes the kind placeholder into a slot, `publish` overwrites the same
  slot). It exists to show that an adapter is a few hundred lines. It knows nothing about windows,
  swapchains, pipelines or drawing.
- **`main.cpp`, `vk_device.{h,cpp}`: the viewer.** Instance and device bring-up, a GLFW window and
  swapchain, one graphics pipeline per vertex layout, draws per spec §8, and an offscreen mode that
  renders a fixed number of frames without a window and writes a PNG.

The library still never sees a Vulkan header: everything here is an example, built only with
`KILN_BUILD_VIEWER=ON`, and links `kiln_runtime` (plus `kiln_cook` for cook-on-miss when present).

### Adapter design

| Concern | Choice |
|---|---|
| Queues | Graphics queue for the viewer; a dedicated transfer queue for uploads when the device has one, else a second graphics-capable queue, else the graphics queue itself (still self-submitting). |
| Ownership | Buffers and images use `VK_SHARING_MODE_CONCURRENT` over the graphics and transfer families, so no queue-family ownership transfer barriers are needed. Slightly slower on some drivers; keeps the adapter small. |
| Staging | One host-visible, host-coherent buffer (`stagingBytes`, default 64 MiB) used as a ring. `begin_upload` carves `size` bytes at `alignment`; when the ring cannot fit the request it returns `Code::Busy` and kiln retries on a later pump. A ring entry is released when the upload that used it has completed. |
| Threads | `begin_upload` and `commit_upload` run on kiln workers: one mutex guards the ring, the transfer command pool and the object tables. `is_upload_complete` reads the timeline counter without the lock. `publish`, `destroy_deferred` and the per-frame retire run on the pump thread, which is the render thread. |
| Completion | Each `commit_upload` records one command buffer (copies plus the layout transition to `SHADER_READ_ONLY_OPTIMAL` or, for meshes, a buffer barrier) and submits it on the transfer queue with `vkQueueSubmit2`, signaling the timeline semaphore with the upload's value. The token is that value, issued at `begin_upload` because kiln needs it before commit; timeline values must be signaled in order, so a commit that overtakes an earlier upload's commit is held until that one arrives (kiln always commits what it began). `is_upload_complete(token)` is `counter >= token`. Command buffers are recycled once their value has completed. When the transfer queue is the graphics queue, submission happens inside `is_upload_complete` on the pump thread so no two threads use one queue. |
| Frame ordering | The viewer waits on the timeline semaphore at `vk_adapter_upload_watermark()` in each frame submit, so a slot that was published this pump is safe to sample this frame. |
| Objects | `GpuObject::native` is a 1-based index into the adapter's object table (image + view + memory, or buffer + memory). `slot` is the bindless slot for textures and is also set on the upload's object, so `gpu()` keeps returning the stable slot once the texture is Ready. Meshes have no slot. An upload larger than the whole ring is `Unsupported`, not `Busy`. |
| Placeholders | kiln publishes ids 1..4 (kind placeholders) and 15 (Failed) at `create()`. The adapter records their objects per id, and `acquire` writes the placeholder for `texKind` into every new slot. A null object in `publish` at unload frees the slot after `framesInFlight` frames. |
| Deferred destroy | `destroy_deferred` queues the object with the current frame number; `vk_adapter_retire(frame)` frees everything queued at least `framesInFlight` frames ago. The viewer calls it after waiting the frame fence. |
| Memory | One `vkAllocateMemory` per object. Enough for an example; a real renderer sub-allocates. |
| Copy constraints | `optimalRowPitchAlign = 1`, `optimalOffsetAlign = 16`, `bufferOffsetAlign = 256`. The adapter recomputes the level offsets the same way `src/runtime/loader.cpp` (`texture_layout`) does, so copy regions match what kiln wrote. |
| Formats | A `static_assert` table in `vk_adapter.cpp` checks every `kiln::Format` value against `VK_FORMAT_*`, which is the check `adapter.md` asked for. `supports_format` asks `vkGetPhysicalDeviceFormatProperties` for sampled-image or vertex-buffer support. |

### Viewer design

- **Device.** volk, Vulkan 1.4 instance, optional validation layer (`--validate`), physical device
  with a Vulkan 1.4 driver, features: timeline semaphores, descriptor indexing (runtime arrays,
  partially bound, update-after-bind, non-uniform indexing), synchronization2, dynamic rendering,
  maintenance5 (`vkCmdBindIndexBuffer2`), buffer device address.
- **Binding model A** from spec §8: one pipeline per distinct `VertexLayout`, created when a
  `MetaReady` event brings a layout not seen before. Attribute locations are fixed by semantic
  (position 0, normal 1, tangent 2, uv0 3, uv1 4, color 5). A layout that lacks an attribute binds
  a small zero buffer at instance rate for that location, so every shader input is always fed.
  Specialization constants tell the shader which attributes are real and whether the normal is
  octahedral.
- **Draw.** Per part and LOD 0: push constants carry the model matrix (part translation and
  rotation, parent chain resolved), `posScale` / `posBias`, the base-color bindless slot from
  `gpu(ctx, textureHandle)`, and flags. `vkCmdDrawIndexed` per submesh with the spec's index
  offset formula. A frame uniform buffer holds the view-projection matrix.
- **Streaming.** A boot group (the meshes named on the command line) is waited on before the first
  frame with `wait()`; their textures are requested afterwards and stream in with a per-frame
  `uploadBytes` budget, so the first frames show placeholders. The viewer logs per-frame CPU time
  and flags frames above a threshold.
- **Offscreen.** `--offscreen --frames N --dump out.png` renders into an image instead of a
  swapchain, reads it back after the last frame and writes a PNG (the encoder is the test helper
  `tests/png_writer.h`). This mode never initializes GLFW and is what a headless machine or a
  software Vulkan driver can run.
- **Camera.** Orbit around the union of the loaded models' bounds; mouse drag and wheel in the
  window, fixed in offscreen mode.

### Shaders

`shaders/mesh.vert` and `shaders/mesh.frag` (GLSL 460, Vulkan) with their `.spv` committed next
to them and embedded into the executable at build time as `uint32_t` arrays. A `viewer-shaders`
target rebuilds the `.spv` from the GLSL when `glslang` or `glslc` is on `PATH`; it is never part
of `ALL`, and the build does not need a shader compiler.

### Dependencies

All example-only, none reach the library (`dependencies.md`, `third_party/README.md`):

| Dependency | Pin | License |
|---|---|---|
| Vulkan-Headers | `vulkan-sdk-1.4.357.0` = `e3b1eec08173d6b825cd3ac88c885a63b621504a` | Apache-2.0 / MIT |
| volk | `7f46f79751d7e3b3a6df20e38d3e3986585bcdf4` (1.4.364) | MIT |
| GLFW | `3.5.1` = `70a9bb3881fe80fd483236e2b203cb451c6ecf40` | zlib |

## Rationale

- Concurrent sharing over ownership transfers: two barriers per upload on two queues is the part
  of a Vulkan uploader that is easy to get wrong and adds nothing to what the example shows.
- A timeline semaphore per transfer submit, polled by value, matches `kSelfSubmitting` exactly and
  gives the viewer one wait per frame.
- Embedding SPIR-V removes a runtime file lookup and a build-time compiler requirement at once.
- Offscreen mode is the only way the viewer can be run on a machine without a display, and it is
  the shape a CI run would take later.

## Alternatives considered

| Alternative | Why not now |
|---|---|
| Vertex pulling (spec §8 model B) | One pipeline for all layouts, but the shader has to decode every quantized format; model A keeps the shader trivial and shows `VertexLayout` mapping directly to vertex input state. |
| A thin abstraction layer over Vulkan | Hides the code the example exists to show (open question A8, resolved: raw Vulkan). |
| Vulkan SDK via `find_package` | Needs an SDK on every machine and runner; headers plus volk need nothing. |
| Running the viewer in CI under lavapipe | Owner chose compile-only for now; the offscreen mode keeps the door open. |

## Consequences / what this constrains later

- Hot reload (M5) needs nothing new at the adapter: `publish` on the same slot and
  `destroy_deferred` of the old object already cover it.
- Progressive mips (v0.8) arrive as further `publish` calls on a slot; the adapter would then keep
  per-level views.
- `vk_adapter.cpp` is the reference for the external project's adapter; keep it readable over
  clever.

## Open points for the owner

- `framesInFlight` default 2; confirm.
- Whether the viewer should also demonstrate the per-frame-lookup binding model (a `--no-bindless`
  switch) or bindless only.
