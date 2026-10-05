# Design notes

These notes record kiln's design decisions. Each note is **Proposed** until the owner signs off.
After sign-off, change its status line to `Accepted (date)` and tick it below. Every note states
what is implemented and where; implementation details that deviate from or add to a note are
recorded as R-numbers in `../open-questions.md`.

The notes follow mesh-format-spec draft v0.5 (magic `KMSH`, namespace
`kiln::mesh`, exact-minor version rule while 0.x, resolved `BLOB` rules; open-questions section B).

| Note | Decides |
|---|---|
| [architecture.md](architecture.md) | Diagrams: building blocks and their dependencies, the asset load sequence, the hot reload sequence |
| [dependencies.md](dependencies.md) | Third-party libraries, their scope and pinning, and dependency rules |
| [error-model.md](error-model.md) | `Status`, `Result<T>`, diagnostics, diagnostic code ranges, panic policy |
| [handles-and-states.md](handles-and-states.md) | Handles, `AssetId`, states and transitions (incl. `MetaReady`), hot reload, requests, events, placeholders, load groups |
| [readiness-sets.md](readiness-sets.md) | Proposed (revised 2026-10-04, R25): group readiness. `Group` gains several groups per asset, `seal()`, an aggregate state and group events in the one event stream; a helper requests a mesh's textures; no separate readiness set |
| [runtime-texture-arrays.md](runtime-texture-arrays.md) | First version implemented (v0.7; R12 decided): assemble independently cooked 2D assets into a GPU array at load time; runtime identity, direct layer uploads, ownership, compatibility checks, and transactional reload |
| [adapter.md](adapter.md) | `Format` enum, the renderer adapter interface (`bind`, `destroy`, `caps`), `GpuObject`, binding models, threading contract |
| [adapter-frames-slots.md](adapter-frames-slots.md) | The host reports frames (`PumpOptions`); kiln releases objects after them, finishes abandoned uploads, and numbers bindless slots (`bind` / `destroy` replace `acquire` / `publish` / `destroy_deferred`). Decided 2026-09-28 |
| [settings.md](settings.md) | v0.5 cook settings structs, resolution layers, settings hashing and store key |
| [store-manifest.md](store-manifest.md) | Decided 2026-09-29; phases A and B shipped in v0.6: immutable artifacts named by an XXH3-128 build key; one binary manifest per target profile, rewritten in place; dev freshness by input size and time; one writer; explicit cleanup. Fixes R9 |
| [threading-and-io.md](threading-and-io.md) | std threading in `.cpp` files, `JobSystem`, `IoBackend`, `pump()` as the only surface |
| [bcn-encoding.md](bcn-encoding.md) | Decided 2026-09-29; steps 1–5 implemented (BC1/3/4/5/7 with `rgbcx` and `bc7enc`, BC6H with a C++ port of the ISPC Texture Compressor; every example adapter uploads BC; `desktop` cooks BC); step 6: Zstd supercompression on by default (2026-09-29), RDO deferred: block-compressed textures. BC1/3/4/5/6H/7 per usage, encoders chosen on the Pareto front of dependency size vs speed and quality (measured), determinism, settings and target format families, adapter changes, then Zstd and RDO; ASTC (astcenc) and ETC2 with the mobile targets |
| [target-profiles.md](target-profiles.md) | Decided and implemented 2026-09-29: named target profiles (`compat` default, `desktop`, `uncompressed`) as block-format sets with a usage table; a store records its profile and a mismatched cook never writes to it; a runtime adapter check fails `create()`; replaces the per-adapter sets and the fallback chain |
| [zstd-supercompression.md](zstd-supercompression.md) | Decided 2026-09-29, implemented: lossless Zstd (KTX2 scheme 2) per mip level, on by default and only where it saves 10% of the file; measurements; why not LZ4, Oodle, Basis, a filter scheme or RDO |
| [async-read-path.md](async-read-path.md) | Part 1 accepted (owner, 2026-10-04, R24), in work. Two parts: Part 1 (v0.8) the load benchmark, persistent load attempts, the read contract and blocking readers; Part 2 (v0.9, on hold until the benchmark asks for it) IOCP/io_uring backends. Cooking and cache writes remain outside the scope |
| [shipping-split.md](shipping-split.md) | The read-only shipping contract: the `kiln_runtime` / `kiln_cook` boundary, the `include/kiln/cook/` header split, install components, shipping presets and the shipping CI job |
| [mesh-cook.md](mesh-cook.md) | The glTF to `.mesh` cooker stage order and its determinism rules |
| [cook-kernels.md](cook-kernels.md) | Kernel contract for the cooker's hot paths, SIMD rules, row and item splitting, `CookEnv`, rollout order |
| [mesh-compression.md](mesh-compression.md) | Implemented and decided 2026-10-01 (default `Meshopt`): `.mesh` payload compression (Basic, Meshopt, MeshoptZstd) measured on the Pacer and the Khronos models |
| [cook-tracing.md](cook-tracing.md) | Implemented 2026-09-30: profiling hooks (`kiln/profile.h`) that report zones and intervals of loads and cooks to the host's profiler; `kiln-cook --trace` and the examples' `KILN_TRACE` write a Chrome trace |
| [hot-reload.md](hot-reload.md) | M5: store poller in the runtime, source poller in the cook provider, reload swap through a `next` metadata set, `request_reload`, `IoBackend::stat` |
| [channel-packing.md](channel-packing.md) | Deferred (owner, 2026-10-01: niche): the mesh cook packs a material's separate occlusion and metallic-roughness images into one ORM texture; per-image channel operations (`channels`, `invert`) as a follow-up |
| [project-config.md](project-config.md) | Decided and implemented 2026-10-01 (R15): `kiln.toml` (TOML subset, own parser): project defaults, presets, glob path rules (first match wins), usage-scoped settings (open); layers 3a-3d; staleness through the host digest; hot reload of the file; `kiln-cook --explain` |
| [texture-shapes.md](texture-shapes.md) | Cube maps and arrays from one strip image (`shape`, `slices`, name hints), KTX2 pass-through, the shape at the adapter boundary and in `RequestOptions` |
| [hdr-textures.md](hdr-textures.md) | Draft: Radiance `.hdr` sources with a kiln-owned decoder, RGBA16F output, the float image path; why not OpenEXR or the packed HDR formats yet |
| [integration-examples.md](integration-examples.md) | Proposed: small renderers per API (GL, bindless GL, sokol, basic and bindless Vulkan, NoGraphicsAPI), how each maps the adapter, the float vertex baseline, `Adapter::flush` |
| [texture-only.md](texture-only.md) | Decided: `KILN_MESH=OFF` drops the glTF importer, mesh cooker and their dependencies; the `kMeshes` adapter capability; why the runtime keeps its mesh code for now |
| [asset-model-next.md](asset-model-next.md) | Draft, experimental: scope cut back to one source file per cooked asset (kiln never follows a reference), project boundaries, roots, asset identity (`root:path.ext#sub`) |
| [viewer.md](viewer.md) | The example Vulkan 1.4 adapter (transfer queue, timeline semaphore, bindless slots, deferred destroy) and viewer, its dependencies and offscreen mode |

