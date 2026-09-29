# Design notes

These notes record kiln's design decisions. Each note is **Proposed** until the owner signs off.
After sign-off, change its status line to `Accepted (date)` and tick it below. Every note states
what is implemented and where; implementation details that deviate from or add to a note are
recorded as R-numbers in `../open-questions.md`.

The notes follow HANDOFF v2 and mesh-format-spec draft v0.3 (magic `KMSH`, namespace
`kiln::mesh`, exact-minor version rule while 0.x, resolved `BLOB` rules; open-questions section B).

| Note | Decides |
|---|---|
| [architecture.md](architecture.md) | Diagrams: building blocks and their dependencies, the asset load sequence, the hot reload sequence |
| [dependencies.md](dependencies.md) | Third-party libraries, their scope and pinning, and dependency rules |
| [error-model.md](error-model.md) | `Status`, `Result<T>`, diagnostics, diagnostic code ranges, panic policy |
| [handles-and-states.md](handles-and-states.md) | Handles, `AssetId`, states and transitions (incl. `MetaReady`), hot reload, requests, events, placeholders, load groups |
| [adapter.md](adapter.md) | `Format` enum, the renderer adapter interface (`acquire`, `publish`, `caps`), `GpuObject`, binding models, threading contract |
| [settings.md](settings.md) | v0.5 cook settings structs, resolution layers, settings hashing and store key |
| [threading-and-io.md](threading-and-io.md) | std threading in `.cpp` files, `JobSystem`, `IoBackend`, `pump()` as the only surface |
| [shipping-split.md](shipping-split.md) | The read-only shipping contract: the `kiln_runtime` / `kiln_cook` boundary, the `include/kiln/cook/` header split, install components, shipping presets and the shipping CI job |
| [mesh-cook.md](mesh-cook.md) | The glTF to `.mesh` cooker stage order and its determinism rules |
| [cook-kernels.md](cook-kernels.md) | Kernel contract for the cooker's hot paths, SIMD rules, row and item splitting, `CookEnv`, rollout order |
| [hot-reload.md](hot-reload.md) | M5: store poller in the runtime, source poller in the cook provider, reload swap through a `next` metadata set, `request_reload`, `IoBackend::stat` |
| [texture-shapes.md](texture-shapes.md) | Cube maps and arrays from one strip image (`shape`, `slices`, name hints), KTX2 pass-through, the shape at the adapter boundary and in `RequestOptions` |
| [hdr-textures.md](hdr-textures.md) | Draft: Radiance `.hdr` sources with a kiln-owned decoder, RGBA16F output, the float image path; why not OpenEXR or the packed HDR formats yet |
| [texture-only.md](texture-only.md) | Decided: `KILN_MESH=OFF` drops the glTF importer, mesh cooker and their dependencies; the `kMeshes` adapter capability; why the runtime keeps its mesh code for now |
| [asset-model-next.md](asset-model-next.md) | Draft, experimental: scope cut back to one source file per cooked asset (kiln never follows a reference), project boundaries, roots, asset identity (`root:path.ext#sub`) |
| [viewer.md](viewer.md) | The example Vulkan 1.4 adapter (transfer queue, timeline semaphore, bindless slots, deferred destroy) and viewer, its dependencies and offscreen mode |

Related documents:

- [`../open-questions.md`](../open-questions.md): owner questions, implementation decisions
  (R-numbers), spec ambiguities, reserved-space register.
- [`../diagnostics.md`](../diagnostics.md): the `Kxxxx` diagnostic code catalogue.
- [`../api-friction.md`](../api-friction.md): friction log from the external project.
- [`../cook-settings.md`](../cook-settings.md): stub for the v0.6 layered settings design.

## Sign-off checklist

The owner ticks each item after reading the note and its "Open points" section.

- [ ] dependencies.md
- [ ] error-model.md
- [ ] handles-and-states.md
- [ ] adapter.md
- [ ] adapter.md: `acquire` / `publish` / `caps` additions
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
