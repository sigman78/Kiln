# Dependencies for v0.5

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** Which third-party libraries kiln uses in v0.5, how each is pinned, and which target links it.

## Decision

Nothing below is added to the build until the owner signs off on this note. Each dependency then
lands in the milestone that first needs it (M1 or M2), with a row in `third_party/README.md`.

| Concern | Choice | License | Scope | Pinning | Lands in |
|---|---|---|---|---|---|
| glTF parsing | **cgltf** | MIT | cook only | vendored `cgltf.h` in `third_party/cgltf/` | M2 |
| PNG decode | **wuffs** (fallback: **stb_image**) | Apache-2.0 (stb: MIT / public domain) | cook only | vendored single release `.c` file in `third_party/wuffs/` | M2 |
| KTX2 read | **own** minimal reader | n/a | runtime | in-tree `src/formats/` | M1 |
| KTX2 write | **own** minimal writer (raw formats only) | n/a | cook only | in-tree `src/formats/` | M1 |
| Mesh optimization | **meshoptimizer** | MIT | cook only | FetchContent, commit hash | M2 |
| Tangents | **MikkTSpace** (reference `mikktspace.c/.h`) | zlib | cook only | vendored in `third_party/mikktspace/` | M2 |
| Zstd | deferred (v0.6) | BSD | runtime decoder-only build, cook encoder | FetchContent, commit hash | v0.6 |
| BCn / ASTC encoders | deferred (v0.6) | to be chosen | cook only | to be chosen | v0.6 |
| Config parsing | deferred (v0.6), lean TOML, decision open | to be chosen | cook only | to be chosen | v0.6 |
| File watching | **own** polling watcher | n/a | runtime (dev builds, `KILN_HOT_RELOAD`) | in-tree `src/io/` | M5 |
| Tests | **own** runner `tests/kiln_test.h` | n/a | tests | in-tree, already in place | M0 |
| Viewer GPU API | raw **Vulkan 1.4** (SDK headers) + **volk** | Apache-2.0 / MIT (headers); MIT (volk) | example viewer only | Vulkan SDK via `find_package(Vulkan)`; volk via FetchContent, commit hash | M4 |

`kiln_runtime` has **zero** third-party dependencies in v0.5. Everything third-party is either
cook-only or example-only.

### glTF: cgltf

- C99, one header, no exceptions, no STL. It compiles under `-fno-exceptions -fno-rtti` today.
- Trivial to isolate: included only by `src/cook/gltf_import.cpp`.
- `EXT_meshopt_compression` decode can be inserted at buffer-view resolution. We resolve every
  accessor through one function of ours (`resolve_buffer_view()`), so a later decode step using
  meshoptimizer's `meshopt_decodeVertexBuffer` / `meshopt_decodeIndexBuffer` slots in there.
  Until then the importer rejects the extension with a K1xxx diagnostic.
- Allocation: cgltf takes `cgltf_memory_options` (alloc/free callbacks). We route them to the
  per-cook arena.
- **fastgltf considered.** It is faster, but pulls in simdjson and a larger C++ surface (templates,
  `std::variant`, optional exceptions). kiln never parses glTF at runtime, and cook time is
  dominated by tangents, optimization and mips, so parse speed is not critical.

### PNG: wuffs, with stb_image as fallback

- wuffs: memory-safe by construction (proved bounds checks), fast, single `.c` release file, no
  allocation of its own (caller provides work buffers). Apache-2.0.
- **16-bit PNG support is required** (height maps, high-precision normal maps). wuffs decodes to
  16-bit per channel via its pixel-format selection; this must be verified in M2 with a test file.
- Fallback: stb_image (`stbi_load_16_from_memory` covers 16-bit). Switch if the wuffs API costs
  more than a day of integration friction. The decision is recorded in `third_party/README.md`.
- spng considered: good, but depends on zlib or miniz, which adds a second dependency.

### KTX2: own reader and writer

- A raw (non-supercompressed) KTX2 file is: identifier, fixed header, level index, Data Format
  Descriptor (DFD), key/value data, then mip data. Reader and writer together are a few hundred
  lines.
- The runtime reader must live in `kiln_runtime` and should stay dependency-free.
- libktx is large, has many build options, and brings Basis and Zstd code we do not need yet.
- **Deferred:** revisit libktx (or a Basis transcoder alone) when Basis or Zstd supercompression
  lands (v0.6+). Zstd supercompression alone can be done with zstd plus our own reader.

