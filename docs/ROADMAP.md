# kiln roadmap

kiln sits between source assets and any renderer. It cooks glTF and images into GPU-ready,
API-agnostic formats (`.mesh`, KTX2), then loads them asynchronously through an adapter the
renderer implements, with optional hot reload. Decisions live in [`design/`](design/README.md),
unresolved items in [`open-questions.md`](open-questions.md), and every change in
[`../CHANGELOG.md`](../CHANGELOG.md). The order below may change with feedback.

## Scope

These rules bound every release:

1. **kiln never calls a graphics API.** All GPU work goes through the adapter interface.
2. **The runtime reads only cooked data**, or data the host registers in memory. Dev builds cook
   on a miss, behind the handle.
3. **Cooking happens on the host.** Shipping builds link only `kiln_core` + `kiln_runtime`, read a
   pre-cooked store and never write files (`design/shipping-split.md`).

Not goals for v1:
- Rendering, shaders, materials as shaders, scene graphs, ECS, gameplay.
- Animation, skinning and morph targets (format space is reserved).
- FBX, OBJ, USD import: external tools convert them to glb.
- Virtual texturing, GPU decompression, DirectStorage. Chunked compression and range reads keep
  the door open.

## Released

| Version | Date | Contents |
|---|---|---|
| **v0.5** | 2026-09-29 | M0-M5: `.mesh` and KTX2 formats, the cooker, the async runtime (placeholders, `MetaReady`, load groups), the adapter interface with Vulkan, GL, sokol and NoGraphicsAPI examples, hot reload; BC1-BC7 and Zstd textures |
| **v0.6** | 2026-09-30 | Target profiles; PBR material factors; the store manifest with `kiln-cook --gc` and `--export` |

## v0.7: quality and pipeline

- **Runtime texture arrays** ([`design/runtime-texture-arrays.md`](design/runtime-texture-arrays.md);
  runtime and the examples `kiln-gl-array`, `kiln-vk-array` done 2026-09-30). Independently cooked 2D assets assembled into one GPU array at load time. First
  version (owner, 2026-09-30): one aggregate upload within today's adapter contract, file-backed 2D
  members, whole-array reload. Range uploads wait for v0.8. Still to do: an array mode in the
  sokol and NGA examples (a `--dump` frame; byte-exact `--verify` where the API can read a texture
  back; sokol cannot). Moves to v0.8 if it needs adapter changes.
- **Faster BC encoding** (done 2026-09-30: Basis `bc7f` for BC7 Fast and Normal, the provider at
  Fast; open-questions R14). Was: decide from the direct-encoder benchmark in
  [`design/bcn-encoding.md`](design/bcn-encoding.md) (Basis `bc7f` / `bc6hf` as fast presets,
  scalar `bc7e` as an offline quality mode, or SIMD in the current encoders). The output stays
  deterministic across threads and compilers.
- **RDO** for BC textures (deferred from the BC rollout).
- **Profiling hooks** ([`design/cook-tracing.md`](design/cook-tracing.md); done 2026-09-30): zones and
  intervals into the host's profiler; `kiln-cook --trace` and `KILN_TRACE` write a Chrome trace.
  Follow-ups from the first traces: a glTF's images cook in parallel (done 2026-09-30); the upload
  job's time (up to 100 ms per 2048² texture); parallel PNG decode.
- **`.mesh` payload compression.** Implement the candidate schemes (Zstd + byte shuffle,
  meshopt, both), measure ratio and decode MB/s on real assets, pick a default. Settle the
  split-blob rules (mesh-format-spec §10, open-questions B16).
- **LOD generation** (simplifier), which fills `MeshLod.geometricError`.
- **Textures:** alpha-coverage-preserving mips, channel packing (for example ORM from separate
  images).
- **Project settings:** config files, presets, path rules, target encodings and
  `kiln-cook --explain` ([`cook-settings.md`](cook-settings.md); file format: open question 7).
- **Fuller dependency tracking for hot reload.** Texture arrays need it too: a changed member
  must reach the arrays that contain it.

## v0.8: streaming

- Range requests; progressive mips and LODs (`State::Partial`, `TextureDesc::firstLevel`).
- An adapter contract for range uploads into an existing object, shared with texture arrays.
- Residency, budget and eviction hooks (`Adapter::reserved`).

## v0.9: platforms

- Mobile target profiles and cross-cooking: ASTC through astcenc, ETC2 only if a target needs it.
- Native async IO backends (Win32 overlapped / IoRing, io_uring, dispatch IO;
  [`design/async-read-path.md`](design/async-read-path.md)).
- Native file watchers; macOS preset and CI.
- A network `IoBackend` for on-device iteration *(discuss)*.

## v1.0: hardening and freeze

- `.mesh` v1 format freeze.
- `KILN_NO_EXCEPTIONS` / `KILN_NO_RTTI` options (`/EHs-c- /GR- /D_HAS_EXCEPTIONS=0`,
  `-fno-exceptions -fno-rtti`), built in CI.
- Sanitizers on every platform; fuzzing; docs.

## Unscheduled

- `EXT_meshopt_compression` input (the importer's buffer-view resolution leaves room for a decode
  step).
- GPU decompression of chunked blobs.
- Pack files, and `create()` with a manifest path (open point 3 of `design/store-manifest.md`).
- Store manifest phase C: a memory-mapped manifest, several writers (set aside 2026-09-30).
- EXR sources; a bgfx adapter example.
- Load-throughput and cook-time benchmarks; tracking full-rebuild time in CI (flag regressions
  over 20%).
- GPU conformance runs under the Vulkan validation layer (with synchronization validation): the
  examples' `--dump` and `--verify` modes. This machine has no layer yet; CI only compiles Vulkan.
- Extracting `kiln/core` into a separate utility library.
- Proposed notes awaiting the owner: [`design/readiness-sets.md`](design/readiness-sets.md).

## How the plan runs

- **External battle test.** The owner integrates kiln into a larger project alongside
  development. kiln stays consumable through `find_package` and `add_subdirectory` at every
  release. Friction goes to [`api-friction.md`](api-friction.md) and is reviewed each release.
- **Pre-1.0 API breaks** are allowed; each is recorded in `CHANGELOG.md` with migration notes.
- **Beyond the current version, reserve space instead of building it:** a field, a section ID or
  an interface hook, listed in the reserved-space register of `open-questions.md`.
- **Decisions are measured** where they can be: compile time, load MB/s, cook time, allocation
  counts, quality numbers.
