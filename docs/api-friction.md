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
| 2026-09-28 | agent | adapter threading | GL and sokol calls must run on the context's thread, but `begin_upload` / `commit_upload` may run on workers. The adapter can only queue there, and nothing tells it when to do the GPU work. | Optional `Adapter::flush`, called on the pump thread in `pump()` and `wait()` | Open: `kiln-gl` needs a host-side `gl_adapter_flush()` before every `pump()`; a host that forgets it never sees an upload complete |
| 2026-09-28 | agent | adapter / `wait()` | An adapter that is not self-submitting cannot use `wait()` (K5007), even when its work would complete on the pump thread. | `wait()` accepts `kSelfSubmitting` or `flush` | Predicted |
| 2026-09-28 | agent | `UploadTarget::object` | The GPU object must exist at `begin_upload`, before the data. GL cannot create names on a worker thread. | Document that `GpuObject` may be an adapter table index filled in later | Rejected as an API change: `kiln-gl` puts its table index in `native` and creates the GL name at flush; this works. Document the pattern in `adapter.md` |
| 2026-09-28 | agent | `publish` without bindless | `publish` gives an `AssetId`; a host with descriptor sets needs its own id-to-material map to know what to rebuild. | Measure in `vk-basic`; maybe a per-frame "changed since" query | Predicted (not hit in `kiln-gl`: it calls `gpu()` per draw, so a new version needs no bookkeeping) |
| 2026-09-28 | agent | formats | A format the adapter rejects fails the asset (K5004); there is no fallback, so each API needs a cook target that matches it. | Named `TargetProfile` presets per API family | Predicted |
| 2026-09-28 | agent | `.mesh` vertex data | Quantized streams need shader decoding and part dequantization in every renderer. | `VertexProfile::Float` for simple renderers; decode snippets for the quantized profile | Accepted: `VertexProfile::Float` is done (unreleased) and `kiln-gl`'s shaders read plain floats; the snippets stay open |
| 2026-09-28 | agent | `destroy_deferred` | GL and sokol can destroy at once; check the contract does not force frame counting on them. | Clarify in `adapter.md` | Rejected: `kiln-gl` deletes at once and GL keeps objects alive for issued commands; no frame counting needed. Say so in `adapter.md` |
| 2026-09-28 | agent (`kiln-gl`) | texture layout | `UploadDesc` gives the `TextureDesc` but not the level offsets, so every adapter re-implements kiln's layout rule (`vk_adapter.cpp` and `gl_adapter.cpp` each copy `texture_layout`). | A public `texture_level_layout(TextureDesc, CopyConstraints, offsets, pitches)` in `adapter.h`, or the offsets in `UploadDesc` | Open |
| 2026-09-28 | agent (`kiln-gl`) | `destroy_deferred` during an upload | Unloading an asset whose upload is in flight calls `destroy_deferred` on its object, and kiln never polls that token again. The adapter must notice and retire the upload itself, or it leaks staging space. Not in the contract. | Document it in `adapter.md` (the token is abandoned; the adapter retires it) | Open |
| 2026-09-28 | agent (`kiln-gl`) | cube maps | Faces arrive as authored (+X -X +Y -Y +Z -Z, a left-handed frame); a right-handed renderer must flip Z when it samples. `kiln-viewer` and `kiln-gl` both found this by hand. | State the convention and the flip in `texture-shapes.md` | Open |
| 2026-09-28 | agent (`kiln-gl`) | material bindings | A `TextureBinding` with `kTextureExternal` has `textureId` 0, so the host resolves the name itself (`resolve_asset_name`) and hashes it to find the handle; embedded ones could use `find_texture(ctx, textureId)` directly. | A helper that returns the texture asset name of a binding | Open |
| 2026-09-28 | agent | host loop | sokol_app owns the main loop (callbacks); `pump()` and `wait()` assume the host owns it. | Measure in `sokol` | Predicted |

Review this table at the end of every milestone; resolved rows that change the API get a
`CHANGELOG.md` entry with migration notes.
