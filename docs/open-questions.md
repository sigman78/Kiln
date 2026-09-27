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
| 10 | Should textures also expose `MetaReady` (extent and format known before pixel data)? | **Decided (2026-09-26): Yes.** One state machine for meshes and textures; a texture at `MetaReady` exposes extent, format, levels and layers, and still samples as its placeholder | Owner decision. Uniform states keep host code and events simple, and UI can pre-size layout. See `design/handles-and-states.md`. |
| 11 | Load groups: may a request belong to more than one group? | **Proposed: one group per `request()` call is enough.** Membership is tracked per request call, not per asset, so the same asset can join several groups through separate requests | Covers the known cases (boot group plus level group) with one field in `RequestOptions` and one member entry per call. See `design/handles-and-states.md`. |

Raised during M0 (not in HANDOFF §13). Numbered R1, R2, ... so they do not clash with HANDOFF §13
numbers (the former row 10 is now R1):

| # | Question | Proposed answer | Reason |
|---|---|---|---|
| R1 | Process-wide log sink and panic handler (`set_log_sink`, `set_panic_handler`) vs the per-context `.log` in HANDOFF §7 | **Keep both.** The process-wide sink is the fallback used by `kiln_core` and by panics, which can fire with no context alive; `Context` takes an optional `LogSink` that overrides it for messages emitted on behalf of that context | `kiln_core` must not know about contexts, and a panic during `create()` still needs somewhere to go. HANDOFF's "no hidden globals except the default allocator" is relaxed to "plus the log sink and panic handler, both write-once at startup". |
| R2 | Hot reload: bump the handle generation (HANDOFF §6.3 wording), or keep it fixed and bump a separate content version? Which one do `publish()`'s `generation` parameter and `Changed` events carry? | **Owner undecided. Proposed: content version.** Slot generation stays fixed for a live asset; a separate `u32` content version increments on each hot reload swap; `publish()` and `Changed` carry the content version | Bindless slots exist so materials never refresh. Bumping the handle generation would force every holder of the handle to refresh, which is the churn the slot model avoids. Cost: one `u32` per registry entry. See `design/handles-and-states.md`. |
| R3 | Adapter hook at request time, so a bindless adapter can hand out a placeholder-bound slot before the asset loads | **Decided (2026-09-26): add `acquire(user, id, kind, texKind, GpuObject* out) -> Status`**, called on the first `request()` of an id, on the requesting thread. Per-frame-lookup adapters may return a null object or leave it null | Owner agreed. HANDOFF v2 §6.4 says the slot is allocated "at request time" but has no call for it; `publish` alone would allocate slots only at `Ready`. See `design/adapter.md`. |

## B. `mesh-format-spec.md` ambiguities

All rows were resolved on 2026-09-27 and folded into `mesh-format-spec.md` draft v0.3. Rows stay as the decision record.

