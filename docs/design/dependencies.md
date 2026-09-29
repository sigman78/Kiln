# Dependencies for v0.5

**Status:** Proposed (awaiting owner sign-off). The owner chose cgltf and wuffs. Implemented in M2
(`third_party/`, pins in `third_party/README.md`) and M4 (`examples/viewer/CMakeLists.txt`).
**Decides:** Which third-party libraries kiln uses in v0.5, how each is pinned, and which target
links it.

## Decision

| Concern | Choice | License | Scope | Pinning |
|---|---|---|---|---|
| glTF parsing | **cgltf** | MIT | cook only, `KILN_MESH=ON` | vendored in `third_party/cgltf/` |
| PNG, JPEG, WebP decode | **wuffs** (WebP only with `KILN_WEBP=ON`) | Apache-2.0 | cook only | vendored single release `.c` file in `third_party/wuffs/` |
| KTX2 read | **own** minimal reader | n/a | runtime | in-tree `src/formats/` |
| KTX2 write | **own** minimal writer (raw and BC formats) | n/a | cook only | in-tree `src/formats/` |
| Mesh optimization | **meshoptimizer** | MIT | cook only, `KILN_MESH=ON` (decoder sources may later join `kiln_runtime`) | FetchContent, commit hash |
| Tangents | **MikkTSpace** (reference `mikktspace.c/.h`) | zlib | cook only, `KILN_MESH=ON` | vendored in `third_party/mikktspace/` |
| Zstd | deferred (v0.6) | BSD | runtime decoder-only build, cook encoder | FetchContent, commit hash |
| BC1/3/4/5/7 encoders | **bc7enc_rdo**: `rgbcx`, `bc7enc` (and `bc7decomp` for tests) | MIT or public domain | cook only | vendored in `third_party/bc7enc_rdo/` |
| BC6H, ASTC encoders | deferred (BC6H: v0.6; ASTC: v0.9) | to be chosen | cook only | to be chosen |
| Config parsing | deferred (v0.6), leaning TOML | to be chosen | cook only | to be chosen |
| File watching | **own** polling watcher (M5) | n/a | runtime (dev builds, `KILN_HOT_RELOAD`) | in-tree |
| Tests | **own** runner `tests/kiln_test.h` | n/a | tests | in-tree |
| Viewer GPU API | raw **Vulkan 1.4** (Vulkan-Headers) + **volk** | Apache-2.0 / MIT; MIT | example viewer only | FetchContent, commit hash (no SDK) |
| Example windows | **GLFW** 3.5.1 | zlib | windowed examples only (`examples/common`) | FetchContent, commit hash; X11 only on Linux |
| sokol example | **sokol** headers (sokol_gfx, sokol_app, sokol_glue, sokol_log) | zlib | `kiln-sokol` only | FetchContent, commit hash |
| sokol shaders | **sokol-shdc** prebuilt binary (sokol-tools-bin) | MIT | `kiln-sokol` build only | downloaded per host at a commit, checked by SHA-256 |
| NoGraphicsAPI example | **NoGraphicsAPI** (one source file), **Vulkan-Loader** (built from source) | MIT; Apache-2.0 | `kiln-nga` only | FetchContent, commit hash |
| NoGraphicsAPI shaders | **Slang** release (slangc) | Apache-2.0 with LLVM exception | `kiln-nga` build only | downloaded per host at a release, checked by SHA-256 |

`kiln_runtime` has **zero** third-party dependencies in v0.5. Everything third-party is cook-only
(through the helper target `kiln_third_party_cook`) or example-only. The `.mesh` blob decode loop
ships with codec `None` only, which needs no library.

### glTF: cgltf

- C99, one header, no exceptions, no STL. Included only by `src/cook/gltf_import.cpp`.
- cgltf's memory callbacks go to the per-cook arena.
- Every accessor and embedded image resolves through `resolve_buffer_view()`, so a later
  `EXT_meshopt_compression` decode (meshoptimizer's `meshopt_decodeVertexBuffer` /
  `meshopt_decodeIndexBuffer`) slots in there. Until then the importer rejects the extension
  (K1002).
- fastgltf is faster but pulls in simdjson and a larger C++ surface. kiln never parses glTF at
  runtime, and cook time is dominated by tangents, optimization and mips.

### PNG, JPEG, WebP: wuffs

- Memory-safe by construction, fast, one release `.c` file compiled with the needed modules only
  (`third_party/wuffs/wuffs_modules.h`), no allocation of its own (the caller provides work buffers).
