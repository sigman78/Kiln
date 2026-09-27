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

## Tools

- `kiln-cook <input>... -o <store>`: cook `.glb`/`.gltf`/`.png`/`.ktx2` sources (files or directories) into the
  content-hashed store; `--check` validates only, `--named` writes human-readable names, `--map` records
  asset path -> file. Dev/CI only (needs `kiln_cook`).
- `kiln-info <file> [--blobs] [--check]`: dump a `.mesh` or `.ktx2`; read-only, ships with the runtime side.

## Docs

- [`docs/HANDOFF.md`](docs/HANDOFF.md) — project hand-off and engineering principles
- [`docs/mesh-format-spec.md`](docs/mesh-format-spec.md) — cooked `.mesh` runtime format spec
- [`docs/design/`](docs/design/) — design notes
- [`docs/open-questions.md`](docs/open-questions.md) — open questions for the project owner

## License

MIT — see [`LICENSE`](LICENSE).