| # | Spec item | Issue | Proposed resolution |
|---|---|---|---|
| B1 | Magic `OMSH`, namespace `orb::mesh`, producer `orb-cook` | Orbital naming inside a kiln format | **Resolved (spec v0.3):** Magic `KMSH`, namespace `kiln::mesh`, producer `kiln-cook`; the spec notes the Orbital `.mesh` origin and that no `OMSH` files need to be read. |
| B2 | §3 "Vertex formats are stored as raw VkFormat. Vulkan-only engine, no translation layer" | kiln is API-agnostic | **Resolved (spec v0.3):** Vertex formats are stored as `kiln::Format`, numerically equal to VkFormat, so the bytes are unchanged and only the §3 wording changes. |
| B3 | §4 `sourceHash` / `cookHash` | What exactly `cookHash` covers | **Resolved (spec v0.3):** `sourceHash` = xxh64 of the source file bytes and `cookHash` = `hash_combine` of settings hash, target hash and cooker version (the store key without the source part), as in `design/settings.md`. |
| B4 | §4 forward-compat rule (`stride` larger than `sizeof(T)`) | Plain `Span<T const>` views break when stride grows | **Resolved (spec v0.3):** Readers expose a strided accessor (element `i` at `offset + i * stride`, copy `min(stride, sizeof(T))` bytes, zero-fill) with a plain span fast path when `stride == sizeof(T)`; spec §4 and §7 say so. |
| B5 | §5.8 `Mount.extrasStr` "key=value;key=value" | Overlaps with reserved `XTRA` section; escaping rules undefined | **Resolved (spec v0.3):** `extrasStr` is flat `key=value;...` with no `;` or `=` inside keys or values (cook warning and drop otherwise), scalar glTF extras only; structured extras wait for `XTRA`. |
| B6 | §5.5 `MeshLod.geometricError` for authored `_lodN` | No error metric exists for authored LODs | **Resolved (spec v0.3):** `geometricError` is 0 unless computed: authored `_lodN` LODs get 0 and the host chooses switch distances; simplifier LODs (v0.6) fill it in. |
| B7 | §5.6 `Submesh.vertexBase` with `IndexType::U8` | Rule says "each submesh under the limit" for U16 only | **Resolved (spec v0.3):** Each submesh references fewer than 2^(8 * indexSize) vertices relative to its `vertexBase`, for every index type; the v0.5 cooker emits U16 or U32 only and U8 is reader-side only. |
| B8 | §8 draw example uses `lod.indexFirstElement` | Field does not exist in `MeshLod` | **Resolved (spec v0.3):** The §8 draw call uses `lod.indexOffset / indexSize + sm.indexFirst` (no new field); a reader view may still offer this as a helper, but the spec does not require one. |
| B9 | Sparse accessors, Draco | HANDOFF §4.1 marks sparse as *(discuss)* | **Resolved (spec v0.3):** Sparse accessors and Draco are rejected with K1xxx diagnostics in v0.5 (spec §1 non-goals). |
| B10 | `.gltf` + external files | "optional" in HANDOFF §4.1 | **Resolved (spec v0.3):** `.gltf` with external `.bin` and image URIs (resolved relative to the `.gltf`) and data URIs is accepted in M2, with hot-reload tracking of the external files; spec header lists it as a source. |
| B11 | §5.7 `TextureBinding.flags` bit0 sRGB | Who sets it | **Resolved (spec v0.3):** The sRGB bit is set from the cooker's slot inference (after resolution), so it matches how the texture was cooked. |
| B12 | §5.2, §5.6, §5.7, §5.8 records | `ModelInfo`, `Submesh`, `MaterialSlot`, `TextureBinding`, `Mount` have no `static_assert` on size | **Resolved (spec v0.3):** Every on-disk record has a `static_assert` on its size in the spec (`Bounds` 32, `VertexAttrib` 12, `ModelInfo` 48, `Submesh` 48, `MaterialSlot` 32, `TextureBinding` 16, `Mount` 48, plus the existing ones); `offsetof` checks land with the M1 code. |
| B13 | §5.9 decode order step 4 vs the `Filter` comments | Step 4 says "Delta first, then ByteShuffle", but the `Delta` comment says "undo after unshuffle", the opposite order. Also `filter` is a single `u8`, so one blob cannot carry both filters | **Resolved (spec v0.3):** One filter per blob; the chained-order sentence is removed from decode step 4, and `Delta` stays reserved and unspecified until its lane width is decided (spec §10). |
| B14 | §5.9 filters vs codecs: order and valid combinations | Decode is outer Zstd, then codec, then unfilter, which implies encode is filter, then codec, then outer Zstd. The spec does not say so, and does not list which codec and filter pairs are valid (for example `MeshoptVertex` + `ByteShuffle`) | **Resolved (spec v0.3):** Filters apply before the codec on encode and are undone after it on decode; the allowed pairs are `None`/`Zstd` with `None`, `ByteShuffle` or `Delta`, `MeshoptVertex` with `None` or a `Meshopt*` filter, and `MeshoptIndex`/`MeshoptIndexSeq` with `None` only, anything else being a writer error and `ValidationFailed` on read. |
| B15 | §5.9 `kBlobOuterZstd` with codec `None` or `Zstd` | `None` + outer Zstd duplicates codec `Zstd`; `Zstd` + outer Zstd is double compression | **Resolved (spec v0.3):** `kBlobOuterZstd` is valid only with the `Meshopt*` codecs. |
| B16 | §2 invariant 3 and §5.9 "Granularity": alignment of split decoded ranges | Every decoded range must start 16-B aligned, but splits "on whole-vertex boundaries" or "whole triangles" do not land on 16 B for a 24-B stride or U16 triangles (6 B) | **Resolved (spec v0.3):** Split points are multiples of `lcm(unit, 16)`, with `unit` = `elementSize` for vertex blobs and 3 x index size for index blobs (24-B stride and U16 triangles both give 48 B). |
| B17 | §5.9 index blobs: size and alignment after decode | Invariant 4 (index ranges are multiples of the index size) is stated per LOD, not per split blob. `MeshoptIndex` needs triangle lists, and meshopt's index codecs support 2- and 4-byte indices only, while §5.5 allows U8 | **Resolved (spec v0.3):** Each index blob has `elementSize` = index size, starts 16-B aligned, and has `decodedSize` a multiple of 3 x index size for `MeshoptIndex` and of the index size otherwise; U8 indices only with `None` or `Zstd`; other combinations are a K3xxx cook error and `ValidationFailed` on read. |
| B18 | §5.9 blob overlap and table order | Only decoded ranges are said not to overlap. Encoded ranges might overlap (dedupe?), and the order of `BLOB` entries is not defined (by `encodedOffset`, `decodedOffset` or `lodRank`?) | **Resolved (spec v0.3):** Encoded ranges must not overlap, `BLOB` is sorted by `encodedOffset` ascending with `lodRank` non-decreasing along the table, decoded order is free, and violations are `Corrupt`. |
| B19 | §4 `gpuDataSize` when compressed | "Encoded bytes in file" does not say whether it includes padding between blobs and at the tail, or whether it equals `GPUD`'s section size and `fileSize - gpuDataOffset` | **Resolved (spec v0.3):** `gpuDataSize == GPUD SectionEntry.size == fileSize - gpuDataOffset`, padding included, and with `kPayloadRaw` also `gpuDataSize == payloadDecodedSize`; the adapter always receives `payloadDecodedSize`. |
| B20 | §2 and §5.9 raw fast path: `kPayloadRaw` and `sectionCount` / `BLOB` | "Ignores BLOB except for validation" leaves open whether `BLOB` may be omitted (changing `sectionCount`) and what "encoded layout == decoded layout" requires per blob | **Resolved (spec v0.3):** `BLOB` is always required and counted in `sectionCount`; `kPayloadRaw` requires every blob to have codec `None`, filter `None`, flags 0, `encodedOffset == decodedOffset`, `encodedSize == decodedSize` and zero `GPUD` gaps, which the loader validates (`Corrupt` otherwise); all-`None` blobs without the flag are valid and use the decode loop. |
| B21 | §5.9 and §7 "decoders never write more than `decodedSize`" | Says nothing about writing less | **Resolved (spec v0.3):** Decoder output must be exactly `decodedSize`; short output is `Corrupt`. |
| B22 | §5.9 `lodRank` across parts | "0 = coarsest LOD", but parts have different LOD counts, so the rank of a blob in a multi-part mesh is ambiguous | **Resolved (spec v0.3):** `lodRank = lodCount(part) - 1 - lodIndex`, capped at 255, so every part's coarsest LOD has rank 0. |
| B23 | §5.9 `checksum`: "0 = not stored" | A real xxh32 can be 0 | **Resolved (spec v0.3):** Checksum 0 means "not stored", even if the real xxh32 is 0; no format change. |
| B27 | §5.9 `checksum` uses xxh32 while all other hashes are FNV-1a 64 / xxh64 | A second hash function in core | **Resolved (spec v0.3):** The checksum stays xxh32, computed with `kiln::xxh32` (added to `hash.h` on 2026-09-26). |
| B24 | §5.9 `PayloadBlob` offsets and sizes are `u32`; header sizes are `u64` | The limit is not stated | **Resolved (spec v0.3):** At most 4 GiB encoded and 4 GiB decoded per `.mesh`; the cooker fails above that (K4xxx writer check), and header fields stay `u64`. |
| B25 | §5.9 "unsupported codec or filter: placeholder plus diagnostic" | HANDOFF v2 has no mesh placeholder | **Resolved (spec v0.3):** The mesh goes to `Failed` with `Unsupported` and one diagnostic; there is no mesh placeholder and `is_ready()` stays false (spec sentence reworded). |
| B26 | §4 version rule vs the v0.2 changes | v0.2 grows the header to 80 B and changes what `MeshLod` offsets mean, yet only `versionMinor` is bumped, and minor bumps are "additive, loader tolerates" | **Resolved (spec v0.3):** While `versionMajor == 0` a loader requires an exact `versionMinor` match and reports `VersionMismatch` otherwise; the additive minor rule applies from 1.0; spec `kVersionMinor` is 3. |

