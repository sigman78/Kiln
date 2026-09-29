# Immutable cooked artifacts and a mapped catalog

**Status:** Proposed (2026-09-29), not implemented. The requested direction is hashed artifacts
and a binary manifest containing its own lookup index. Loading the manifest into memory is
acceptable initially; its representation must also support read-only memory mapping.
**Decides:** Artifact identity, pass-through KTX2, catalog layout and lookup, publication,
freshness, and explicit cleanup.
**Related:** [settings.md](settings.md), [asset-model-next.md](asset-model-next.md),
[hot-reload.md](hot-reload.md), [shipping-split.md](shipping-split.md),
[async-read-path.md](async-read-path.md).

## Motivation and current behavior

A named store file is used as long as it exists (open-questions R9): after a settings, target or
cooker change it is not re-cooked until someone deletes it. Deleting cooked files to force a
re-cook loses previous artifacts before replacements exist, file extensions do not establish
ownership, and sources may be unavailable.

Generated files already embed source/cook hashes, but the loader accepts existing named files
without consulting the provider. Pass-through KTX2 copies source bytes unchanged and need not
contain kiln metadata. The existing `kiln-cook --hashed` output has no runtime name catalog.

This proposal invalidates without deleting. Installing a provider does not remove files.
A cook publishes new immutable artifacts and then a catalog selecting them. Old artifacts remain
until an explicitly requested cleanup. Existing runtime behavior remains until this is implemented.

## Identity and keys

Three concepts remain distinct:

| Concept | Meaning |
|---|---|
| Logical asset name | Existing canonical `root:path.ext#sub` identity used by requests and mesh bindings |
| Build key | Digest of everything needed to reproduce one artifact |
| Output digest | Digest of the actual artifact bytes, used for integrity and detecting conflicting output |

Artifact filenames use the **build key**, not merely the output digest. This is an input-addressed
cache: a caller can calculate a key before cooking. Different keys may produce identical bytes;
deduplicating those bytes is deferred. Logical names and `AssetId` semantics do not change.

Proposed digest algorithm: SHA-256 for build keys, output digests, and catalog identities.
This is a new format, not a silent reinterpretation of the current 64-bit `store_key`.
The implementation/provider dependency for SHA-256 must be selected before implementation.
Existing embedded hashes remain useful diagnostics, but are not the catalog's authority.

The build-key serializer is versioned, field-by-field, little-endian, with domain tags and
length-prefixed variable fields. Never hash C++ struct memory or concatenate ambiguous strings.
Its inputs are:

- Key schema and cooker version; artifact kind and canonical logical name. Names matter because
  current cooked meshes can embed identities and references.
- Digests of the source bytes and every additional input actually consumed by this cook.
  For a glTF cook this includes external buffers it reads. Referenced textures cooked independently
  are separate assets; merely mentioning their URI does not require hashing their texels here.
- Resolved settings serialized directly, including effective fast-preview changes, and the target.
- Explicit policy version for behavior not represented in the resolved settings.
- For a generated subasset, its stable output selector and the owner input identity needed to
  reproduce it. Different embedded images must never alias merely because they share an owner.

Sidecars and name rules participate through resolved settings; any additional data consumed outside
settings must also enter the key. File times and sizes are acceleration hints, not content identity.
Absolute source directories do not enter the key unless their values actually affect output.

A configuration digest identifies a catalog's intended cooker/defaults/target/rules/policy
configuration. It is a selection and diagnostic field, not proof that every entry is fresh.
Source changes require per-asset checks. A configuration mismatch never deletes artifacts.

## Store layout and selection

Illustrative layout:

```text
store/
  artifacts/ab/<64-hex-build-key>.ktx2
  artifacts/7c/<64-hex-build-key>.mesh
  catalogs/<64-hex-catalog-digest>.kcat
  selections/desktop.current
  selections/mobile.current
```

