# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Pre-1.0: API breaks are allowed but every break is recorded here with migration notes.

## [Unreleased]

### Added
- The integration examples (`kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`, `kiln-vk-basic`, `kiln-nga`)
  take no arguments: they show the reference scene (WaterBottle under the HDR test sky) from
  `examples/assets`, cook into the build tree's `example-store` and hot reload. The only option is
  `--dump <file.png>`; `+` / `-` change the exposure. Migration: drop `--store`, `--source`, `--root`,
  `--sky`, `--exposure`, `--width`, `--height`, `--watch`, `--offscreen`, `--timeout` and the model
  name. The `viewer-assets` target (the demo models) now exists whenever a windowed example is built.
- `kiln-nga` (`examples/nga`, CMake option `KILN_EXAMPLE_NGA`, OFF everywhere, its own CI job): the
  scene through NoGraphicsAPI. Mesh payloads are written by kiln straight into CPU-visible GPU
  memory and read through GPU pointers; textures are descriptor heap indices behind a CPU slot table.
  Fetches NoGraphicsAPI, builds the Vulkan loader from source (no SDK) and downloads Slang. Built,
  not yet run: it needs `VK_EXT_descriptor_heap`.
- `kiln-vk-basic` (`examples/vk-basic`, CMake option `KILN_EXAMPLE_VK_BASIC`, ON in the presets): the
  scene on Vulkan 1.4 without bindless, a descriptor set per material and frame slot rewritten on
  kiln's events. The viewer's Vulkan code is now the library `kiln_example_vk`: its adapter takes
  `AdapterDesc::bindless` (false: no `acquire`; `adapter_texture()` gives the image view), and its
  renderer takes a host's SPIR-V and material set layout (`RendererDesc`).
- `EventKind` documents which events may change what `gpu()` returns (Ready, Changed, Failed).
- `kiln-sokol` (`examples/sokol`, CMake option `KILN_EXAMPLE_SOKOL`, ON in the presets): the scene
  through sokol_gfx with sokol_app owning the main loop (D3D11 on Windows, GL on Linux, Metal on
  macOS). Its adapter writes uploads into per-upload CPU memory and makes the images and buffers in
  `flush`. New example-only dependencies: the sokol headers and the sokol-shdc binary, both at
  pinned commits (the binary checked by SHA-256). The command line of the integration examples
  moved to `examples/common` (`ex::Options`, `ex::parse_options`).
- `kiln-gl-bindless` (`examples/gl/main_bindless.cpp`, CMake option `KILN_EXAMPLE_GL_BINDLESS`, ON in
  the presets): `kiln-gl` with `ARB_bindless_texture`. The GL adapter gains a bindless mode
  (`GlAdapterDesc::bindless`): `acquire` gives each texture a slot in a persistently mapped table
  of resident handles, `publish` writes the real handle, and retired handles and slots wait for a
  fence. The two GL examples share `gl_util` and render the same pixels.
- `Adapter::flush` (optional): kiln calls it at the start of every `pump()` (so in every `wait()`
  loop) and in `create()`'s placeholder spin, on that thread. An adapter for an API that must be
  called on one thread (GL, sokol) does its GPU work there. `wait()` and the placeholder wait now
  accept `kSelfSubmitting` or `flush`. The field sits between `destroy_deferred` and `caps`: code
  that fills `Adapter` with designated initializers keeps compiling; code that relies on aggregate
  order without designators must add it.
- `texture_level_layout(TextureDesc, CopyConstraints, offsets, pitches)` in `kiln/adapter.h`: the
  level offsets and row pitches kiln writes into a texture upload, so an adapter no longer copies
  the rule. The example Vulkan and GL adapters use it.
- `kiln-gl` (`examples/gl`, CMake option `KILN_EXAMPLE_GL`, ON in the presets): the first
  integration example (`docs/design/integration-examples.md`). One model and a cube sky through
  OpenGL 4.6 core with textures bound per draw; its adapter writes uploads into a persistently mapped
  staging ring on kiln's workers and does the GL work in a host-side flush on the GL thread. Own GL
  loader, no new dependency. `examples/common` holds what the windowed examples share (logging,
  orbit camera, PNG dumps) and now fetches GLFW for all of them.