Related documents:

- [`../open-questions.md`](../open-questions.md): owner questions, implementation decisions
  (R-numbers), spec ambiguities, reserved-space register.
- [`../diagnostics.md`](../diagnostics.md): the `Kxxxx` diagnostic code catalogue.
- [`../api-friction.md`](../api-friction.md): friction log from the external project.
- [`../cook-settings.md`](../cook-settings.md): the full cook-settings model the v0.7 project-settings work fills in.
- [`../ROADMAP.md`](../ROADMAP.md): scope, released versions and the plan.

## Sign-off checklist

The owner ticks each item after reading the note and its "Open points" section.

- [ ] dependencies.md
- [ ] error-model.md
- [ ] handles-and-states.md
- [ ] adapter.md
- [ ] adapter.md: `caps` additions (`acquire` / `publish` gave way to `bind` / `destroy`, decided in adapter-frames-slots.md)
- [ ] settings.md
- [ ] threading-and-io.md
- [ ] shipping-split.md (decided 2026-09-27, except the mechanism choice in its Decision 2)
- [ ] mesh-cook.md
- [ ] cook-kernels.md (rollout approved; kernel contract and open points pending)
- [ ] viewer.md (M4 decisions taken; open points pending)
- [ ] open-questions.md section A (owner questions)
- [ ] open-questions.md R5a-R5l (runtime implementation decisions)
- [ ] open-questions.md section B (mesh-format-spec resolutions, folded into spec v0.3 on
      2026-09-27; B1 magic and namespace and B13-B26 blob table in particular)
