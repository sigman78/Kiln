# API friction log

**Purpose.** The owner integrates kiln into an external project in parallel with development.
Every place where the API is awkward, surprising, missing something, or forces a workaround goes
into this table. It is the input for the API review before each milestone closes and before v0.5
is tagged.

Add one row per friction point. Keep "Friction" to what happened, and "Proposed change" to one
concrete suggestion. Status is one of: Predicted (expected, not yet met in code), Open, Accepted,
Rejected (with reason), Done (with version). The integration examples
(`design/integration-examples.md`) turn each Predicted row into Open or Rejected.

| Date | Reporter | Area | Friction | Proposed change | Status |
|---|---|---|---|---|---|
| 2026-09-28 | agent | adapter threading | GL and sokol calls must run on the context's thread, but `begin_upload` / `commit_upload` may run on workers. The adapter can only queue there, and nothing tells it when to do the GPU work. | Optional `Adapter::flush`, called on the pump thread in `pump()` and `wait()` | Predicted |
| 2026-09-28 | agent | adapter / `wait()` | An adapter that is not self-submitting cannot use `wait()` (K5007), even when its work would complete on the pump thread. | `wait()` accepts `kSelfSubmitting` or `flush` | Predicted |
| 2026-09-28 | agent | `UploadTarget::object` | The GPU object must exist at `begin_upload`, before the data. GL cannot create names on a worker thread. | Document that `GpuObject` may be an adapter table index filled in later | Predicted |
| 2026-09-28 | agent | `publish` without bindless | `publish` gives an `AssetId`; a host with descriptor sets needs its own id-to-material map to know what to rebuild. | Measure in `vk-basic`; maybe a per-frame "changed since" query | Predicted |
| 2026-09-28 | agent | formats | A format the adapter rejects fails the asset (K5004); there is no fallback, so each API needs a cook target that matches it. | Named `TargetProfile` presets per API family | Predicted |
| 2026-09-28 | agent | `.mesh` vertex data | Quantized streams need shader decoding and part dequantization in every renderer. | `VertexProfile::Float` for simple renderers (done, unreleased); decode snippets for the quantized profile | Predicted |
| 2026-09-28 | agent | `destroy_deferred` | GL and sokol can destroy at once; check the contract does not force frame counting on them. | Clarify in `adapter.md` | Predicted |
| 2026-09-28 | agent | host loop | sokol_app owns the main loop (callbacks); `pump()` and `wait()` assume the host owns it. | Measure in `sokol` | Predicted |

Review this table at the end of every milestone; resolved rows that change the API get a
`CHANGELOG.md` entry with migration notes.