The shard directory is the first two hex digits of the key. Extensions come from artifact kind.
Catalog entries contain keys, not arbitrary filesystem paths. The reader derives paths under the
configured store. Each catalog selects one artifact per `(logical name, kind)` for one configuration.
Different configurations may share artifacts while selecting independent catalogs.

A context selects either an immutable catalog explicitly (shipping/reproducibility) or a named
selection pointer (development). A pointer is a small versioned text record containing exactly one
catalog digest, not an arbitrary path. Its atomic replacement is the publication commit point.
Using separate immutable catalog files also avoids replacing a file still mapped by readers.

No directory search or source hashing is needed to resolve an asset in a shipping build.

## Binary catalog, draft format 0.1

The manifest is a serialized catalog: a header, fixed-size entries, a sorted on-disk index, and
a UTF-8 string table. All offsets are relative to the beginning of the file unless stated otherwise.
There are no pointers, compression, native container layouts, relocations, or runtime hash tables.
The exact draft major/minor must match; reserved fields and flags are zero in 0.1.

All integers are little-endian. Sections start at 8-byte boundaries; padding is zero. Digest fields
are 32 raw bytes, rendered as lowercase hex in filenames. Writers emit the sections below in order.
An implementation should decode integer fields safely rather than assuming arbitrary input buffers
have C++ object lifetime or alignment suitable for casting to structs.

### Header: 128 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `u8[4]` | Magic `KCAT` |
| 4 | `u16` | Major, 0 |
| 6 | `u16` | Minor, 1 |
| 8 | `u32` | Header bytes, 128 |
| 12 | `u32` | Flags, 0 |
| 16 | `u64` | Total file bytes |
| 24 | `u64` | Entry count, also index count |
| 32 | `u64` | Entry section offset |
| 40 | `u64` | Index section offset |
| 48 | `u64` | String section offset |
| 56 | `u64` | String section bytes |
| 64 | `u8[32]` | Configuration digest |
| 96 | `u8[32]` | Catalog digest |

Catalog digest is SHA-256 of the entire file with bytes 96..127 treated as zero. No timestamps,
absolute paths, or random generation numbers enter the file. Identical catalog contents produce
identical bytes and filenames. This digest is integrity metadata, not an authenticity signature.

### Entry: 96 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `u64` | Name offset relative to string section |
| 8 | `u32` | Name length in bytes, no terminator |
| 12 | `u16` | Kind: 1 mesh, 2 texture; other values rejected |
| 14 | `u16` | Flags, 0 |
| 16 | `u8[32]` | Build key |
| 48 | `u8[32]` | Output digest |
| 80 | `u64` | Artifact byte length |
| 88 | `u64` | Reserved, 0 |

Entries are ordered by canonical name bytes, then kind. Duplicate `(name, kind)` is an error.
Strings are emitted in entry order; adjacent entries with the same name reuse its string range.
Source paths, dependency lists and mutable stat hints belong in optional cook-side metadata,
not the shipping catalog. Loss of that metadata requires recomputation, never deletion.

### Embedded index: 16 bytes per entry

| Offset | Type | Field |
|---:|---|---|
| 0 | `u64` | Existing `hash_name(name)`: FNV-1a 64 over exact canonical UTF-8 bytes |
| 8 | `u64` | Entry ordinal |

Index rows are sorted by `(name hash, entry ordinal)`. Every entry occurs exactly once.
Lookup binary-searches the hash, examines its equal range, then compares kind and full name bytes.
Hash equality alone never identifies an asset. Expected work is `O(log N)` plus collision candidates;
pathological hash collisions can make the candidate scan linear. The index is deliberately a compact
sorted array rather than a serialized implementation-specific hash table.

### Validation and memory ownership

`CatalogView` conceptually borrows a read-only byte span. Lookup allocates nothing. The first
implementation may read the entire file into one owned buffer; a future IO backend may supply a
mapping with the same view. No deserialization into a second catalog or rebuilding of its index.
Mapping avoids an eager full-file copy, but touched pages still require IO.

