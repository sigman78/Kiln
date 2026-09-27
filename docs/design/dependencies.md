# Dependencies for v0.5

**Status:** Proposed (awaiting owner sign-off). The owner chose cgltf and wuffs. Implemented in M2
(`third_party/`, pins in `third_party/README.md`) and M4 (`examples/viewer/CMakeLists.txt`).
**Decides:** Which third-party libraries kiln uses in v0.5, how each is pinned, and which target
links it.

## Decision

| Concern | Choice | License | Scope | Pinning |
|---|---|---|---|---|
| glTF parsing | **cgltf** | MIT | cook only | vendored in `third_party/cgltf/` |
| PNG decode | **wuffs** | Apache-2.0 | cook only | vendored single release `.c` file in `third_party/wuffs/` |
| KTX2 read | **own** minimal reader | n/a | runtime | in-tree `src/formats/` |
| KTX2 write | **own** minimal writer (raw formats only) | n/a | cook only | in-tree `src/formats/` |
| Mesh optimization | **meshoptimizer** | MIT | cook only (decoder sources may later join `kiln_runtime`) | FetchContent, commit hash |
| Tangents | **MikkTSpace** (reference `mikktspace.c/.h`) | zlib | cook only | vendored in `third_party/mikktspace/` |
| Zstd | deferred (v0.6) | BSD | runtime decoder-only build, cook encoder | FetchContent, commit hash |
| BCn / ASTC encoders | deferred (v0.6) | to be chosen | cook only | to be chosen |
| Config parsing | deferred (v0.6), leaning TOML | to be chosen | cook only | to be chosen |
| File watching | **own** polling watcher (M5) | n/a | runtime (dev builds, `KILN_HOT_RELOAD`) | in-tree |
| Tests | **own** runner `tests/kiln_test.h` | n/a | tests | in-tree |
| Viewer GPU API | raw **Vulkan 1.4** (Vulkan-Headers) + **volk** | Apache-2.0 / MIT; MIT | example viewer only | FetchContent, commit hash (no SDK) |
| Viewer window | **GLFW** 3.5.1 | zlib | example viewer only | FetchContent, commit hash; X11 only on Linux |

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

### PNG: wuffs

- Memory-safe by construction, fast, one release `.c` file compiled with the PNG modules only, no
  allocation of its own (the caller provides work buffers).
- Decodes 8-bit and 16-bit PNG; 16-bit keeps 16 bits per channel (height maps, precise normal maps).
- stb_image was the fallback and is not needed.

### KTX2: own reader and writer

- A raw KTX2 file is: identifier, fixed header, level index, Data Format Descriptor, key/value
  data, then mip data. Reader and writer together are a few hundred lines.
- The runtime reader lives in `kiln_runtime` and stays dependency-free.
- libktx is large, has many build options, and brings Basis and Zstd code not needed yet. Revisit
  libktx (or a Basis transcoder alone) when Basis or Zstd supercompression lands (v0.6+).

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