- `VertexProfile::Float` (`profile = "float"` in a sidecar, `kiln-cook --profile float`): every
  vertex attribute as a plain float vector (position, normal `R32G32B32_SFLOAT`, tangent
  `R32G32B32A32_SFLOAT`, UVs `R32G32_SFLOAT`; color stays `R8G8B8A8_UNORM`), 48 B/vertex for the
  usual attributes. No shader decoding; for simple renderers and the integration examples. The
  default `TargetProfile::maxVertexProfile` is now `Float`, which changes the target hash: every
  cook key and the `cookHash` stored in cooked files change (payloads are unchanged), so existing
  stores re-cook once.
- Texture-only builds (`docs/design/texture-only.md`): CMake option `KILN_MESH` (default ON). With
  OFF, `kiln_cook` builds without the glTF importer and mesh cooker, and cgltf, MikkTSpace and
  meshoptimizer are neither fetched nor built. `cook_mesh` stays declared and fails with
  `Unsupported` and the new K1021; `kiln-cook` still cooks the other inputs. The `viewer-demo`
  target and the glb-based CTests need `KILN_MESH=ON`.
- `AdapterCaps::kMeshes`: the adapter accepts mesh payloads. Without it kiln never acquires or
  uploads a mesh, and a mesh request fails with K5004. **Break:** an adapter that loads meshes must
  now set the bit. Migration: add `kMeshes` to `Adapter::caps` (the null adapter and the example
  Vulkan adapter do).
- `kiln-viewer --tonemap auto|none|aces` and `--exposure <ev>`: a display curve for HDR content.
  `auto` (default) applies ACES only once an HDR (float) `--sky` has loaded, so LDR scenes render
  unchanged. `FrameUniforms` gains `tonemap` (now 112 bytes); `mesh.frag` and `sky.frag` apply it.
- HDR textures (`docs/design/hdr-textures.md`): Radiance `.hdr` sources through kiln's own decoder
  (`decode_hdr`, `is_hdr`; flat and run-length scanlines, standard orientation), usage `Hdr`
  cooking to `R16G16B16A16_SFLOAT` with f32 mips and `float_to_half` / `half_to_float`
  (round to nearest even, saturating). `Image::bitsPerChannel` may be 32 (f32). A `.hdr` source
  implies usage `Hdr` and a linear color space; the provider and `kiln-cook` accept `.hdr`,
  including cube and array strips. New warning K2011 for an integer source with usage `Hdr`. No
  new dependency.
- `kiln-viewer --offscreen` stops when the scene settles (every mesh and texture Ready or Failed)
  by default, capped by the new `--timeout <s>` (exit code 1 when it expires); `--at <ms>` stops at
  the first frame at or after that time; `--frames N` still renders exactly N frames. The default
  used to be 60 frames, which could dump placeholders. Log lines carry milliseconds since start.
- `kiln-headless` drops `--frame <ms>`: it pumps until settled and rests 1 ms only after an idle
  pump. Progress lines lose their frame number; the summary reports milliseconds and pumps.
  Migration: remove `--frame`.
- `kiln-viewer --sky <name>` draws a cube texture behind the scene (a sky pass with
  `shaders/sky.{vert,frag}`). The example Vulkan adapter now takes cube and array textures: one
  bindless binding per `TextureShape` (2D, cube, array) sharing a slot index, cube-compatible
  images, and `kCubeTextures | kArrayTextures` (`docs/design/texture-shapes.md`, step 5).
- Cube and array textures from one strip image (`docs/design/texture-shapes.md`, steps 3 and 4):
  `TextureCookSettings::shape` (`CookShape`) and `slices` cut a vertical strip into faces or layers,
  each with its own mip chain and size cap; a bad strip is the new K2010. Name rules stack
  (`rock_array_n.png` is an Array of normal maps) and gain a shape: `NameRule::shape`,
  `hints_from_name`, default rules `_cube` and `_array`. Sidecar keys `shape` and `slices`. A KTX2
  source whose shape differs from a non-Auto `shape` is K2004. Texture settings schema 2: the
  settings hash changes, so stores re-cook textures.
- KTX2 cube and array textures in the cook (`docs/design/texture-shapes.md`, step 2):
  `ktx2::WriteDesc` gains `layers`, `faces` and `isArray`; a cube or array KTX2 source passes
  through with its shape (a volume or cube array is still K2004); new
  `TargetProfile::maxArrayLayers` (default 2048), where more layers is K2004 rather than a clamp.
  CTest runs `ktx validate` on cube and array output and round-trips the corpus cube and array.
