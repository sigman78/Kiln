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
| Mesh optimization | **meshoptimizer** | MIT | cook only (decoder sources may later join `kiln_runtime`) | FetchContent, commit hash | M2 |
| Tangents | **MikkTSpace** (reference `mikktspace.c/.h`) | zlib | cook only | vendored in `third_party/mikktspace/` | M2 |
| Zstd | deferred (v0.6) | BSD | runtime decoder-only build, cook encoder; shared by KTX2 supercompression and `.mesh` blobs | FetchContent, commit hash | v0.6 |
| BCn / ASTC encoders | deferred (v0.6) | to be chosen | cook only | to be chosen | v0.6 |
| Config parsing | deferred (v0.6), lean TOML, decision open | to be chosen | cook only | to be chosen | v0.6 |
| File watching | **own** polling watcher | n/a | runtime (dev builds, `KILN_HOT_RELOAD`) | in-tree `src/io/` | M5 |
| Tests | **own** runner `tests/kiln_test.h` | n/a | tests | in-tree, already in place | M0 |
| Viewer GPU API | raw **Vulkan 1.4** (SDK headers) + **volk** | Apache-2.0 / MIT (headers); MIT (volk) | example viewer only | Vulkan SDK via `find_package(Vulkan)`; volk via FetchContent, commit hash | M4 |

`kiln_runtime` has **zero** third-party dependencies in v0.5. Everything third-party is either
cook-only or example-only. The `.mesh` blob decode loop (mesh-format-spec §5.9) ships in v0.5 with
codec `None` only, which needs no library. Neither zstd nor meshoptimizer is added to
`kiln_runtime` in v0.5.

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

### Zstd (deferred, v0.6+)

- One dependency, two users: **KTX2 Zstd supercompression** and **`.mesh` payload blobs**
  (codec `Zstd` and the `kBlobOuterZstd` flag, used by the Basic and meshopt + Zstd schemes).
- `kiln_runtime` gets a **decoder-only build** (zstd's `decompress/` and `common/` sources, no
  compressor, no legacy formats, no dictionary builder). `kiln_cook` gets the full library for
  encoding.
- The runtime wrapper is one `.cpp` (`src/formats/zstd_decode.cpp`) exposing a kiln function that
  decodes into caller memory, with the context `Allocator` routed through zstd's custom memory
  hooks.
- **Not added in v0.5.** It lands in v0.6 with BCn / supercompression work or with `.mesh`
  compression, whichever comes first. This is the first planned `kiln_runtime` dependency, and the
  runtime-dependency rule below requires this note to carry its reason.

### Mesh processing: meshoptimizer + MikkTSpace

- meshoptimizer (MIT): vertex cache, overdraw and vertex fetch optimization in v0.5. Later:
  simplification (LOD generation, v0.6) and vertex/index codecs, including the
  `EXT_meshopt_compression` decoder.
- **Cook now, maybe runtime later.** If the meshopt or meshopt + Zstd scheme is chosen for `.mesh`
  payloads (decided by measurement in v0.6-0.7), its **vertex and index decoder** sources
  (`vertexcodec.cpp`, `indexcodec.cpp`, `vertexfilter.cpp`, a few files, no allocation) may be
  compiled into `kiln_runtime`, decoder-only. The rest of meshoptimizer stays cook-only.
- **Not in `kiln_runtime` in v0.5.**
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
  In v0.5 there are none. Planned later: zstd (decoder only) and possibly meshoptimizer's decoder
  sources, both for `.mesh` blob codecs (see above).

### Runtime-side dependency rule

`kiln_runtime` accepts third-party code only as **decoders**: matching an encoder or writer that
already lives in `kiln_cook`, built from source with kiln's own compiler flags (never a prebuilt
binary), and never able to write files or import a source format (glTF, PNG). In v0.5 there are
none. The only candidates on the table are zstd (decode-only build) and, if a meshopt-based
`.mesh` payload scheme is chosen, meshoptimizer's vertex/index decoder sources; both keep an
encoder side in `kiln_cook`. This rule, the `include/kiln/cook/` header boundary, and the CI job
that checks `kiln_runtime` builds and links without `kiln_cook`, are the read-only shipping
contract in `shipping-split.md` (M1.5).

## Rationale

- Every v0.5 pick is small, C or C-style C++, and exception-free, which keeps the
  `KILN_NO_EXCEPTIONS` hardening step (v1.0) cheap.
- Keeping the runtime dependency-free makes the shipping link (`kiln_core` + `kiln_runtime`)
  trivial for the external host project.
- Deferring zstd, encoders and config parsing avoids choosing before we have measured needs.
- The blob decode loop exists in v0.5 with codec `None`, so adding zstd or meshopt later is a new
  decoder behind an existing dispatch, not a loader change.

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
- When `.mesh` compression lands, `kiln_runtime` gains its first third-party code. Each codec
  should be behind a CMake option so a project that ships only uncompressed payloads keeps a
  dependency-free runtime.

## Open points for the owner

- Lets go with cgltf
- Use wuffs as first choice for PNG
- Confirm "own KTX2 reader and writer" for v0.5.
- Defer: viewer uses raw Vulkan 1.4 + volk
- Defer: Window library for the viewer (GLFW vs SDL3)