# Example Vulkan adapter and viewer (M4)

**Status:** Proposed (awaiting owner sign-off). Implemented in M4 (2026-09-27) in
`examples/viewer/`; the M4 done-when criteria were met on a GTX 1080 Ti. Owner decisions taken at
M4: Vulkan headers and volk through FetchContent (no SDK required), GLFW for the window, SPIR-V
committed next to the GLSL and regenerated when a compiler is found, CI compiles the viewer but
does not run it.
**Decides:** The shape of the example Vulkan 1.4 adapter and viewer, their dependencies and the
offscreen mode.

## Decision

`examples/viewer/` holds two things with a hard line between them:

- **`vk_adapter.{h,cpp}`: the example adapter** (namespace `kiln::vkx`). A `kiln::Adapter` on raw
  Vulkan 1.4 that is self-submitting (dedicated transfer queue, one timeline semaphore; an
  upload gets its timeline value at `commit_upload`, so uploads submit in commit order and a
  small one never waits for a larger one that began earlier) and
  bindless (one descriptor array per `TextureShape`: 2D, cube, array, sharing one slot index;
  `bind` writes the image kiln names into kiln's slot, in the binding of its shape). It declares `kCubeTextures` and `kArrayTextures`. It shows that an adapter is a few hundred lines. It knows
  nothing about windows, swapchains, pipelines or drawing.
- **The viewer:** `vk_device.{h,cpp}` (instance and device bring-up, shared with the smoke test),
  `viewer_render.{h,cpp}` (swapchain or offscreen image, frames in flight, one pipeline per vertex
  layout), `viewer_math.h`, and `main.cpp` (scene, camera, streaming, draws per spec §8).

The adapter, the device code and `viewer_render` form the library `kiln_example_vk`, which
`kiln-vk-basic` (`examples/vk-basic`, the non-bindless integration example) links too: its adapter
runs with `AdapterDesc::bindless = false`, and `RendererDesc` takes its SPIR-V and material set
layout.

Targets, all built only with `KILN_BUILD_VIEWER=ON` (ON in every preset):

- `kiln-viewer`: links `kiln_runtime`, plus `kiln_cook` for cook-on-miss when it is built.
- `kiln-vk-smoke` (`adapter_smoke.cpp`): loads the golden store through the real adapter with no
  window. Run by hand; it needs a Vulkan driver, so it is not a CTest.
- `viewer-assets` (`fetch_assets.cmake`): fetches the CC0 Khronos demo models, pinned and
  hash-checked, into the git-ignored `examples/assets/khronos/`. Not part of `ALL`.

The library never sees a Vulkan header.

### Adapter design

| Concern | Choice |
|---|---|
| Queues | Graphics queue for the viewer; a dedicated transfer queue for uploads when the device has one, else a second graphics-capable queue, else the graphics queue itself (still self-submitting). |
| Ownership | Buffers and images use `VK_SHARING_MODE_CONCURRENT` over the graphics and transfer families, so no queue-family ownership transfer barriers are needed. Slightly slower on some drivers; keeps the adapter small. |
| Staging | One host-visible, host-coherent buffer (`AdapterDesc::stagingBytes`, default 64 MiB) used as a ring. `begin_upload` carves `size` bytes at `alignment`; when the ring cannot fit the request it returns `Code::Busy` and kiln retries on a later pump. An upload larger than the whole ring is `Unsupported`. A ring entry is released when its upload has completed. |
| Threads | `begin_upload` and `commit_upload` run on kiln workers: one mutex guards the ring, the transfer command pool and the object tables. `upload_status` reads the timeline counter under the lock. `bind` and `destroy` run on the pump thread, which is the render thread. |
| Completion | Each `commit_upload` records one command buffer (copies plus the layout transition to `SHADER_READ_ONLY_OPTIMAL`, or a buffer barrier for meshes) and submits it on the transfer queue with `vkQueueSubmit2`, signaling the timeline semaphore with the upload's value. The token is that value, issued at `begin_upload`; timeline values must be signaled in order, so a commit that overtakes an earlier one is held until that one arrives (kiln always commits what it began). `upload_status(token)` is Complete once `counter >= token` (a submitted copy never fails). When the transfer queue is the graphics queue, submission happens inside `upload_status` on the pump thread, so no two threads use one queue. |
| Frame ordering | `upload_status` raises `adapter_upload_watermark()` when it returns Complete; each frame submit waits on the timeline semaphore at that value, so a slot bound this pump is safe to sample this frame. |
| Objects | `GpuObject::native` is a 1-based index into the adapter's object table (image + view + memory, or buffer + memory). kiln sets `slot` (its own slot number) on what `gpu_object()` returns. Meshes have no slot. |
| Placeholders | Nothing special: kiln binds a new slot to the placeholder object of the texture's kind and shape. |
| Destroy | `destroy` frees at once. The viewer reports its frames (`renderer_wait_frame` returns the frame about to be recorded and the last one whose fence it waited), so kiln calls `destroy` only after the frames that used the object. |
| Memory | One `vkAllocateMemory` per object. Enough for an example; a real renderer sub-allocates. |
| Copy constraints | `optimalRowPitchAlign = 1`, `optimalOffsetAlign = 16`, `bufferOffsetAlign = 256`. The adapter recomputes level offsets the same way `texture_layout` in `src/runtime/loader.cpp` does, so copy regions match what kiln wrote. |
| Formats | A `static_assert` table in `vk_adapter.cpp` checks every `kiln::Format` value against `VK_FORMAT_*`. `supports_format` asks `vkGetPhysicalDeviceFormatProperties` for sampled-image or vertex-buffer support. |

### Viewer design

- **Device.** volk, Vulkan 1.4 instance, optional validation layer (`--validate`), a physical
  device with a Vulkan 1.4 driver; features: timeline semaphores, descriptor indexing (runtime
  arrays, partially bound, update-after-bind, non-uniform indexing), synchronization2, dynamic
  rendering, maintenance5 (`vkCmdBindIndexBuffer2`), buffer device address.
- **Binding model A** from spec §8: one pipeline per distinct `VertexLayout`, created on first use.
  Attribute locations are fixed by semantic (position 0, normal 1, tangent 2, uv0 3, uv1 4,
  color 5). A layout that lacks an attribute binds a small zero buffer at instance rate for that
  location, so every shader input is fed. Specialization constants tell the shader which
  attributes are real and whether the normal is octahedral.
- **Draw.** Per part and LOD 0: push constants carry the model matrix (part translation and
  rotation, parent chain resolved), `posScale` / `posBias`, the base-color bindless slot from
  `gpu_object(ctx, textureHandle)`, flags, and the draw's entry in the material table.
  `vkCmdDrawIndexed` per submesh with the spec's index offset formula. A frame uniform buffer holds
  the view-projection matrix and the material table: the PBR factors of `MaterialSlot` for up to
  127 materials, every loaded model's materials in order, the last entry glTF's defaults
  (`vkx::FrameUniforms::materials`). The viewer multiplies the base color by its factor and adds
  the emissive factor of a material without an emissive map (it binds no emissive map).
- **Streaming.** The meshes named on the command line form a boot group, waited on with `wait()`
  before the first frame. Their textures are requested afterwards and stream in under a per-pump
  `uploadBytes` budget (`--budget-mib`), so the first frames show placeholders. The viewer logs
  per-frame CPU time and warns on frames over twice the running average.
- **Offscreen.** `--offscreen` renders into an image instead of a swapchain, and `--dump out.png`
  reads back the last frame and writes a PNG (the encoder is the test helper `tests/png_writer.h`).
  Offscreen frames are not paced: they follow each other as fast as the GPU allows, and a pump
  with nothing to do rests 1 ms. So a frame count says nothing about time, and one of three
  triggers picks the last frame:

  | Trigger | Option | Use |
  |---|---|---|
  | The scene settles (default) | none; `--timeout <s>` caps the wait (60 s, then exit code 1) | the result of loading: every mesh and texture Ready or Failed, independent of machine speed |
  | A time | `--at <ms>`: the first frame at or after `<ms>` since the first request | what a user sees mid-stream (placeholders, partial loads); depends on disk and CPU |
  | A count | `--frames N` | exactly N frames, for the per-frame CPU statistics |

  Log lines carry milliseconds since start, and event lines also their frame number. This mode
  never initializes GLFW and runs on a headless machine or a software Vulkan driver.
- **Sky.** `--sky <name>` requests a cube texture (`RequestOptions::textureShape = Cube`), for
  example a vertical strip `sky_cube.png` (`texture-shapes.md`), and draws it behind the scene:
  a full-screen triangle whose fragment shader turns the camera basis into a ray per pixel. The
  cube placeholder shows until the cube arrives.
- **Tonemapping.** The target is an sRGB image, so the shaders output linear color and values
  above 1 would clip. `--tonemap auto|none|aces` picks the display curve: `aces` is Narkowicz's
  ACES fit, `none` only clamps, and `auto` (default) uses `aces` once the `--sky` texture has
  loaded in a float (HDR) format, so LDR scenes look exactly as before. `--exposure <ev>` scales
  every color by 2^ev first. Both are in the frame uniforms (`FrameUniforms::tonemap`). This is a
  display aid for inspecting assets: no auto-exposure, no bloom, no HDR swapchain.
- **Placement and camera.** Each boot model is scaled to bounding radius 1 and placed in a row 2.5
  units apart; `--no-fit` keeps native sizes. The camera orbits the union of the placed bounds and
  frames every model; mouse drag and wheel in the window, fixed in offscreen mode.

### Shaders

`shaders/mesh.vert`, `shaders/mesh.frag`, `shaders/sky.vert` and `shaders/sky.frag` (GLSL 460,
Vulkan) with their `.spv` committed next to them and embedded into the executable as `uint32_t` arrays at configure time. The `viewer-shaders`
target rebuilds the `.spv` when `glslang` or `glslc` is on `PATH`; it is never part of `ALL`, and
the build needs no shader compiler.

### Dependencies

Example-only, pinned in `examples/viewer/CMakeLists.txt` and listed in `third_party/README.md`:
Vulkan-Headers (`vulkan-sdk-1.4.357.0`), volk (1.4.364), GLFW 3.5.1.

## Rationale

- Concurrent sharing over ownership transfers: two barriers per upload on two queues is the part
  of a Vulkan uploader that is easy to get wrong and adds nothing to what the example shows.
- A timeline semaphore per transfer submit, polled by value, matches `kSelfSubmitting` exactly and
  gives the viewer one wait per frame.
- Embedding SPIR-V removes a runtime file lookup and a build-time compiler requirement at once.
- Offscreen mode lets the viewer run on a machine without a display, the shape a CI run would take.

## Alternatives considered

| Alternative | Why not now |
|---|---|
| Vertex pulling (spec §8 model B) | One pipeline for all layouts, but the shader has to decode every quantized format; model A keeps the shader trivial and maps `VertexLayout` directly to vertex input state. |
| A thin abstraction layer over Vulkan | Hides the code the example exists to show (open-questions A8, resolved: raw Vulkan). |
| Vulkan SDK via `find_package` | Needs an SDK on every machine and runner; headers plus volk need nothing. |
| Running the viewer in CI under lavapipe | Owner chose compile-only for now; the offscreen mode keeps the door open. |

## Consequences / what this constrains later

- Hot reload (M5) needs nothing new at the adapter: `bind` on the same slot and `destroy` of the
  old object after its frames cover it.
- Progressive mips (v0.8) arrive as further `bind` calls on a slot; the adapter would then keep
  per-level views.
- `vk_adapter.cpp` is the reference for the external project's adapter; keep it readable over
  clever.

## Open points for the owner

- Whether the viewer should also demonstrate the per-frame-lookup binding model (a `--no-bindless`
  switch) or bindless only.
