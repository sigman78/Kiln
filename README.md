# Kiln

Kiln is a graphics-API-agnostic asset cook/load library for C++23. It sits between **source
assets** (`.glb`, `.png`, `.ktx2`) and **any renderer**: it cooks sources into GPU-ready,
API-agnostic runtime formats (the `.mesh` format and KTX2), loads them asynchronously and directly
into memory the renderer provides, and optionally hot-reloads them on source change. The library
never calls a graphics API itself — all GPU interaction goes through a small adapter interface that
the renderer implements.

**Status:** pre-alpha, M0 in progress, API unstable.

```
source (glb, png, ktx2) ──► cook (in-process or CLI) ──► cooked store (content-hashed)
                                     │                            │
                                     └─(cache-less: in memory)────┤
                                                                  ▼
renderer (Vulkan / sokol / bgfx / custom) ◄── adapter ◄── async runtime loader (+ hot reload)
```

## Build

**Requirements:**

- CMake ≥ 3.25
- A C++23 compiler: MSVC 2022 17.10+ (or 2026), clang-cl / clang 17+, or gcc 13+. The baseline is deliberately
  recent; when a compiler lags, the answer is to update the compiler, not to add workarounds.
- clang-format (any recent version) for contributors: `.clang-format` is the style, and the pre-commit
  hook in `.githooks/` rejects unformatted staged files. Enable it once per clone:
  `git config core.hooksPath .githooks`.

**Commands:**

```sh
cmake --preset win-msvc-debug
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

Other presets (`win-clangcl-*`, `linux-clang-*`, `linux-gcc-*`, `mac-appleclang-debug`) follow the
same pattern — see `CMakePresets.json`.

**Shipping presets** (read-only: Release, `KILN_BUILD_COOK=OFF`, `KILN_HOT_RELOAD=OFF`,
`KILN_BUILD_TOOLS=OFF`, tests on): `win-msvc-shipping`, `win-clangcl-shipping`,
`linux-clang-shipping`, `linux-gcc-shipping`. See
[`docs/design/shipping-split.md`](docs/design/shipping-split.md).

### CMake targets

| Target | Contents | Ships in product |
|---|---|---|
| `kiln_core` | Core vocabulary types, allocators, logging | yes |
| `kiln_runtime` | Async runtime loading, `.mesh` + KTX2 readers, IO | yes |
| `kiln_cook` | glTF / PNG import, encoders, settings resolution, validation, store writer | no |

### CMake options

| Option | Default |
|---|---|
| `KILN_BUILD_COOK` | `ON` |
| `KILN_BUILD_TOOLS` | `ON` |
| `KILN_HOT_RELOAD` | `ON` |
| `KILN_BUILD_TESTS` | `ON` when top-level |
| `KILN_BUILD_EXAMPLES` | `OFF` |
| `KILN_BUILD_VIEWER` | `OFF` |
| `KILN_WARNINGS_AS_ERRORS` | `ON` when top-level |

## Consuming kiln

A product that ships pre-cooked assets links `kiln::core` + `kiln::runtime` only:

```cmake
find_package(kiln CONFIG REQUIRED)
target_link_libraries(app PRIVATE kiln::runtime)
```

A dev build or tool that also needs to cook assets asks for the `cook` component, which fails
clearly if the kiln install was built with `KILN_BUILD_COOK=OFF` (any shipping preset):

```cmake
find_package(kiln CONFIG REQUIRED COMPONENTS cook)
target_link_libraries(app PRIVATE kiln::runtime kiln::cook)
```

See [`docs/design/shipping-split.md`](docs/design/shipping-split.md) for the full read-only
shipping contract.

### Runtime in ten lines

```cpp
kiln::Adapter adapter{};
kiln::NullAdapter* na = *kiln::null_adapter_create({}, &adapter); // or a real renderer adapter

kiln::Context* ctx = *kiln::create({.adapter = &adapter, .storeDir = "cooked/"});

kiln::MeshHandle ship = kiln::request_mesh(ctx, "meshes/ship_hauler_a");

kiln::pump(ctx);                                  // call once per frame
if (kiln::has_meta(ctx, ship)) {
    kiln::mesh::MeshView const* v = kiln::mesh_view(ctx, ship); // parts, lods, mounts, bounds
}
if (kiln::is_ready(ctx, ship)) { /* draw; kiln::gpu(ctx, ship) is the payload GpuObject */ }
for (kiln::Event const& e : kiln::events(ctx)) { /* MetaReady / Ready / Changed / Failed */ }

