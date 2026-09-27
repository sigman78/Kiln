# Open questions

Proposed answers wait for owner sign-off. When the owner decides, replace "Proposed" with
"Decided (date)" and keep the reason.

## A. Owner questions from HANDOFF §13

| # | Question | Proposed answer | Reason |
|---|---|---|---|
| 1 | Project name and license | Name **kiln**, license **MIT** | Already in `LICENSE`; changing before the first release is trivial. |
| 2 | C++20 or C++23 | **C++20** baseline; C++23 features only when MSVC, clang and gcc all support them | MSVC is the constraint; nothing in v0.5 needs C++23. |
| 3 | Own threading wrapper vs std | **`<thread>`/`<mutex>` in `.cpp` files only**; thin OS wrapper only for file IO, directory listing, file times, atomic rename, native watchers later | Portable today, no header cost. See `design/threading-and-io.md`. |
| 4 | fastgltf vs cgltf | **cgltf** | C99, no exceptions, easy to isolate; parse speed does not matter on the cook side. See `design/dependencies.md`. |
| 5 | Format enum | **Compact enum with values equal to `VkFormat`**, plus a constexpr `format_info()` table | KTX2 and `.mesh` already store VkFormat numbers; Vulkan adapter is a cast. See `design/adapter.md`. |
| 6 | Material remapping | **Host's job.** kiln ships material names, name hashes and authored texture bindings only | Remapping is engine policy; a data-driven table can be added later without format changes. |
| 7 | Config file format | **Deferred to v0.6**, leaning TOML | No config files in v0.5; structs are the interface. |
| 8 | Viewer on raw Vulkan or a thin layer | **Raw Vulkan 1.4 + volk**, final decision at M4 | The viewer exists to show the adapter is small; a layer would hide that code. |
| 9 | Pack files in v1 | **Out of v1 scope.** `IoBackend` keeps range reads so a pack backend can slot in | Directory stores are enough for v0.5 and the external project. |

Raised during M0 (not in HANDOFF §13):

| # | Question | Proposed answer | Reason |
|---|---|---|---|
| 10 | Process-wide log sink and panic handler (`set_log_sink`, `set_panic_handler`) vs the per-context `.log` in HANDOFF §7 | **Keep both.** The process-wide sink is the fallback used by `kiln_core` and by panics, which can fire with no context alive; `Context` takes an optional `LogSink` that overrides it for messages emitted on behalf of that context | `kiln_core` must not know about contexts, and a panic during `create()` still needs somewhere to go. HANDOFF's "no hidden globals except the default allocator" is relaxed to "plus the log sink and panic handler, both write-once at startup". |

## B. `mesh-format-spec.md` ambiguities

| # | Spec item | Issue | Proposed resolution |
|---|---|---|---|
| B1 | Magic `OMSH`, namespace `orb::mesh`, producer `orb-cook` | Orbital naming inside a kiln format | Use `KMSH` and `kiln::mesh`, producer `kiln-cook`. Alternative: keep `OMSH` for file compatibility with Orbital. **Owner decides.** |
| B2 | §3 "Vertex formats are stored as raw VkFormat. Vulkan-only engine, no translation layer" | kiln is API-agnostic | Resolved by the `Format` decision: values equal VkFormat, so the bytes stay the same and the wording changes to "stored as `kiln::Format` (numerically equal to VkFormat)". |
| B3 | §4 `sourceHash` / `cookHash` | What exactly `cookHash` covers | Both xxh64 as specified. `sourceHash` = xxh64 of source bytes. `cookHash` = `hash_combine` of settings hash, target hash and cooker version (the store key without the source part). See `design/settings.md`. |
| B4 | §4 forward-compat rule (`stride` larger than `sizeof(T)`) | Plain `Span<T const>` views break when stride grows | The reader exposes a strided accessor (`StridedView<T>`: element `i` at `offset + i * stride`, copy `min(stride, sizeof(T))` bytes, zero-fill). It returns a plain `Span` fast path when `stride == sizeof(T)`. |
| B5 | §5.8 `Mount.extrasStr` "key=value;key=value" | Overlaps with reserved `XTRA` section; escaping rules undefined | v0.5: flat string, keys and values must not contain `;` or `=` (cook warning and drop otherwise). Only scalar glTF extras are carried. Structured extras wait for `XTRA`. |
| B6 | §5.5 `MeshLod.geometricError` for authored `_lodN` | No error metric exists for authored LODs | 0 unless computed. For authored LODs the cooker writes 0 in v0.5 and documents that the host must choose switch distances itself. Simplifier-generated LODs (v0.6) fill it in. |
| B7 | §5.6 `Submesh.vertexBase` with `IndexType::U8` | Rule says "each submesh under the limit" for U16 only | Same rule generalized: each submesh must reference fewer than 2^(8 * indexSize) vertices relative to its `vertexBase`. v0.5 cooker never emits U8 (U16 or U32 only); U8 support is reader-side only. |
| B8 | §8 draw example uses `lod.indexFirstElement` | Field does not exist in `MeshLod` | Replace with `lod.indexOffset / indexSize` in the spec text, or have the view compute it. Proposed: view exposes `indexFirstElement`. |
| B9 | Sparse accessors, Draco | HANDOFF §4.1 marks sparse as *(discuss)* | Reject both with K1xxx diagnostics in v0.5. Sparse is mostly used for morph targets, which are out of scope. |
| B10 | `.gltf` + external files | "optional" in HANDOFF §4.1 | Support in M2 if cgltf's file loading is enough (it is): external `.bin` and image URIs resolved relative to the `.gltf`. Data URIs supported. Hot-reload dependency tracking covers the `.bin` too. |
| B11 | §5.7 `TextureBinding.flags` bit0 sRGB | Who sets it | Derived from slot inference (after resolution), so it matches how the texture was actually cooked. |
| B12 | §5.2, §5.6, §5.7, §5.8 records | `ModelInfo`, `Submesh`, `MaterialSlot`, `TextureBinding`, `Mount` have no `static_assert` on size | Add `static_assert` for all records and `offsetof` checks in M1 (HANDOFF §2). |