Opening checks version, total size, section alignment, non-overlap, and overflow-safe count/range
arithmetic against the actual file size. Every dereference also requires validated bounds.
Full validation checks ordering, unique names, valid names/kinds, index permutation and hashes,
reserved fields, string ranges, and catalog digest. It can use temporary validation workspace;
it must not retain a runtime lookup index. Writers and untrusted catalog readers perform full
validation. An eventual trusted-package mode may defer the full scan/digest check to preserve lazy
paging; that trust mode must be explicit and must retain bounds checks. Initially validate fully.

Views and entry references remain valid only while their owning buffer/mapping remains alive.
Each load job pins the catalog snapshot used to resolve its artifact. Publication swaps the active
snapshot for future jobs; old snapshots are released only after their last reader finishes.

## Pass-through KTX2

1. Resolve settings and compute the expected build key from the input and target.
2. Reuse an existing verified artifact for that key, or validate the source using the pass-through
   rules (format support, dimensions, shape, layers and complete level data).
3. If validation succeeds, publish the source bytes unchanged under the build-key filename.
4. Record build key, output digest and byte length in the catalog.

No KTX2 key/value injection or metadata rewriting is required. If a changed target is incompatible,
the cook fails without changing the active entry. Two different size caps may produce different
build keys containing identical KTX2 bytes; this is allowed. Embedded metadata is not required for
freshness of any catalog-backed artifact.

## Development freshness and cooking

The provider must participate for catalog hits as well as misses. Merely adding a manifest while
keeping the existing cook-only-on-file-miss hook would retain the present freshness bug.
The runtime delegates preparation/freshness to the installed provider; knowledge of sources,
settings and policies stays in `kiln_cook`. Final callback/API names are an implementation decision.

On first request in a provider session, resolve the asset's inputs/settings and compare the expected
build key with the selected entry. A match reuses the artifact and registers source watching even
though no cook occurred. A different key selects an already verified cached artifact or cooks one.
Subsequent requests can use session freshness records; source/sidecar changes invalidate them.
Changes made while the application was closed must also be detected at first use.

If sources or settings cannot be resolved, the provider reports an unverifiable/stale result; it
does not silently claim freshness. A caller may explicitly allow using the selected old artifact.
A runtime without a provider simply treats the selected catalog as authoritative.

A newly selected configuration starts with an empty or fully verified catalog for that configuration;
do not relabel an old catalog with a new configuration digest. Same-configuration development catalogs
may contain not-yet-rechecked entries, so they are not sufficient evidence of a fresh shipping build.
Shipping publication verifies/cooks the entire requested asset set before publishing its catalog.

## Publication, failures and concurrency

1. Build and validate all outputs for one cook transaction in temporary files.
2. Publish immutable artifacts using atomic create-if-absent semantics. If a key already exists,
   verify its length/digest. A conflicting result is corruption, a collision, or nondeterminism;
   report it and never overwrite the artifact. The current overwrite-capable store writer needs
   a separate immutable-publication path.
3. Merge changed entries into the latest selected snapshot and write/validate a new catalog.
4. Publish its immutable catalog file, then atomically replace the selection pointer.

An owner mesh and its generated embedded textures commit together. Outputs that the new cook no
longer emits disappear from the new catalog, but their old files remain. Independently referenced
textures keep their own entries and are not implicitly cooked by this transaction.

Cooking may run concurrently, but one publisher serializes catalog updates for a selection. Initial
scope: an exclusive writer lock across processes for that selection; acquire it before reading the
latest snapshot and hold it through the pointer update. Merge by affected owner/output set so
parallel cooks do not drop unrelated changes. Recheck that inputs have not changed before commit;
discard/retry obsolete work. Different selections may share immutable artifact publication safely.