- Cook: per-asset `.kiln` sidecar files (`wall_n.png.kiln`, `chair.glb.kiln`) set cook settings for
  one standalone texture or mesh. The syntax is a strict TOML subset parsed by kiln itself (no
  dependency); keys are the settings field names. `apply_sidecar()` in the new
  `kiln/cook/sidecar.h`; the provider and `kiln-cook` read sidecars, and the source poller re-cooks
  when one is added, edited or removed. New diagnostics K3005 (syntax) and K3006 (key or value).
  Syntax and keys: `docs/design/settings.md`, "Sidecar files".
- Cook: name rules give a standalone texture source its usage from its file-stem suffix
  (`wall_n.png` is a linear normal map, `crate_orm.png` is ORM). `NameRule`, `kDefaultNameRules`
  and `usage_from_name` in `kiln/cook/settings.h`; `ProviderDesc::nameRules` (default: the
  built-in table, copied at install) and `kiln-cook` apply them when the usage is `Auto`. Before,
  every standalone texture was cooked as sRGB color. Textures referenced from a glTF still take
  their usage from the material slot.
- JPEG texture sources, embedded in glTF (`image/jpeg`) or loose `.jpg`/`.jpeg` files, through the
  JPEG decoder of the already vendored wuffs (no new dependency). WebP sources (`.webp` files and
  `EXT_texture_webp`) behind the new CMake option `KILN_WEBP`, default OFF. `kiln/cook/image.h` adds
  `decode_jpeg`, `decode_webp`, `decode_image` (picks the decoder by signature), `is_jpeg`,
  `is_webp`, `is_lossy_image` and `webp_decode_enabled`. New warning K2009 when a Normal or Height
  texture comes from a lossy source. The cook provider and `kiln-cook` accept the new extensions.
- `viewer-assets` fetches a sixth CC0 model, DiffuseTransmissionTeacup, whose GLB embeds JPEG base-color
  textures next to PNG maps; `viewer-demo` shows it.
- M5 hot reload (`docs/design/hot-reload.md`). Runtime: `request_reload()` reloads a store-backed
  asset while the old version stays servable; success bumps the content version, publishes the new
  object, sends the old one to `destroy_deferred` and emits `Changed` (or `Ready` from `Failed`, which
  also moves the asset from `failed` to `ready` in its load group); failure keeps the old version with
  K5010; memory-registered assets give K5012. `ContextDesc::hotReload.watchStore` starts a store
  poller (`KILN_HOT_RELOAD` builds; otherwise K5011). `IoBackend::stat` is a new optional entry, the
  compat backend implements it and opens files with `FILE_SHARE_DELETE` on Windows. Cook side:
  `ProviderDesc::watchSources` / `pollMs` start a source poller that re-cooks changed sources (a glb
  with its embedded textures, a PNG or KTX2 on its own) into the store; `cook::store_write` gains
  `bool overwrite = false`. `kiln-viewer --watch` and `kiln-headless --watch` turn both on.
- `ContextDesc::workerPriority` passes a `ThreadPriority` to the built-in pool, so a host that lets kiln
  create the pool can still keep loading below its own threads.
- M4: `kiln-viewer` draws cooked meshes through the example adapter with one pipeline per vertex
  layout (zero-buffer inputs and specialization constants for attributes a layout lacks), dynamic
  rendering and synchronization2, two frames in flight, and frame submits that wait on the adapter's
  upload watermark. Boot meshes are waited on as a group; their base-color textures stream in under
  `--budget-mib`, so placeholders show first. Orbit camera in the window; `--offscreen --frames N
  --dump out.png` renders headless; `--source` enables cook-on-miss. Boot models are scaled to a bounding radius of 1 and placed 2.5 units apart
  (`--no-fit` keeps native sizes). Demo models: `viewer-assets`
  fetches five CC0 Khronos glTF models at a pinned commit with SHA-256 checks into
  `examples/assets/khronos/` (git-ignored; `examples/assets/README.md`).
- M4: `examples/viewer`. Vulkan 1.4 device bring-up (volk, optional validation layer),
  the example `kiln::Adapter` (dedicated transfer queue, timeline-semaphore tokens, host-coherent
  staging ring with `Busy` back-pressure, bindless slots with placeholder-then-publish, deferred
  destroy by frames in flight, per-format `static_assert`s against `VkFormat`), and `kiln-vk-smoke`,
  which loads cooked assets through the adapter with no window. Dependencies, examples only:
  Vulkan-Headers, volk and GLFW through pinned FetchContent (`docs/design/viewer.md`,
  `third_party/README.md`). `KILN_BUILD_VIEWER=ON` in every preset; CI compiles the viewer targets.