- Decodes 8-bit and 16-bit PNG; 16-bit keeps 16 bits per channel (height maps, precise normal maps).
- JPEG (owner decision, 2026-09-27): core glTF allows `image/png` and `image/jpeg`, and many GLB
  files embed JPEG. The decoder is in the same release file, so JPEG adds no dependency. Baseline
  and progressive decode; arithmetic coding, 12/16-bit, lossless and hierarchical JPEG are rejected.
- WebP (owner decision, 2026-09-27): `EXT_texture_webp` and loose `.webp` sources, lossy and
  lossless, behind the CMake option `KILN_WEBP` (default OFF), which adds the `VP8` and `WEBP`
  modules. Off, WebP sources fail with K2002.
- stb_image was the fallback and is not needed.

### KTX2: own reader and writer

- A raw KTX2 file is: identifier, fixed header, level index, Data Format Descriptor, key/value
  data, then mip data. Reader and writer together are a few hundred lines.
- The runtime reader lives in `kiln_runtime` and stays dependency-free.
- libktx is large, has many build options, and brings Basis and Zstd code not needed yet. Revisit
  libktx (or a Basis transcoder alone) when Basis or Zstd supercompression lands (v0.6+).

### BCn: bc7enc_rdo

- `rgbcx` encodes BC1, BC3, BC4 and BC5; `bc7enc` encodes BC7. Both are plain C++ with no heap
  allocation and no exceptions; each fills global tables once, before the first encode.
- Chosen by a measurement of five candidates against compiled size, speed and quality
  (`bcn-encoding.md`, owner decision 2026-09-29): `rgbcx` gave the best quality at every speed for
  BC1–5, and `bc7enc` is 47 KB against 300 KB for `bc7e.ispc`, which needs the ISPC compiler.
- `RGBCX_USE_SMALLER_TABLES=1`: the smaller table, 120 KB compiled instead of 300 KB, with the
  same output quality.
