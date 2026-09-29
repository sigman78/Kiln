# Integration examples

**Status:** Decided (owner, 2026-09-28): as proposed, all open points as proposed. All five steps
(`gl`, `gl-bindless`, `sokol`, `vk-basic`, `nga`) are implemented, and so is `Adapter::flush`; `nga`
is built but not yet run on a supported GPU.
**Decides:** Which small renderers show newcomers how to plug kiln in, what each one maps kiln's
adapter onto, how their third-party code is fetched, and how the work feeds the API review.

## Goals

- Show a newcomer the whole path in the API they already use: request a model, see
  placeholders, see the real textures arrive, draw.
- Cover about 80% of what hosts do, not every feature: one model with several texture kinds, one
  cube texture, the swap from placeholder to real texture, hot reload.
- Test the design. Each API stresses a different assumption of the adapter interface. What does
  not fit goes into `../api-friction.md`, which is the input for the API review.

## The baseline every example shares

| Item | Choice | Covers |
|---|---|---|
| Model | `WaterBottle.glb` (CC0, fetched by `viewer-assets`) | base color (sRGB), ORM, normal, emissive; one material |
| Sky | `examples/assets/skies/hdr_cube.hdr` (committed) | `TextureShape::Cube`, `R16G16B16A16_SFLOAT` |
| Vertex data | `VertexProfile::Float` (owner decision, 2026-09-28) | no decoding in any shader: float3 position and normal, float4 tangent, float2 UV |
| Cooking | cook-on-miss from `--source`, as the viewer does | the provider path |
| Controls | orbit camera, hot reload | `Changed` events, `bind` with the new object |

`VertexProfile::Float` is implemented (`mesh-format-spec.md` §6). The quantized `default` profile
stays the profile for shipped content; the existing Vulkan viewer keeps showing it.

Shared code lives in `examples/common/`: the GLFW window, the orbit camera, the scene (requests,
a draw list built from `mesh_view`), and a CPU-side upload queue for the adapters that defer GPU
work (below). The per-API code is what each example exists to show, and what the friction log
measures.

## The examples

| Example | API | Binding model | Self-submitting | Runs on the dev machine |
|---|---|---|---|---|
| `vk-bindless` | Vulkan 1.4 | bindless slots | yes | yes (the existing `kiln-viewer`) |
| `vk-basic` | Vulkan 1.4 | one descriptor set per material | yes | yes |
| `gl` | OpenGL 4.6 core, DSA | texture units bound per draw | no: `flush` | yes |
| `gl-bindless` | OpenGL 4.6 + `ARB_bindless_texture` | resident 64-bit handles | no: `flush` | yes (not on macOS) |
| `sokol` | sokol_gfx (D3D11 on Windows, Metal on macOS, GL on Linux) | bindings per draw | no: `flush` | yes |
| `nga` | NoGraphicsAPI (Vulkan with descriptor heaps, or Metal 4) | descriptor heap index | yes | build only: needs RTX 30+ / RDNA 3+ |

### How the adapter maps

| Adapter part | `vk-basic` | `gl` | `gl-bindless` | `sokol` | `nga` |
|---|---|---|---|---|---|
| `bind` (`bindlessSlots`) | null | null | writes the resident handle into kiln's slot of the handle table | null | points kiln's slot in a CPU table at the object's descriptor |
| `begin_upload` memory | staging ring | persistently mapped PBO | same as `gl` | CPU memory per upload | CPU-visible GPU heap (`gpu_heap`, ReBAR) |
| `commit_upload` | submits a copy | queues | queues | queues | queues |
| GPU work | transfer queue | `flush`: `glTextureSubImage*`, then a fence | same as `gl` | `flush`: `sg_make_image` + view / `sg_make_buffer`; complete at once | `flush`: texture creation and copies on queue 0, a timeline semaphore; meshes: none (written in place) |
| `GpuObject` | `VkImage` / `VkBuffer` | the adapter's own table index | table index in `native`, kiln's slot in `slot` | the adapter's own table index | table index in `native` (a mesh's GPU address from `nga_mesh`), kiln's slot in `slot` |
| After `Ready` / `Changed` | rebuild that material's set | nothing; bound per draw | nothing: `bind` wrote the handle | nothing; bound per draw | nothing: the next root data reads the slot's new descriptor |
| `destroy` | immediate; the host reports frames | immediate; no frames | make non-resident, then delete; the host reports frames (fences) | immediate; no frames | immediate; the host reports frames (timeline values) |