- `tools/cli.h`: a small option-table argument parser shared by `kiln-cook`, `kiln-info` and
  `kiln-headless` (not installed, no allocation, no exceptions). Options are declared once with
  designated initializers; `--help` prints usage generated from the same table; values accept
  `--opt value` and `--opt=value`; numbers are range-checked and choices validated. Every existing
  option and exit code is unchanged.
- `ThreadPoolDesc::priority` (`ThreadPriority::Normal` / `Low` / `High`): the host's hint for where
  kiln's workers sit relative to its own threads. The built-in pool applies it per worker on
  Windows (`SetThreadPriority`) and Linux (per-thread nice); other platforms ignore it for now.
- Cook thread budget: `CookEnv::maxThreads` (default 3, caller included; 1 = inline, 0 = no cap)
  limits how many pool threads one cook may occupy; the image functions take `JobBudget { jobs,
  maxThreads }` in place of the bare `JobSystem const*`. `kiln-cook --threads <n>` sets both the
  pool size and the budget. The renormalize kernel's 8-bit first step comes from a 256-entry table,
  and the normal-map downsample renormalizes on the same SSE2 path. Output is unchanged.
- Cook kernels, steps 4 and 5: `cook_mesh` builds and quantizes each (part, LOD) as a task over
  `CookEnv::jobs` and merges the results sequentially in traversal order, so cooked bytes and the
  diagnostic order are unchanged with or without a pool (tested with 1 and 8 threads); the mesh
  `CookStats` fields are summed task time and can exceed `totalUs` with a pool. The renormalize
  kernel (normal maps: level 0 prepare and `renormalize()`) has an SSE2 path on x64 that is
  bit-identical to the scalar path and about 2.8x faster at 4096x4096. `KILN_ARCH_X64` /
  `KILN_ARCH_ARM64` join `kiln/core.h`.
- Cook kernels, step 3: `kiln::cook::CookEnv { alloc, diag, jobs }`; when `jobs` is set, the image
  passes (prepare, flip green, renormalize, and the level 0 to 1 downsample) split by row bands over
  the job system through an internal helping `parallel_for` that never deadlocks inside a worker.
  Output is byte-identical with or without a pool. `kiln::jobs(ctx)` exposes the context's pool and the
  cook-on-miss provider passes it; `kiln-cook --threads <n>` and `kiln_bench_image --threads <n>`
  select the pool size. Image functions gain a trailing `JobSystem const* jobs = nullptr`.
- Cook kernels, step 1 and 2 of docs/design/cook-kernels.md. `kiln::cook::CookStats` on
  `CookedTexture` / `CookedMesh` gives per-stage microseconds (decode / prepare / mips / write for
  textures; import / build / tangents / optimize / pack / write for meshes) and `kiln-cook --verbose`
  prints them. `kiln_bench_image` (tests/, manual, not a CTest) benchmarks the image kernels and
  `cook_texture` end to end. `src/cook/kernels.h` holds the image kernels as format-specialized
  templates with a kernel table; `linear16_to_srgb8` uses a 64 KiB table instead of a binary search;
  `kiln::cook::prepare_image` does convert + flip green + renormalize in one pass. Cooked bytes are
  unchanged (`kCookerVersion` not bumped); `downsample_2x` on 4K RGBA8 is about 5x (linear) and 9x
  (sRGB) faster. `KILN_HOT` and `KILN_RESTRICT` join `kiln/core.h`.
- `examples/headless`: a GPU-free walkthrough of the runtime API (null adapter, requests, a load
  group, `pump()` per frame, events, metadata queries, cook-on-miss when built with `kiln_cook`)
  that logs every step. `--slow` / `--latency` add artificial IO and cook delays so large files
  visibly take time. Built by every preset (`KILN_BUILD_EXAMPLES=ON`) and run as a CTest smoke test.