- Compiled with FP contraction off (`-ffp-contract=off`; MSVC's default), because cooked textures
  are compared byte for byte on every compiler (`tests/golden/`).
- `bc7decomp` is only linked into `kiln_tests`, which decodes BC7 output to check quality.

### Zstd (deferred, v0.6+)

- One dependency, two users: KTX2 Zstd supercompression and `.mesh` payload blobs (codec `Zstd` and
  `kBlobOuterZstd`).
- `kiln_runtime` gets a **decoder-only build** (zstd's `decompress/` and `common/` sources). The
  runtime wrapper is one `.cpp` that decodes into caller memory, with the context `Allocator`
  routed through zstd's memory hooks. `kiln_cook` gets the full library.
- It is the first planned `kiln_runtime` dependency, so this note carries its reason.

### Mesh processing: meshoptimizer + MikkTSpace

- meshoptimizer: vertex cache, overdraw and vertex fetch optimization in v0.5. Later:
  simplification (LOD generation, v0.6) and vertex/index codecs.
- If a meshopt scheme is chosen for `.mesh` payloads (by measurement, v0.6-0.7), its vertex and
  index **decoder** sources may be compiled into `kiln_runtime`. The rest stays cook-only.
- MikkTSpace: the reference implementation, so tangents match what bakers (Blender, Substance,
  xNormal) assume.

### Viewer (M4): raw Vulkan 1.4 + volk + GLFW

Decided at M4 (open-questions A8, `viewer.md`): raw Vulkan, because a thin layer would hide the
code the example exists to show; volk loads entry points without linking the loader; headers come
from Vulkan-Headers through FetchContent, so no machine needs the SDK; GLFW (not SDL3) for the
window, never initialized in offscreen mode; SPIR-V committed next to the GLSL.

### sokol example: sokol + sokol-shdc

Decided with the integration examples (`integration-examples.md`, owner 2026-09-28): the example
shows sokol as sokol users write it, so sokol_app owns the loop and shaders go through sokol-shdc.
The headers are fetched; the implementation is one C file compiled without kiln's warnings.
sokol-shdc is a native tool with no source build in CMake, so the prebuilt binary for the host is
downloaded from sokol-tools-bin at a pinned commit and checked by hash (`examples/sokol/CMakeLists.txt`).
The two are pinned together: the generated shader code must match the sokol_gfx it compiles against.

### NoGraphicsAPI example: NoGraphicsAPI + Vulkan-Loader + Slang

Decided with the integration examples (owner 2026-09-28): no SDK. NoGraphicsAPI's own CMake wants the
SDK's Vulkan package and exports install targets, so kiln fetches its sources and compiles its one
file itself. It links the Vulkan loader (kiln's other examples load entry points with volk), so the
loader is built from source at the SDK tag that matches the fetched headers. Slang compiles the
shaders; its release archive for the host is downloaded and checked by hash. SPIRV-Tools, which
NoGraphicsAPI's examples use to validate shaders, is left out (decided in the note). All of this is
behind `KILN_EXAMPLE_NGA`, OFF in every preset, and the manual `extended` workflow builds it.

## Rules

- **Pin versions.** FetchContent with a commit hash, or vendored source. Never a branch or bare tag.
- **Build with our flags** where possible (`cmake/kiln_warnings.cmake`). Third-party sources may
  get warning suppressions, per target, never globally.
- **Permissive license only:** MIT, BSD, zlib, Apache-2.0, or similar. Nothing copyleft.
- **Record it** in `third_party/README.md` with version or commit, license, and using target.
- **Ask before adding.** Any dependency not in this note needs owner approval first.
- **Headers stay in `.cpp` files.** No third-party header under `include/kiln/`, and no
  third-party type in kiln's public API. One kiln `.cpp` wraps each dependency.
- **Exceptions:** if a dependency can throw, catch only in that wrapping `.cpp` and convert to
  `Status` (`error-model.md`). None of the current dependencies throw.
- **Allocation:** route a dependency's allocator callbacks to kiln's `Allocator` (`Tag::Cook`)
  where it has them: cgltf and wuffs are routed. meshoptimizer allocates temporaries through global
  `new` (`meshopt_setAllocator` is process-global, so kiln leaves it alone) and MikkTSpace uses
  `malloc`; neither can be redirected without patching. Both are cook-only, so cook tag statistics
  under-report by these temporaries.

### Runtime-side dependency rule

`kiln_runtime` accepts third-party code only as **decoders**: matching an encoder or writer in
`kiln_cook`, built from source with kiln's own flags (never a prebuilt binary), and never able to
write files or import a source format. In v0.5 there are none. The candidates are zstd
(decode-only) and meshoptimizer's decoder sources. This rule, the `include/kiln/cook/` header
boundary and the `shipping` CI job form the read-only shipping contract (`shipping-split.md`).

## Rationale

- Every v0.5 pick is small, C or C-style C++, and exception-free, which keeps the
  `KILN_NO_EXCEPTIONS` hardening step (v1.0) cheap.
- A dependency-free runtime keeps the shipping link (`kiln_core` + `kiln_runtime`) trivial.
- Deferring zstd, encoders and config parsing avoids choosing before needs are measured.
- The blob decode loop exists with codec `None`, so zstd or meshopt later is a new decoder behind
  an existing dispatch, not a loader change.

## Alternatives considered

| Concern | Alternative | Why not now |
|---|---|---|
| glTF | fastgltf + simdjson | Larger C++ surface and a second dependency; parse speed does not matter on the cook side |
| PNG | spng | Needs zlib or miniz |
| PNG | libpng | Large, setjmp/longjmp error handling |
| KTX2 | libktx for writing | Large; many options; brings Basis/Zstd code not used in v0.5 |
| Tests | doctest | Own runner already works and has no exceptions or iostreams |
| File watch | efsw, dmon | Polling is enough to shape the API; native watchers come in v0.9 |

## Consequences / what this constrains later

- The importer keeps a single buffer-view resolution point (for meshopt decode later).
- The KTX2 reader keeps the level index accessible, so later range reads can load the smallest
  mips first.
- When supercompression lands, the reader gains a supercompression dispatch; the level index
  already carries `byteLength` and `uncompressedByteLength`, so the file layout does not change.
- CI needs no Vulkan SDK: `KILN_BUILD_VIEWER` is ON in every preset, fetches headers and compiles
  the viewer, but CI never runs it. Linux runners need the X11 development packages GLFW builds
  against.
- When `.mesh` compression lands, `kiln_runtime` gains its first third-party code. Each codec
  should be behind a CMake option, so a project that ships only uncompressed payloads keeps a
  dependency-free runtime.

## Open points for the owner

- cgltf and wuffs: decided by the owner.
- Viewer stack: decided at M4.
- Confirm "own KTX2 reader and writer" for v0.5.
