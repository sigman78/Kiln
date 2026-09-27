# Kiln

Kiln is a graphics-API-agnostic asset cook/load library for C++20. It sits between **source
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
- A C++20 compiler: MSVC 2022+, clang-cl, clang 16+, or gcc 12+

**Commands:**

```sh
cmake --preset win-msvc-debug
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

Other presets (`win-clangcl-*`, `linux-clang-*`, `linux-gcc-*`, `mac-appleclang-debug`) follow the
same pattern — see `CMakePresets.json`.

### CMake targets

| Target | Contents |
|---|---|
| `kiln_core` | Core vocabulary types, allocators, logging |
| `kiln_runtime` | Async runtime loading, `.mesh` + KTX2 readers, IO |
| `kiln_cook` | glTF / PNG import, encoders, settings resolution, validation, store writer |

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

## Docs

- [`docs/HANDOFF.md`](docs/HANDOFF.md) — project hand-off and engineering principles
- [`docs/mesh-format-spec.md`](docs/mesh-format-spec.md) — cooked `.mesh` runtime format spec
- [`docs/design/`](docs/design/) — design notes
- [`docs/open-questions.md`](docs/open-questions.md) — open questions for the project owner

## License

MIT — see [`LICENSE`](LICENSE).