- `kiln::cook_provider(ctx)` returns the installed provider so a host can wrap it.
- M3: runtime (kiln/assets.h): context, handles with generations, states Unloaded/Pending/MetaReady/Ready/Failed, refcounted requests with two priorities, pump() with upload budget and Busy back-pressure, events, load groups with progress/wait (panics on misuse), kind-specific placeholders (host-overridable, dev magenta for Failed), publish/acquire adapter hooks and gpu() lookup, in-memory registration, cook-on-miss provider hook; compat IO backend (positional reads) and built-in thread pool (kiln/io.h); null adapter (kiln/null_adapter.h); kiln_cook install_provider (kiln/cook/provider.h). Zero steady-state allocations verified by tag stats. Diagnostics K5001-K5009.
- M3: cook-on-miss provider (`kiln/cook/provider.h`, `kiln::cook::install_provider`/`uninstall_provider`):
  installs a `CookProvider` on a runtime `Context` that cooks missing mesh/texture assets from the
  context's source roots (`.glb`/`.gltf`, `.png`/`.ktx2`), writing `StoreLayout::Named` files in Disk mode
  or staying cache-less in Memory mode; a texture with no source of its own is produced by cooking its
  owning mesh. Dev builds only (`kiln_cook`); `kiln_runtime` never references it.
- M2: cooker. glTF/GLB -> `.mesh` (`kiln/cook/cook.h`: cgltf import, naming conventions `mount_`/`_lodN`/`col_`/`_`, hierarchy parts with baked scale, MikkTSpace tangents, meshoptimizer vertex cache/overdraw/fetch, quantized default profile with float fallback, authored LOD pass-through, materials with `.NNN` stripping, texture bindings + UV sets, mount extras); PNG -> KTX2 (`kiln/cook/image.h`: wuffs decode incl. 16-bit, integer-exact sRGB mips, normal renormalization, size caps) and KTX2 pass-through; v0.5 settings subset with slot inference and field-wise hashing (`kiln/cook/settings.h`); content-hashed store with atomic writes; `kiln-cook` CLI (`--check`, `--named`, `--map`); glTF corpus (`tests/corpus/gltf/`, generated edge cases + Khronos samples) and golden files (`tests/golden/`); diagnostics K1001-K1018, K2001-K2008, K3001-K3004. Dependencies: cgltf 1.15, MikkTSpace, wuffs 0.4 (vendored), meshoptimizer 1.3 (FetchContent, pinned), cook side only.
- KTX2 real-world corpus (`tests/corpus/ktx2/`): curated Khronos KTX-Software test images (Apache-2.0) plus `ktx create`-generated variants, a manifest-driven reader test, writer round-trips with DFD byte comparison, and `ktx validate` CTest checks on every file kiln writes (CI installs KTX-Software 4.4.2). Format table gains RGB8, ETC2/EAC and ASTC LDR rows.
- M1.5: read-only shipping contract: cook headers moved to include/kiln/cook/, install components (runtime, cook), *-shipping presets, shipping CI job with a find_package consumer, reader-only test split.
- M1: `.mesh` reader (`kiln/mesh.h`: spec v0.3 records, `MeshView`, always-on header/BLOB checks, full validation, payload decode loop with codec None, checksum and index checks) and deterministic writer (`kiln/cook/mesh_writer.h`, kiln_cook); KTX2 raw reader (`kiln/ktx2.h`) and writer (`kiln/cook/ktx2_writer.h`); `Format` enum + constexpr table (`kiln/formats.h`, values == VkFormat); `kiln-info` tool; `docs/diagnostics.md` (K4000-K4199).
- Project display name is Kiln; GitHub repository renamed to sigman78/Kiln (namespace, CMake package and targets stay lowercase `kiln`).

### Fixed
- Example Vulkan adapter: a small upload no longer waits for a larger one that began earlier. The
  timeline value is given at `commit_upload` instead of `begin_upload`, so submission follows
  commit order; the upload token is now the object handle (index and generation). A small sky
  texture now becomes Ready about 5 ms before a 22 MB texture requested with it, instead of on the
  same frame. `kiln-vk-smoke` checks the order before loading and exits 1 if it breaks.
- `PumpStats::uploadsStarted` and `uploadBytes` counted a Busy retry again each time it was
  re-dispatched, so a run with back-pressure could report many times the bytes actually uploaded.
  Retries still consume the per-pump budget and show up in `busyRetries`.
- KTX2 writer: the alpha sample of sRGB formats carried the EXPONENT qualifier (0x20) instead of LINEAR (0x10); found by `ktx validate`.
- Cook: external glTF image URIs were not percent-decoded, so a texture such as `my%20wood.png`
  could not be read (K1005 / "cannot read external texture"). `TextureRef::uri` is now the decoded
  relative path, as the `UriResolver` already received for buffers.

