# Kiln

Kiln is a graphics-API-agnostic asset cook and load library for C++23. It cooks source assets
(`.glb`, `.png`, `.ktx2`) into GPU-ready runtime formats (`.mesh` and KTX2), loads them
asynchronously straight into memory the renderer provides, and will hot-reload them on source
change. The library never calls a graphics API: every GPU interaction goes through a small adapter
struct that the renderer fills in.

**Status:** pre-alpha, M4 done (example Vulkan adapter and viewer), M5 hot reload next. API unstable;
breaks are listed in `CHANGELOG.md`.

```
source (glb, png, ktx2) --> cook (kiln_cook, in-process or kiln-cook CLI) --> store (<store>/<asset>.mesh|.ktx2)
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
| `kiln_runtime` | `.mesh` and KTX2 readers, IO backend and thread pool, the async runtime, null adapter | yes |
| `kiln_cook` | glTF and PNG import, image kernels, cooker, settings, store writer | no |

| Option | Default | Meaning |
|---|---|---|
| `KILN_BUILD_COOK` | ON | build `kiln_cook`; OFF is the shipping configuration |
| `KILN_BUILD_TOOLS` | ON | `kiln-cook` (needs cook) and `kiln-info` |
| `KILN_BUILD_TESTS` | ON when top-level | own runner, golden files, tool smoke tests |
| `KILN_BUILD_EXAMPLES` | OFF (presets: ON) | `kiln-headless` |
| `KILN_BUILD_VIEWER` | OFF (presets: ON) | `kiln-viewer`, `kiln-vk-smoke`; fetches Vulkan-Headers, volk and GLFW, no SDK needed |
| `KILN_HOT_RELOAD` | ON | hot-reload support in `kiln_runtime` (M5) |
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
if (kiln::is_ready(ctx, ship)) { kiln::GpuObject g = kiln::gpu(ctx, ship); /* draw */ }
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

- `kiln-cook <input>... -o <store>` cooks files or directories into a store. `--check` validates
  only, `--threads <n>` sets the pool size and the per-cook thread budget, `--verbose` prints per-stage
  timings.
- `kiln-info <file>` dumps a `.mesh` or `.ktx2`; `--check` decodes and verifies. Read-only, ships
  with the runtime side.
- `kiln-headless` (`examples/headless`) drives the runtime with the null adapter and logs every event;
  `--slow` and `--latency` simulate slow IO and cooking.
- `kiln-viewer` (`examples/viewer`) draws cooked meshes through the example Vulkan 1.4 adapter:
  boot meshes are waited on as a group, textures stream in under a per-frame budget, so the first
  frames show placeholders. `--offscreen --frames N --dump out.png` renders without a window.
  `kiln-vk-smoke` exercises the adapter alone. Both need a Vulkan 1.4 driver to run.

```sh
cmake --build --preset win-msvc-debug --target viewer-demo   # fetches five CC0 Khronos models, cooks, opens the viewer
```

`examples/README.md` has the recipes.

## Docs

- `docs/design/` design notes, one per decision area; `docs/design/README.md` is the index
- `docs/mesh-format-spec.md` the cooked `.mesh` format
- `docs/diagnostics.md` diagnostic codes (K1xxx cook, K2xxx image, K3xxx settings, K4xxx formats, K5xxx runtime)
- `docs/open-questions.md` decisions awaiting the owner, and the reserved-space register
- `docs/HANDOFF.md` the owner's project brief and engineering principles

## License

MIT, see `LICENSE`. Third-party components and their licenses: `third_party/README.md`.