## C. Reserved-space register

Every item that a design note reserves for later. Nothing here is built in v0.5.

| Item | Where | Reserved for | Target version |
|---|---|---|---|
| `State::Partial` (fits between `MetaReady` and `Ready`) | handles-and-states | progressive mip/LOD loads | v0.8 |
| `RequestOptions` range field | handles-and-states | partial loads | v0.8 |
| `RequestOptions.group`: one group per request call | handles-and-states | several groups per request, if ever needed | if needed |
| `GroupStatus` fields beyond counts and bytes (for example per-priority counts) | handles-and-states | richer loading screens | if needed |
| Priority boost used by `wait()`, internal only | handles-and-states | a public third priority level | if needed |
| `TextureKind` values after `Emissive` | handles-and-states | more placeholder kinds (detail, mask) | as needed |
| `ContextDesc.devPlaceholders`, `ContextDesc.placeholders[kind]` | handles-and-states | per-kind Failed placeholders, runtime toggle | as needed |
| Placeholder asset ids 6..15 | handles-and-states | more built-in placeholders | as needed |
| Deferred / LRU unload at refcount 0 | handles-and-states | memory reuse on churn | v0.6+ |
| `Adapter::reserved[4]` | adapter | residency hooks (budget, eviction) | v0.8 |
| `TextureDesc.firstLevel` | adapter | partial mip uploads | v0.8 |
| `Format` BC1-BC7 (131-146) | adapter | block compression | v0.6 |
| `Format` ETC2/EAC (147-156), ASTC (157-184) | adapter | mobile targets | v0.9 |
| `Adapter` `workerUploads` flag (or an `AdapterCaps` bit) | adapter | pump-thread-only adapters | if needed |
| `AdapterCaps` bits 1..31 (only `kSelfSubmitting` defined) | adapter | new capability flags | as needed |
| `GpuObject.kind` | adapter | adapter-defined tag; kiln never interprets it | adapter-owned |
| `TextureCookSettings`: alphaMode, premultiply, dilation, encoding, supercompression, residentMips, shape | settings | quality and streaming | v0.6-v0.8 |
| `MeshCookSettings.genLods` (field exists, `true` rejected) | settings | simplifier | v0.6 |
| `MeshCookSettings`: indexWidthPolicy, unit/axis override, prefixes | settings | encoding and import options | v0.6+ |
| `MeshCookSettings.compression` values `Basic`, `Meshopt`, `MeshoptZstd` (field exists, only `None` accepted) | settings | `.mesh` payload compression | v0.6-0.7 |
| `MeshCookSettings.zstdLevel` (ignored), `blobChunkSize` (non-zero rejected) | settings | compression tuning, split blobs | v0.6-0.7 |
| Per-target default compression scheme in `TargetProfile` | settings | per-target selection | v0.6 |
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
| `Codec` `Zstd`, `MeshoptVertex`, `MeshoptIndex`, `MeshoptIndexSeq` and `kBlobOuterZstd` (defined, with the allowed codec/filter pairs of spec v0.3; v0.5 runtime rejects them) | mesh-format-spec §5.9 | payload compression | v0.6-0.7 |
| `Codec` 5..127 reserved, 128..255 vendor / experimental | mesh-format-spec §5.9 | new codecs | as needed |
| `Filter` 1..5 (defined; v0.5 runtime rejects them; `Delta` also waits for its lane width), 6..255 (one filter per blob, so a combined shuffle-plus-delta value would take one of these) | mesh-format-spec §5.9 | new filters | v0.6+ |
| `BlobFlags` bits 1..15, `HeaderFlags` bits 1..31 | mesh-format-spec §4, §5.9 | new blob and header flags | as needed |
| `PayloadBlob._reserved`, `FileHeader._reserved` | mesh-format-spec §4, §5.9 | appended fields | as needed |
| Zstd decoder and meshopt decoder sources in `kiln_runtime` | dependencies | blob codecs | v0.6-0.7 |
| `Semantic::Joints`, `Semantic::Weights` | mesh-format-spec §5.3 | skinning | post-v1 |
| `TextureSlot` 5-15 | mesh-format-spec §5.7 | engine-defined slots | as needed |