### Changed
- **Breaking (runtime, adapter):** textures carry a shape: 2D, cube or array
  (`docs/design/texture-shapes.md`, step 1).
  - New `TextureShape` and `texture_shape_name()` in `kiln/adapter.h`. The adapter's `TextureDesc`
    gains `shape`; a cube still arrives as 6 layers, in the order +X, -X, +Y, -Y, +Z, -Z.
  - `Adapter::acquire` gains a `TextureShape shape` parameter.
  - New caps `kCubeTextures` and `kArrayTextures`. kiln uploads placeholders of a shape only for an
    adapter that declares it, and a request for an undeclared shape fails with K5004.
  - `RequestOptions::textureShape` (default `Tex2D`, first request wins) picks the placeholder. A
    cooked texture of another shape fails with the new K5017.
  - Placeholders exist per shape: `placeholder_asset_id(kind, shape)`, and
    `failed_placeholder_id(shape)` replaces `kFailedPlaceholderId` (now id 13 for 2D). A context
    creates up to 15 placeholder objects instead of 5.
  - Migration: add the `shape` parameter to `acquire`, and set the caps bits if the adapter can
    bind cube or array views.
- **Breaking (cook):** settings resolve in a fixed layer order (`docs/design/settings.md`,
  "Resolution layers"): built-in defaults, host settings, sidecar, inference, `CookPolicy`, then
  validation. Host settings are defaults, so a sidecar beats them; the new `CookPolicy` (a texture
  and a mesh function plus `void* user`) has the last word and may refuse an asset (new K3007).
  - `ProviderDesc::texture` / `mesh` are now `textureDefaults` / `meshDefaults`; new
    `ProviderDesc::policy`.
  - New `resolve_texture_layers` / `resolve_mesh_layers` and `ResolveDesc`, `CookAssetInfo`, used
    by the provider and `kiln-cook`. The color space and the Normal-only flags are derived after
    the policy.
  - The `kiln-cook` CLI is now `cook_cli_main(argc, argv, policy)` in `kiln/cook/cli.h`; a
    project's own cook tool calls it with its policy.
  - Migration: rename the two `ProviderDesc` fields. A rule that must hold even against a sidecar
    moves from the defaults into a `CookPolicy`.
- **Breaking (runtime, cook, store, tools):** asset names are `root:path/file.ext#sub`, the source
  path with its extension (`docs/design/asset-model-next.md`, Part 2).
  - The runtime no longer normalizes names: no extension stripping, no `./`, `\` or `//` clean-up.
    `request_*`/`register_*` check a name with the new `check_asset_name()` and reject a bad one
    at the call (null handle, new K5013). `asset_id()` hashes the name as given (0 if invalid).
    New helpers in `kiln/assets.h`: `check_root_name`, `split_asset_name`, `resolve_asset_name`
    (a relative URI in a source to a name in the same root), `store_file_path`.
  - `ContextDesc::sourceRoots` is now `ContextDesc::roots` (`Root{name, dir}`; an empty name is
    the default root), and `source_roots()` is `roots()`. `create()` rejects a bad or repeated
    root name (K5013).
  - Store files are `<store>/<name>.mesh|.ktx2` (`props/chair.glb.mesh`,
    `props/chair.glb#wood.ktx2`); a named root `m:` is the top-level directory `@m/`, so a
    default-root path may not start with `@` (K5013).
  - The provider finds `<root dir>/<path>` with no extension search. The extension gives the
    kind; a mismatch is K5014, an unknown root K5015, a name that differs in case from the file
    on disk K5016 (checked on Windows, `cook::source_case_matches`).
  - Mesh cook: `MeshSource::assetPath` must be a valid name. An external URI that is absolute or
    leaves the root is the new error K1020. K1019 now checks `<mesh>#<image>` with the name rules,
    which also reject `< > " | ? *`.
  - `kiln-cook`: names keep the extension; `--root [<name>=]<dir>` is repeatable and sets the
    default root when it has no name. `kiln-viewer` takes mesh names (`Lantern.glb`,
    `lib:props/chair.glb`) instead of `.mesh` store paths. `kiln-viewer` and `kiln-headless` take
    `--root` too and keep `--source <dir>` for the default root. `kiln-headless` arguments are a
    name plus `.mesh`/`.ktx2` (`Lantern.glb.mesh`). `tools/cli.h` options gain an `each` callback for repeatable options.
  - Migration: delete the store and re-cook. Request `chair.glb` instead of `chair` and
    `chair.glb#wood` instead of `chair#wood`. Replace `sourceRoots = {dir}` with
    `roots = {Root{{}, dir}}`. Resolve external texture URIs with `resolve_asset_name()`. A
    store cooked elsewhere keeps working under names without a source extension.
