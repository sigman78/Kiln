# Design notes (M0)

These notes record the decisions HANDOFF §12 asks for before more code lands. Each note is
**Proposed** until the owner signs off. After sign-off, change its status line to
`Accepted (date)` and tick it below.

The notes were aligned to HANDOFF v2 and mesh-format-spec v0.2 on 2026-09-26: `.mesh` blob table,
`MetaReady`, kind-specific texture placeholders, load groups, adapter `acquire` / `publish` /
`caps`, and `kiln::gpu()`. Changed decisions stay **Proposed**.

| Note | Decides |
|---|---|
| [dependencies.md](dependencies.md) | Third-party libraries for v0.5, their scope and pinning, and dependency rules |
| [error-model.md](error-model.md) | `Status`, `Result<T>`, diagnostics, diagnostic code ranges, panic policy |
| [handles-and-states.md](handles-and-states.md) | Handles, `AssetId`, states and transitions (incl. `MetaReady`), hot reload, requests, events, placeholders, load groups |
| [adapter.md](adapter.md) | `Format` enum, the renderer adapter interface (`acquire`, `publish`, `caps`), `GpuObject`, binding models, threading contract |
| [settings.md](settings.md) | v0.5 cook settings structs, resolution layers, settings hashing and store key |
| [threading-and-io.md](threading-and-io.md) | std threading in `.cpp` files, `JobSystem`, `IoBackend`, `pump()` as the only surface |

Related documents:

- [`../open-questions.md`](../open-questions.md): HANDOFF §13 answers, spec ambiguities, reserved-space register.
- [`../api-friction.md`](../api-friction.md): friction log from the external project.
- [`../cook-settings.md`](../cook-settings.md): stub for the v0.6 layered settings design.

## Sign-off checklist

The owner ticks each item after reading the note and its "Open points" section.

- [ ] dependencies.md
- [ ] error-model.md
- [ ] handles-and-states.md
- [ ] adapter.md
- [ ] adapter.md: acquire/publish/caps additions
- [ ] settings.md
- [ ] threading-and-io.md
- [ ] open-questions.md section A (HANDOFF §13 answers)
- [ ] open-questions.md section B (mesh-format-spec resolutions, B1 magic and namespace and B13-B26 blob table in particular)