A failed cook leaves the old entry and files intact and reports failure to the requesting caller.
Already loaded GPU assets may remain live under existing reload semantics. Serving the old artifact
to a new request requires the caller's explicit stale fallback policy.
Failure before pointer replacement leaves at worst unreachable immutable files. Readers never see
a pointer to an incomplete catalog. Atomic publication assumes local same-filesystem rename/create
primitives; network filesystems require a supported equivalent. Power-loss durability additionally
requires platform flush/directory-sync ordering and is separate from process-crash atomicity.

## Hot reload and cleanup

The runtime watches the selection pointer, not immutable artifact modification times. After loading
and validating a replacement catalog it compares keys for requested assets and schedules reloads
for changed entries. Preserve existing handles and load through a pinned snapshot. An entry removed
from the catalog is unavailable to new resolution; already loaded objects follow existing lifetime
rules rather than being destroyed as a side effect of publication. Group-wide atomic GPU swaps are
outside this proposal.

Cleanup is a separate explicit operation with a dry-run option. Its roots are all retained catalog
snapshots, including every configuration/selection and user-pinned release. Keeping all catalog
files therefore retains all referenced artifacts; retiring snapshots is an explicit prerequisite
to reclaiming their artifacts. Only recognized files in the managed artifact/catalog namespaces
are eligible. Never scan arbitrary `.mesh` or `.ktx2` files and infer ownership from extensions.

Initially cleanup requires applications/readers and writers using the store to be stopped. A catalog
may have readers after its pointer changes, and a published artifact may not yet be catalog-reachable.
Online collection would need reader leases and writer coordination; it is deferred.

## Migration and implementation sequence

1. Implement the catalog writer/view, deterministic binary index and validation over loaded buffers.
2. Add immutable artifact publication and explicit catalog selection to the CLI/runtime. The new
   catalog mode is distinct from today's `--hashed` files without a manifest.
3. Add provider preparation on hits, input tracking, transaction publication and catalog hot reload.
4. Switch examples/default development flow.
5. Add explicit migration, snapshot retirement and offline cleanup tools; mapping can follow later.

Keep legacy named-store mode explicitly selectable during migration. Do not silently fall back to
named files on a catalog miss: that would hide missing entries and bypass version selection.
Migration re-cooks from available sources, or explicitly imports precooked files as opaque artifacts
with a distinct import-key domain based on bytes/kind/name. Imported files are usable by shipping
readers but have unknown build provenance; a provider must not mistake them for verified cook hits.
Migration never deletes the old store.

When implemented, update R4/R9, settings, architecture, IO, hot-reload and shipping documentation.
This note is a proposal and does not redefine their descriptions of current code yet.

## Acceptance checks

- Unchanged requests reuse artifacts, including after restart; source/sidecar/settings/target/cooker
  changes select appropriate new keys without deleting old files.
- Pass-through KTX2 is byte-identical to the source and needs no kiln metadata.
- Multiple configurations coexist; switching back reuses retained artifacts.
- Real name-hash collisions resolve by full name/kind; malformed offsets, integer overflow,
  duplicate entries, invalid index rows and unsupported versions are rejected safely.
- Catalog bytes are deterministic; loaded-buffer and mapped views return identical lookup results
  without constructing runtime indexes.
- Failure at every publication step preserves the previous selection; parallel cooks preserve
  unrelated updates; conflicting bytes for an existing build key are reported.
- Mesh/subasset publication is atomic at catalog level, removed generated outputs leave no stale
  bindings in the new catalog, and load jobs safely retain old snapshots during replacement.
- Shipping loads without source access; legacy migration and explicit cleanup preserve unowned files.

## Open implementation choices

- SHA-256 implementation/dependency and hashing performance; changing the proposed digest algorithm
  or widths requires settling the wire format before its first release.
- Public preparation callback and configuration/selection API names, plus limits/budgets for catalog
  size and validation workspace.
- Persistence format for optional cook-side input/stat records; correctness must not depend on it.
- Platform mapping support and Windows file-sharing rules for selection-pointer replacement.