### Mesh processing: meshoptimizer + MikkTSpace

- meshoptimizer (MIT): vertex cache, overdraw and vertex fetch optimization in v0.5. Later:
  simplification (LOD generation, v0.6) and vertex/index codecs, including the
  `EXT_meshopt_compression` decoder.
- MikkTSpace (zlib): the reference implementation, so tangents match what bakers (Blender,
  Substance, xNormal) assume.

### Viewer (M4): raw Vulkan 1.4 + volk

- The viewer's purpose is to prove that an adapter is "a few hundred lines". A thin
  NoGraphicsAPI-style layer would hide exactly the code we want to show (staging ring, timeline
  semaphore, deferred destroy).
- volk loads Vulkan entry points without linking the loader statically and removes dispatch
  overhead. It is example-only; the library never sees it.
- Alternative: build the viewer on a thin NoGraphicsAPI-style layer (fewer lines, more modern
  bindless style). Proposed to decide at M4, see `docs/open-questions.md` A8.
- Window and input: to be decided at M4 (GLFW or SDL3, both zlib). Not part of this sign-off.

## Rules

Repeated from HANDOFF §8, plus kiln-specific header rules.

- **Pin versions.** FetchContent with a commit hash, or vendored source. Never a branch or bare tag.
- **Build with our flags** where possible (`cmake/kiln_warnings.cmake`). Third-party sources may
  get warning suppressions, applied per target, never globally.
- **Permissive license only:** MIT, BSD, zlib, Apache-2.0, or similar. Nothing copyleft.
- **Record it** in `third_party/README.md` with version or commit, license, and using target.
- **Ask before adding.** Any dependency not in this note needs owner approval first (HANDOFF §12).
- **Headers stay in `.cpp` files.** No third-party header is included from any file under
  `include/kiln/`. Third-party types never appear in kiln's public API. Each dependency is wrapped
  by one kiln `.cpp` file that exposes kiln types (`Result<T>`, `Span`, kiln structs).
- **Exceptions:** if a dependency can throw, catch only in that wrapping `.cpp` and convert to
  `Status` (see `error-model.md`). None of the proposed v0.5 dependencies throw.
- **Allocation:** where a dependency accepts allocator callbacks (cgltf, wuffs work buffers,
  meshoptimizer `meshopt_setAllocator`), route them to kiln's `Allocator` with `Tag::Cook`.
- **Runtime stays lean.** A dependency in `kiln_runtime` needs an explicit reason in this note.
  In v0.5 there are none.

## Rationale

- Every v0.5 pick is small, C or C-style C++, and exception-free, which keeps the
  `KILN_NO_EXCEPTIONS` hardening step (v1.0) cheap.
- Keeping the runtime dependency-free makes the shipping link (`kiln_core` + `kiln_runtime`)
  trivial for the external host project.
- Deferring zstd, encoders and config parsing avoids choosing before we have measured needs.

## Alternatives considered

| Concern | Alternative | Why not now |
|---|---|---|
| glTF | fastgltf + simdjson | Larger C++ surface and a second dependency; parse speed does not matter on the cook side |
| PNG | spng | Needs zlib or miniz |
| PNG | libpng | Large, setjmp/longjmp error handling |
| KTX2 | libktx for writing | Large; many options; brings Basis/Zstd code we do not use in v0.5 |
| Tests | doctest | Own runner already works and has no exceptions or iostreams |
| File watch | efsw, dmon | Polling is enough to shape the API; native watchers come in v0.9 |

## Consequences / what this constrains later

- The importer structure must have a single buffer-view resolution point (for meshopt decode later).
- The own KTX2 reader must keep the level index accessible so post-v0.5 range reads can load the
  smallest mips first.
- When supercompression lands, the reader gains a supercompression dispatch; the level index
  already carries `byteLength` and `uncompressedByteLength`, so the file layout does not change.
- CI needs no Vulkan SDK unless `KILN_BUILD_VIEWER=ON`.

## Open points for the owner

- Confirm cgltf over fastgltf.
- Confirm wuffs as first choice for PNG (and accept stb_image as a no-further-approval fallback).
- Confirm "own KTX2 reader and writer" for v0.5.
- Confirm the viewer uses raw Vulkan 1.4 + volk (or defer to M4).
- Window library for the viewer (GLFW vs SDL3) can wait until M4.