## C. Reserved-space register

Every item that a design note reserves for later. Nothing here is built in v0.5.

| Item | Where | Reserved for | Target version |
|---|---|---|---|
| `State::Partial` | handles-and-states | progressive mip/LOD loads | v0.8 |
| `RequestOptions` range field | handles-and-states | partial loads | v0.8 |
| Deferred / LRU unload at refcount 0 | handles-and-states | memory reuse on churn | v0.6+ |
| `Adapter::reserved[4]` | adapter | residency hooks (budget, eviction) | v0.8 |
| `TextureDesc.firstLevel` | adapter | partial mip uploads | v0.8 |
| `Format` BC1-BC7 (131-146) | adapter | block compression | v0.6 |
| `Format` ETC2/EAC (147-156), ASTC (157-184) | adapter | mobile targets | v0.9 |
| `Adapter` `workerUploads` flag | adapter | pump-thread-only adapters | if needed |
| `TextureCookSettings`: alphaMode, premultiply, dilation, encoding, supercompression, residentMips, shape | settings | quality and streaming | v0.6-v0.8 |
| `MeshCookSettings.genLods` (field exists, `true` rejected) | settings | simplifier | v0.6 |
| `MeshCookSettings`: indexWidthPolicy, compression, unit/axis override, prefixes | settings | encoding and import options | v0.6+ |
| Resolution layers 3-6 and `--explain` | settings | presets, targets, rules, sidecars | v0.6 |
| Per-field "set" mask for layering | settings | layered overrides | v0.6 |
| Additional `TargetProfile`s (mobile) | settings | cross-cooking | v0.9 |
| `IoBackend::read_range` completion token + `poll` | threading-and-io | native async IO | v0.9 |
| Pack-file `IoBackend` | threading-and-io | archives | unscheduled |
| Native file watchers | threading-and-io | hot reload | v0.9 |
| `try`/`catch` boundary compiled out under `KILN_NO_EXCEPTIONS` | error-model | hardening | v1.0 |
| Diagnostic ranges K6000-9999 | error-model | new areas | as needed |
| `Code` values (append only) | error-model | new failure kinds | as needed |
| `EXT_meshopt_compression` decode at buffer-view resolution | dependencies | compressed glTF input | unscheduled |
| libktx or Basis transcoder | dependencies | Basis / Zstd supercompression | v0.6+ |
| `.mesh` sections `SKIN`, `MORF`, `MLET`, `COLL`, `XTRA` | mesh-format-spec §9 | skinning, morphs, meshlets, collision, structured extras | post-v1 |
| `.mesh` `flags` fields (header, sections, records) | mesh-format-spec | future flags | as needed |
| `Semantic::Joints`, `Semantic::Weights` | mesh-format-spec §5.3 | skinning | post-v1 |
| `TextureSlot` 5-15 | mesh-format-spec §5.7 | engine-defined slots | as needed |
