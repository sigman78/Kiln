# Kiln

<img src="docs/logo.svg" alt="Kiln logo: a kiln with a glowing opening and three stacked cubes" width="200" height="200" align="right">

Kiln is a graphics-API-agnostic asset cook and load library for C++23. It cooks source assets
(`.glb`, `.gltf`, `.png`, `.jpg`, `.hdr`, `.ktx2`) into GPU-ready runtime formats (`.mesh`, and
KTX2 with BC1-BC7 textures and Zstd supercompression), loads them asynchronously straight into
memory the renderer provides, and hot-reloads them when a source changes. The library never calls
a graphics API: every GPU interaction goes through a small adapter struct that the renderer fills
in.

**Status:** v0.6 (milestones M0-M5, BC and Zstd textures, target profiles, PBR factors, the store
manifest). The API is not stable yet; breaks
are listed in `CHANGELOG.md` with migration notes. Next: see `docs/ROADMAP.md`.

```
source (glb, png, jpg, hdr, ktx2) --> cook (kiln_cook, in-process or kiln-cook CLI) --> store (manifest + artifacts)
                                                                                     |
renderer (Vulkan, sokol, bgfx, ...) <-- adapter <-- kiln_runtime: async load, pump() per frame, placeholders
```

## Build

Requirements: CMake 3.25+, a C++23 compiler (MSVC 2022 17.10+, clang-cl / clang 17+, gcc 13+), and
clang-format for contributors. The baseline is deliberately recent: when a compiler lags, update the
compiler rather than add workarounds. Enable the format hook once per clone with
`git config core.hooksPath .githooks`.

```sh
cmake --preset win-msvc-debug && cmake --build --preset win-msvc-debug && ctest --preset win-msvc-debug
```

Presets: `win-msvc-*`, `win-clangcl-*`, `linux-clang-*`, `linux-gcc-*` (`debug`, `release`) and the
read-only `*-shipping` presets (Release, cook, tools and hot reload off; see
`docs/design/shipping-split.md`). Every preset builds the tests, the examples and the viewer.