`nga` is the one example where kiln writes mesh payloads straight into GPU memory, with no staging
copy. The adapter interface allows it (`UploadTarget::dst` is "staging / ReBAR / scratch"), but
nothing exercises it yet.

## Proposed API change: `Adapter::flush`

GL and sokol calls must run on the thread that owns the context. `begin_upload` and
`commit_upload` may run on kiln worker threads, so these adapters can only write memory and queue
there. Today nothing tells them when to do the real work, and without `kSelfSubmitting`, `wait()`
cannot be used.

Proposal:

- `void (*flush)(void* user)`, optional. kiln calls it on the pump thread at the start of every
  `pump()`, before it polls `upload_status`, and in every loop of `wait()`.
- The host must call `pump()` on the thread that owns the graphics context. Hosts already do this.
- `wait()` works when the adapter sets `kSelfSubmitting` or `flush`.
- A Vulkan adapter that records uploads into the frame's command buffer can use `flush` the same
  way.

The `gl` example was built first to confirm the shape before the API changed. It first called its
own flush from the host loop; now kiln calls `Adapter::flush`.

What each example showed is recorded in `../api-friction.md` ("Integration notes"), with the
friction rows.

## Dependencies

All are fetched with `FetchContent` at a pinned commit or release; nothing is vendored. Each example
has its own CMake option, OFF by default. Each dependency gets its entry in `dependencies.md` and
`third_party/README.md` in the step that adds it.

| Example | Fetched | License | Notes |
|---|---|---|---|
| all | GLFW (already fetched for the viewer) | zlib | window and input (`sokol` uses sokol_app instead) |
| `gl`, `gl-bindless` | none | — | an example-owned loader of about 60 functions over `glfwGetProcAddress`, no glad |
| `sokol` | sokol headers (commit `2e75443d`); `sokol-shdc` (sokol-tools-bin `11d0cf67`) | zlib; MIT | the shader compiler is a downloaded binary, checked by SHA-256 |
| `vk-basic` | Vulkan-Headers, volk (already fetched) | Apache-2.0, MIT | committed SPIR-V, as the viewer does |
| `nga` | NoGraphicsAPI; a Slang compiler release | MIT; Apache-2.0 with LLVM exception | x86-64 with AVX2; its windowed examples exist on Windows and macOS only |

CMake options: `KILN_EXAMPLE_VK_BASIC`, `KILN_EXAMPLE_GL`, `KILN_EXAMPLE_GL_BINDLESS`,
`KILN_EXAMPLE_SOKOL`, `KILN_EXAMPLE_NGA`. `kiln-viewer` stays under `KILN_BUILD_VIEWER`. CI builds
all of them without running them, on Windows (and Linux where the API exists).

## Friction log

The predicted friction points are rows in `../api-friction.md` with status **Predicted**. Each
example marks the rows it hits as confirmed (Open) or refutes them (Rejected, with the reason), and
adds what nobody predicted.

## Rollout

1. *(done)* `examples/common/` and `gl`, with the host-side flush; then decide `Adapter::flush`.
2. *(done)* `gl-bindless`.
3. *(done)* `sokol`.
4. *(done)* `vk-basic`.
5. *(done: built, not yet run)* `nga` (built on CI, run by the owner on a supported GPU).
6. API review of the friction log; changes go to `CHANGELOG.md` with migration notes.

## Open points

1. **`flush`.** Name, call points, and whether it lifts the `kSelfSubmitting` requirement of
   `wait()`. Proposed: as above.
2. **sokol's window.** sokol_app runs its own main loop (callbacks), which changes how the host
   calls `pump()`. GLFW with sokol_gfx keeps the shared code. Proposed: sokol_app, because that is
   how sokol users write programs, and its callback loop is itself a friction test.
3. **The shared scene code.** A shared draw list keeps examples short but hides the part a
   newcomer copies. Proposed: share the window and camera; keep each example's request, draw and
   material code in its own file.
4. **NoGraphicsAPI toolchain.** Its README asks for the Vulkan SDK for shader validation. Proposed:
   download only Slang and build without validation, so no SDK is needed.
