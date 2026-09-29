# Integration examples

**Status:** Proposed (2026-09-28). The owner asked for the examples, the separate bindless GL
example and the float vertex baseline; the rest awaits sign-off.
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
| Controls | orbit camera, `--watch` for hot reload | `publish` with a new version |

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
| `acquire` | null | null | handle of the placeholder, made resident | `sg_alloc_image` | a heap index bound to the placeholder |
| `begin_upload` memory | staging ring | persistently mapped PBO | same as `gl` | CPU memory | CPU-visible GPU heap (`gpu_heap`, ReBAR) |
| `commit_upload` | submits a copy | queues | queues | queues | records and submits a copy (textures); nothing (meshes) |
| GPU work | transfer queue | `flush`: `glTextureSubImage*`, then a fence | same as `gl` | `flush`: `sg_init_image` / `sg_init_buffer` | copy queue, timeline semaphore |
| `GpuObject` | `VkImage` / `VkBuffer` | the adapter's own table index | 64-bit handle in `native` | sokol id in `native` | GPU address in `native`, heap index in `slot` |
| After `publish` | rebuild that material's set | nothing; bound per draw | write the new handle | nothing; bound per draw | nothing |
| `destroy_deferred` | by frames in flight | immediate | make non-resident, then delete | immediate | by timeline value |

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
  `pump()`, before it polls `is_upload_complete`, and in every loop of `wait()`.
- The host must call `pump()` on the thread that owns the graphics context. Hosts already do this.
- `wait()` works when the adapter sets `kSelfSubmitting` or `flush`.
- A Vulkan adapter that records uploads into the frame's command buffer can use `flush` the same
  way.

The `gl` example is built first to confirm the shape before the API changes. Until then it calls
its own flush from the host loop.

## Dependencies

All are fetched with `FetchContent` at a pinned commit or release; nothing is vendored. Each example
has its own CMake option, OFF by default. Each dependency gets its entry in `dependencies.md` and
`third_party/README.md` in the step that adds it.

| Example | Fetched | License | Notes |
|---|---|---|---|
| all | GLFW (already fetched for the viewer) | zlib | window and input; not used by `sokol` if it uses sokol_app |
| `gl`, `gl-bindless` | none | — | an example-owned loader of about 60 functions over `glfwGetProcAddress`, no glad |
| `sokol` | sokol headers; `sokol-shdc` binary (sokol-tools-bin) | zlib; MIT | the shader compiler is a downloaded binary |
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

1. `examples/common/` and `gl`, with the host-side flush; then decide `Adapter::flush`.
2. `gl-bindless`.
3. `sokol`.
4. `vk-basic`.
5. `nga` (built on CI, run by the owner on a supported GPU).
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