| Target | Contents | Ships |
|---|---|---|
| `kiln_core` | vocabulary types, allocators, containers, hashing, logging, `Result` | yes |
| `kiln_runtime` | `.mesh` and KTX2 readers (with zstd's decoder), IO backend and thread pool, the async runtime, null adapter | yes |
| `kiln_cook` | glTF, PNG, JPEG and Radiance HDR import (WebP optional), image kernels, BC encoders, Zstd, cooker, settings, store writer | no |

| Option | Default | Meaning |
|---|---|---|
| `KILN_BUILD_COOK` | ON | build `kiln_cook`; OFF is the shipping configuration |
| `KILN_BUILD_TOOLS` | ON | `kiln-cook` (needs cook) and `kiln-info` |
| `KILN_BUILD_TESTS` | ON when top-level | own runner, golden files, tool smoke tests |
| `KILN_BUILD_EXAMPLES` | OFF (presets: ON) | `kiln-headless` |
| `KILN_BUILD_VIEWER` | OFF (presets: ON) | `kiln-viewer`, `kiln-vk-smoke`; fetches Vulkan-Headers, volk and GLFW, no SDK needed |
| `KILN_EXAMPLE_GL`, `KILN_EXAMPLE_GL_BINDLESS`, `KILN_EXAMPLE_SOKOL`, `KILN_EXAMPLE_VK_BASIC` | OFF (presets: ON) | the integration examples `kiln-gl` (and `kiln-gl-array`), `kiln-gl-bindless`, `kiln-sokol`, `kiln-vk-basic` |
| `KILN_EXAMPLE_NGA` | OFF | `kiln-nga` (NoGraphicsAPI; fetches its sources, the Vulkan loader and Slang) |
| `KILN_HOT_RELOAD` | ON | hot-reload support in `kiln_runtime` (M5) |
| `KILN_WEBP` | OFF | WebP texture sources in `kiln_cook` (`.webp`, `EXT_texture_webp`) |
| `KILN_MESH` | ON | mesh cooking in `kiln_cook`; OFF is a texture-only cook without cgltf, MikkTSpace and meshoptimizer (`docs/design/texture-only.md`) |
| `KILN_WARNINGS_AS_ERRORS` | ON when top-level | |
| `KILN_INSTALL` | ON when top-level | install rules and the `kiln` CMake package |

## Consuming kiln

A product that ships pre-cooked assets links the runtime only. A tool that also cooks asks for the
`cook` component, which fails clearly against an install built without it:

```cmake
find_package(kiln CONFIG REQUIRED)                    # kiln::runtime
find_package(kiln CONFIG REQUIRED COMPONENTS cook)    # + kiln::cook
target_link_libraries(app PRIVATE kiln::runtime)
```

## Runtime in ten lines

```cpp
kiln::Adapter adapter{};
kiln::NullAdapter* na = *kiln::null_adapter_create({}, &adapter); // or the renderer's adapter
kiln::Context* ctx    = *kiln::create({.adapter = &adapter, .storeDir = "cooked/"});

kiln::Group boot      = kiln::group(ctx);
kiln::MeshHandle ship = kiln::request_mesh(ctx, "meshes/ship", {.group = boot});
kiln::GroupStatus st  = kiln::wait(ctx, boot, {.timeoutMs = 5000}); // self-submitting adapters only

kiln::pump(ctx, {.uploadBytes = 8u << 20});                          // once per frame
if (kiln::has_meta(ctx, ship)) { kiln::mesh::MeshView const* v = kiln::mesh_view(ctx, ship); }
if (kiln::is_ready(ctx, ship)) { kiln::GpuObject g = kiln::gpu_object(ctx, ship); /* draw */ }
for (kiln::Event const& e : kiln::events(ctx)) { /* MetaReady, Ready, Changed, Failed */ }

kiln::release(ctx, ship);
kiln::destroy(ctx);
kiln::null_adapter_destroy(na);
```

Textures serve a placeholder until they arrive, and a magenta checker in dev builds when they fail.
Everything runs on the thread that calls `pump()`; workers only read, decode and write into the
memory the adapter handed out. `docs/design/handles-and-states.md` and `docs/design/adapter.md`
have the contracts.

## Tools and examples

All programs print their options with `--help`.

- `kiln-cook <input>... -o <store>` cooks files or directories into a store: artifacts named by
  their build key and the store's manifest (docs/design/store-manifest.md). It cooks only the
  sources whose inputs changed; `--verify` compares their content instead of size and time. The
  store records its roots, so `kiln-cook -o <store>` alone scans them again. `--gc` deletes the
  artifacts nothing references; `--export <dir>` writes a runtime-only copy for shipping.
  `--watch` keeps cooking what changes; an app that watches the store (no cook provider) reloads
  it. `--check` validates
  only, `--target compat|desktop|uncompressed` picks the target profile (the block formats its
  GPUs sample), `--quality` the BC encoder effort,
  `--zstd <level>` the texture supercompression (0 = off), `--threads <n>` sets the pool size and
  the per-cook thread budget, `--verbose` prints per-stage timings.
- `kiln-info <file>` dumps a `.mesh`, a `.ktx2` or a store's `manifest.dir` (it knows a file by its
  first bytes, so an artifact needs no extension); `--check` decodes and verifies (for a manifest,
  every artifact of every profile). Read-only, ships with the runtime side.
- `kiln-headless` (`examples/headless`) drives the runtime with the null adapter and logs every event;
  `--slow` and `--latency` simulate slow IO and cooking; `--watch` keeps it running to log hot reloads.
- `kiln-viewer` (`examples/viewer`) draws cooked meshes through the example Vulkan 1.4 adapter:
  boot meshes are waited on as a group, textures stream in under a per-frame budget, so the first
  frames show placeholders. `--offscreen --frames N --dump out.png` renders without a window.
  `--watch` hot-reloads changed store files and, with `--source`, re-cooks changed sources.
  `kiln-vk-smoke` exercises the adapter alone. Both need a Vulkan 1.4 driver to run.

- `kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`, `kiln-vk-basic` and `kiln-nga` are small
  integration examples, one per graphics API: each draws the same reference scene (WaterBottle under
  an HDR test sky) through its own adapter, cooking on first run.

```sh
cmake --build --preset win-msvc-debug --target viewer-demo   # fetches six CC0 Khronos models, cooks, opens the viewer
```

`examples/README.md` has the recipes.

## Docs

- `docs/design/` design notes, one per decision area; `docs/design/README.md` is the index
- `docs/mesh-format-spec.md` the cooked `.mesh` format
- `docs/diagnostics.md` diagnostic codes (K1xxx cook, K2xxx image, K3xxx settings, K4xxx formats, K5xxx runtime)
- `docs/open-questions.md` decisions awaiting the owner, and the reserved-space register
- `docs/ROADMAP.md` scope, released versions and the plan

## License

MIT, see `LICENSE`. Third-party components and their licenses: `third_party/README.md`.