kiln::release(ctx, ship);
kiln::destroy(ctx);
kiln::null_adapter_destroy(na);
```

Load groups add a `wait()` for a small critical set (fonts, loading-screen art) instead of polling
frame by frame — only safe on a `kSelfSubmitting` adapter, which the null adapter is:

```cpp
kiln::Group boot     = kiln::group(ctx);
kiln::TextureHandle font = kiln::request_texture(ctx, "ui/font", {.group = boot});
kiln::GroupStatus st = kiln::wait(ctx, boot, {.timeoutMs = 5000});
```

## Tools

Both tools are dev/CI only except `kiln-info`, which is read-only and ships with the runtime side.

```
kiln-cook <input>... [-o <store>] [--root <dir>] [--check] [--hashed] [--map <file>]
          [--target <name>] [--profile default|precise] [--no-tangents] [--no-optimize]
          [--no-mips] [--no-lods] [--threads <n>] [--quiet] [--verbose]

  <input>       .glb / .gltf / .png / .ktx2 files, or directories (recursed)
  -o <store>    store directory (default: ./cooked). Files are <store>/<assetPath>.<ext>,
                the Named layout the v0.5 runtime store expects (kiln/assets.h StoreLayout)
  --root <dir>  source root for asset paths (default: the input directory, or the file's
                directory for single files). Asset path = relative path, forward slashes,
                extension stripped.
  --check       validate only: cook in memory, report diagnostics, write nothing
  --hashed      write content-hash file names instead of <store>/<assetPath>.<ext>
                (kept for the index-based hashed layout arriving in v0.6)
  --map <file>  append "<assetPath>	<file name>	<key hex>" lines for every output
  --threads <n> cooking threads including the main one: 0 (default) = one per core,
                1 = single-threaded. Cooked bytes are identical for every value.

Exit codes: 0 all inputs cooked, 1 usage, 2 IO failure, 3 one or more cook errors.
```

```
kiln-info <file> [--blobs] [--check] [--quiet]

  --blobs   print the full BLOB table (default: summary only)
  --check   .mesh: decode the payload, verify checksums and index values
            .ktx2: verify every level is present with the expected size
  --quiet   errors only (the exit code still reports the result)

Exit codes: 0 ok, 1 usage, 2 file could not be read, 3 open/validation failed, 4 --check failed.
```

## Examples

`examples/headless` drives the runtime without a GPU: it creates a null adapter and a context, requests
meshes and textures in one load group, pumps once per simulated frame, and logs every request, event,
metadata query and diagnostic. `--slow <ms per MiB>` and `--latency <ms>` add artificial IO and cook
delays so large files visibly take several frames; `--source <dir>` turns on cook-on-miss when the
build has `kiln_cook`.

```
kiln-headless --store tests/golden --slow 200 --latency 5 mesh/Box.mesh ktx2/color_srgb.ktx2
kiln-headless --store /tmp/store --source tests/corpus/gltf/khronos --latency 300 Box.mesh
```

Every preset builds it (`KILN_BUILD_EXAMPLES=ON`), and CTest runs it against the golden files.

`examples/viewer` is M4: the example Vulkan 1.4 adapter (`vk_adapter.cpp`: dedicated transfer queue,
timeline-semaphore tokens, staging ring with back-pressure, bindless slots with placeholder-then-publish,
deferred destroy) and `kiln-viewer`, which draws cooked meshes through it. Boot meshes are waited on as a
group; their textures stream in under a per-frame budget, so the first frames show placeholders. Built
with `KILN_BUILD_VIEWER=ON` (every preset; headers, volk and GLFW are fetched, no SDK needed); needs a
Vulkan 1.4 driver to run. `docs/design/viewer.md` has the design.

```
cmake --build --preset win-msvc-debug --target viewer-assets     # five CC0 Khronos models, ~40 MiB, not committed
kiln-viewer --source examples/assets/khronos --store build/demo-store Lantern.mesh WaterBottle.mesh Avocado.mesh
kiln-viewer --offscreen --frames 60 --dump frame.png --store tests/golden mesh/BoxTextured.mesh
kiln-vk-smoke --store tests/golden --staging-kib 4 mesh/Box.mesh ktx2/normal.ktx2   # adapter only, no window
```

## Docs

- [`docs/HANDOFF.md`](docs/HANDOFF.md) — project hand-off and engineering principles
- [`docs/mesh-format-spec.md`](docs/mesh-format-spec.md) — cooked `.mesh` runtime format spec
- [`docs/design/`](docs/design/) — design notes
- [`docs/open-questions.md`](docs/open-questions.md) — open questions for the project owner

## License

MIT — see [`LICENSE`](LICENSE).
