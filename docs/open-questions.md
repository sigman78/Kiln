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
| B13 | §5.9 decode order step 4 vs the `Filter` comments | Step 4 says "Delta first, then ByteShuffle", but the `Delta` comment says "undo after unshuffle", the opposite order. Also `filter` is a single `u8`, so one blob cannot carry both filters | One filter per blob. Drop the chained-order sentence from step 4. If Delta plus ByteShuffle is ever needed, add a combined value (for example `ShuffleDelta`) whose decode order is fixed: unshuffle, then undo delta. Delta stays unused until its lane width is specified (spec §10). |
| B14 | §5.9 filters vs codecs: order and valid combinations | Decode is outer Zstd, then codec, then unfilter, which implies encode is filter, then codec, then outer Zstd. The spec does not say so, and does not list which codec and filter pairs are valid (for example `MeshoptVertex` + `ByteShuffle`) | State it: **filters apply before the codec on encode and are undone after it on decode.** Add an allowed-combination table: codec `None` or `Zstd` with filter `None`, `ByteShuffle` or `Delta`; `MeshoptVertex` with `None` or a `Meshopt*` filter; `MeshoptIndex` / `MeshoptIndexSeq` with `None` only. Any other pair is `ValidationFailed` on read and a writer error. |
| B15 | §5.9 `kBlobOuterZstd` with codec `None` or `Zstd` | `None` + outer Zstd duplicates codec `Zstd`; `Zstd` + outer Zstd is double compression | `kBlobOuterZstd` is valid only with the `Meshopt*` codecs. One canonical encoding per scheme also keeps writer output deterministic. |
| B16 | §2 invariant 3 and §5.9 "Granularity": alignment of split decoded ranges | Every decoded range must start 16-B aligned, but splits "on whole-vertex boundaries" or "whole triangles" do not land on 16 B for a 24-B stride or U16 triangles (6 B) | Split points are multiples of `lcm(unit, 16)`, where `unit` is `elementSize` for vertex blobs and 3 x index size for index blobs (48 B in both examples). Split blobs then start 16-B aligned with no padding inside a stream. Alternative: require 16-B alignment only for the first blob of a stream or index range. Proposed: the lcm rule. |
| B17 | §5.9 index blobs: size and alignment after decode | Invariant 4 (index ranges are multiples of the index size) is stated per LOD, not per split blob. `MeshoptIndex` needs triangle lists, and meshopt's index codecs support 2- and 4-byte indices only, while §5.5 allows U8 | Each index blob has `elementSize` = index size, starts 16-B aligned (B16), and has `decodedSize` a multiple of 3 x index size for `MeshoptIndex` and of the index size otherwise. U8 indices only with codec `None` or `Zstd`. Other combinations are a cook error (K3xxx) and `ValidationFailed` on read. |
| B18 | §5.9 blob overlap and table order | Only decoded ranges are said not to overlap. Encoded ranges might overlap (dedupe?), and the order of `BLOB` entries is not defined (by `encodedOffset`, `decodedOffset` or `lodRank`?) | Encoded ranges must not overlap either (no dedupe in v0.x). The table is sorted by `encodedOffset` ascending, and `lodRank` is non-decreasing along it (invariant 6). Decoded order is free, and loaders must not assume it. Violations are `Corrupt`. |
| B19 | §4 `gpuDataSize` when compressed | "Encoded bytes in file" does not say whether it includes padding between blobs and at the tail, or whether it equals `GPUD`'s section size and `fileSize - gpuDataOffset` | `gpuDataSize == GPUD SectionEntry.size == fileSize - gpuDataOffset`, padding included. With `kPayloadRaw`, also `gpuDataSize == payloadDecodedSize`. The adapter always receives `payloadDecodedSize`, never `gpuDataSize`. |
| B20 | §2 and §5.9 raw fast path: `kPayloadRaw` and `sectionCount` / `BLOB` | "Ignores BLOB except for validation" leaves open whether `BLOB` may be omitted (changing `sectionCount`) and what "encoded layout == decoded layout" requires per blob | `BLOB` stays required and counted in `sectionCount` with or without the flag. With the flag, every blob has codec `None`, filter `None`, flags 0, `encodedOffset == decodedOffset` and `encodedSize == decodedSize`, and gaps in `GPUD` are zero in the file. The loader validates this (cheap) and fails with `Corrupt` otherwise. A file with all-`None` blobs but no flag is valid and goes through the decode loop. |
| B21 | §5.9 and §7 "decoders never write more than `decodedSize`" | Says nothing about writing less | Output must be exactly `decodedSize`. Short output is `Corrupt` (see `design/error-model.md`). |
| B22 | §5.9 `lodRank` across parts | "0 = coarsest LOD", but parts have different LOD counts, so the rank of a blob in a multi-part mesh is ambiguous | `lodRank = lodCount(part) - 1 - lodIndex`, so every part's coarsest LOD has rank 0, and a prefix read by rank gets a coarse LOD of every part. Capped at 255. |
| B23 | §5.9 `checksum`: "0 = not stored" | A real xxh32 can be 0 | Accept it. A 0 hash is treated as "not stored", which only skips a debug check (1 in 2^32). No format change. |
| B27 | §5.9 `checksum` uses xxh32 while all other hashes are FNV-1a 64 / xxh64 | A second hash function in core | Accepted: `kiln::xxh32` (constexpr, one-shot) was added to `hash.h` on 2026-09-26; the checksum stays xxh32 as specified. |
| B24 | §5.9 `PayloadBlob` offsets and sizes are `u32`; header sizes are `u64` | The limit is not stated | Max 4 GiB encoded and 4 GiB decoded per `.mesh`. The cooker fails above that (K4xxx writer check). Header fields stay `u64`. |
| B25 | §5.9 "unsupported codec or filter: placeholder plus diagnostic" | HANDOFF v2 has no mesh placeholder | The mesh goes to `Failed` with `Code::Unsupported` and one diagnostic; `is_ready()` stays false. Reword the spec sentence. |
| B26 | §4 version rule vs the v0.2 changes | v0.2 grows the header to 80 B and changes what `MeshLod` offsets mean, yet only `versionMinor` is bumped, and minor bumps are "additive, loader tolerates" | While `versionMajor == 0`, any minor bump may be breaking: the loader requires an exact `0.minor` match and reports `VersionMismatch` otherwise. The cooker version in the store key re-cooks old files. The additive rule applies from 1.0. |

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
| `Codec` `Zstd`, `MeshoptVertex`, `MeshoptIndex`, `MeshoptIndexSeq` (defined; v0.5 runtime rejects them) | mesh-format-spec §5.9 | payload compression | v0.6-0.7 |
| `Codec` 5..127 reserved, 128..255 vendor / experimental | mesh-format-spec §5.9 | new codecs | as needed |
| `Filter` 1..5 (defined; v0.5 runtime rejects them), 6..255 | mesh-format-spec §5.9 | new filters | v0.6+ |
| `BlobFlags` bits 1..15, `HeaderFlags` bits 1..31 | mesh-format-spec §4, §5.9 | new blob and header flags | as needed |
| `PayloadBlob._reserved`, `FileHeader._reserved` | mesh-format-spec §4, §5.9 | appended fields | as needed |
| Zstd decoder and meshopt decoder sources in `kiln_runtime` | dependencies | blob codecs | v0.6-0.7 |
| `Semantic::Joints`, `Semantic::Weights` | mesh-format-spec §5.3 | skinning | post-v1 |
| `TextureSlot` 5-15 | mesh-format-spec §5.7 | engine-defined slots | as needed |