- **Breaking (format, cook, store):** a mesh cook no longer cooks the images its glTF references by
  URI (`docs/design/asset-model-next.md`, "one source file per cooked asset").
  - `.mesh` 0.4: `TextureBinding` flag `kTextureExternal` marks such a binding; `pathStr` is the
    URI (percent-decoded, relative to the source) and `textureId` is 0. The host maps it to a
    texture (the viewer resolves it against the mesh's directory).
  - Embedded images are named `<mesh>#<image name>` (was `<mesh>/<sanitized stem>`); an unnamed
    image is `image<N>`. A duplicate or reserved-character name is the new error K1019.
  - `TextureRef` lists embedded images only and loses `uri`. The provider splits a texture
    request at `#` instead of guessing the owning mesh from the last `/`.
  - `kCookerVersion` 3.
  - Migration: delete the store and re-cook; 0.3 files fail with `VersionMismatch`. Request
    externally referenced textures by their own names; the name rules pick their usage.
- Tests: `kiln_tests` compiles in the corpus, golden and scratch (`<build>/tests/samples`)
  directories, so running it by hand runs every test; `--corpus`, `--golden` and `--samples` still
  override. A missing directory now stops the run (exit 2) instead of silently skipping tests.
- **Breaking (cook):** `cook_mesh` and `cook_texture` take `CookEnv const& env = {}` in place of the
  trailing `Allocator const* alloc, DiagSink const* diag`. Migration:
  `cook_texture(src, s, target, alloc, &sink)` becomes `cook_texture(src, s, target, {.alloc = alloc, .diag = &sink})`;
  `cook_mesh(src, s, target, default_allocator())` becomes `cook_mesh(src, s, target)`.
- `.mesh` writer: `WriteOptions::splitBytes` and `kiln::mesh::split_unit()` are removed (pre-1.0 break).
  The cooker never set them; the writer emits one blob per vertex stream per LOD and one per index
  range. Blob splitting returns with the v0.6 codec work, and spec §5.9 now records the split rules as
  deferred. Migration: drop the field; files written without splitting are unchanged.
- `kiln-cook` writes the `StoreLayout::Named` layout (`<store>/<assetPath>.<ext>`) by default, matching
  what the runtime's v0.5 store expects (`kiln/assets.h`); the old content-hashed file names move behind
  `--hashed`, kept for the index-based hashed layout landing in v0.6 (pre-1.0 break: scripts that relied
  on the previous default now need `--hashed`, or to switch to the named paths).
- Cooked KTX2 files carry `kiln.sourceHash` and `kiln.cookHash` key/value entries (the invalidation identity for the named store layout); the KTX2 writer accepts extra sorted key/value entries. Cooker version 2; goldens regenerated.
- C++23 idioms: `if consteval` in the hash readers, `std::unreachable()` instead of `KILN_UNREACHABLE`, `std::to_underlying` and `std::is_scoped_enum` for enum hashing/printing, `KILN_ASSUME`, and monadic `Result<T>` (`and_then`, `transform`, `or_else`).
- Language baseline raised from C++20 to C++23 (owner decision 2026-09-27): CMake `cxx_std_23`; minimum compilers MSVC 2022 17.10+, clang 17+, gcc 13+.
- Header paths: kiln/mesh_writer.h -> kiln/cook/mesh_writer.h, kiln/ktx2_writer.h -> kiln/cook/ktx2_writer.h (pre-1.0 break; update includes).

- M0: repository skeleton, CMake targets and presets, CI, core vocabulary types (allocator,
  Result/Status, panic, log, Span/StrView, FixedArray/Vec/HashMap, hash/fourcc), minimal test
  runner, design notes.
- Design notes aligned to HANDOFF v2 (blob table, MetaReady, kind placeholders, load groups,
  publish/acquire/caps, gpu()).
- mesh-format-spec v0.3: KMSH magic, kiln::mesh namespace, exact-minor version rule while 0.x, BLOB rules resolved (filter/codec combinations, split alignment, table order, raw fast path conditions, lodRank, size limits).
