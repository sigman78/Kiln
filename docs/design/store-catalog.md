# Cooked artifacts and a store catalog

**Status:** Decided (owner, 2026-09-29); the named layout removed (owner, 2026-09-30, no
backward compatibility); phase A implemented (branch `store-catalog`, choices made
on the way in open-questions R11). The owner asked for hashed artifacts and a
binary catalog with its own lookup index, then chose: XXH3-128 keys, no source re-hashing for
freshness, one catalog per profile rewritten in place (not a new file per change), and a single
writer. Phase A (below) is the last step of v0.6.
**Decides:** How cooked files are named, how the runtime finds them, how a dev cook knows a file is
still fresh, how the store is written and cleaned, and how this replaces the named store's
"a file is used while it exists" rule (open-questions R9).
**Related:** [target-profiles.md](target-profiles.md), [settings.md](settings.md),
[hot-reload.md](hot-reload.md), [shipping-split.md](shipping-split.md),
[asset-model-next.md](asset-model-next.md).

## Summary

- A cooked file (an **artifact**) is named by its **build key**: an XXH3-128 hash of everything that
  produced it: cooker version, asset kind and name, target profile, resolved settings, and the
  content of every input file the cook read. A change to any of them gives a new name. So a
  stale file is never picked by accident, and nothing is deleted to force a re-cook.
- Artifacts are immutable. A **catalog** per target profile maps each asset name to its artifact.
  It is a small binary file with a sorted index, read into memory; the runtime looks names up in
  it without building a table.
- The catalog file is rewritten in place (a temporary file, then a rename) when a cook publishes.
  Old catalogs are not kept, so they do not pile up.
- In dev, the cook provider checks each asset once per session. It compares the size and
  modification time of the asset's recorded inputs; it hashes nothing when they match. A mismatch
  re-cooks the asset, which reads and hashes the changed input anyway.
- One process writes a store's catalog at a time (an OS file lock, fail fast). Hot reload batches
  its re-cooks into one catalog write.
- Cleanup is an explicit, offline command that deletes artifacts no catalog references.

## What exists

- The **named** store layout: `<store>/<asset name>.mesh|.ktx2`. The runtime loads such a file as
  long as it exists (R9). Cook-on-miss runs only when the file is missing. `kiln-store.txt` records
  the store's target profile (`target-profiles.md`), so a cook with another profile refuses the
  store; within one profile, a changed sidecar, host setting or cooker still goes unnoticed.
- `kiln-cook --hashed`: files named by a 64-bit key (`store_key`), with no catalog, so the runtime
  cannot find them by name.
- The provider keeps one source record per cooked asset (path, size, modification time) to watch it
  for hot reload. It does not know the other files a cook reads (a `.gltf`'s buffers, a sidecar).
- Cooked files carry `sourceHash` and `cookHash` (in the `.mesh` header, and as KTX2 keys); a
  pass-through KTX2 source carries nothing of kiln's.

## Decision

### 1. Artifacts are named by their build key

The build key is XXH3-128 over a field-by-field, versioned serialization (little-endian, a tag per
field, lengths before variable fields; never struct memory):

- key schema, `kCookerVersion`, asset kind, canonical asset name (a cooked mesh embeds names);
- the target profile (`hash_target`) and the resolved settings (`hash_settings`);
- for each input file the cook read, in a fixed order: its role and name, then the XXH3-128 of its
  content. Inputs are the source, its sidecar, a `.gltf`'s external buffers, and any other file
  the cooker opens. A texture that a mesh only references by URI is its own asset, not an input;
- for an embedded image (`mesh.glb#image`), the image's name within its owner.

Sidecars and name rules act through the resolved settings, and their files are inputs too. File
times and absolute directories never enter the key. XXH3-128 comes from the xxhash that zstd
already vendors (XXH3 is compiled in for this); it is a checksum, not a signature.

Artifacts live under the store as `artifacts/<first 2 hex digits>/<32 hex digits>.mesh|.ktx2`. They
are written once (temporary file, then rename) and never changed. The same key means the same bytes;
if a new cook produces other bytes for an existing key, the cooker is not deterministic, and that is
reported (K3010), never overwritten.

A pass-through KTX2 source is published unchanged under its build key; it needs no kiln metadata.

### 2. One catalog per target profile

`<store>/catalogs/<profile>.kcat` maps `(asset name, kind)` to a build key. A store may hold
catalogs of several profiles: each profile's keys differ, so their artifacts never mix, and
two profiles may share an artifact whose key is the same.

The catalog records the profile's name, `hash_target` and block formats, so `create()` checks the
adapter against it once (K5018).

The catalog is written as a new temporary file and renamed over the old one. Readers load it into
memory, so a rename never disturbs them. Nothing keeps old catalogs.

### 3. The runtime

- `ContextDesc::profile` (a name) selects `catalogs/<profile>.kcat`. `create()` reads it into one buffer and validates it fully.
- A request looks the name up (a binary search of the index, no allocation). The path of the
  artifact comes from the key. The loader resolves the path on the pump thread when it dispatches
  the job and copies it into the job, so a later catalog swap does not affect jobs in flight.
- A name missing from the catalog goes to the cook provider, as a store miss does today; without
  a provider it fails with K5001. There is no fallback to named files.
- Hot reload watches the catalog file. When it changes, the runtime loads the new catalog, swaps it
  in on the pump thread, and reloads each loaded asset whose key changed. An asset that left the
  catalog stays loaded under the existing lifetime rules; new requests for it miss.

### 4. Freshness in dev: size and modification time, no re-hashing

The provider now takes part on catalog **hits** too, not only on misses: the first request of an
asset in a session asks the provider to **prepare** it. (The name of the new `CookProvider`
callback is decided at implementation.)

The provider keeps **input records** in `<store>/inputs/<profile>.kin`: for each catalog entry, the
path, size, modification time and content hash of every input its cook read, and the host-settings
digest (below) it was cooked under. It is cook-side only and never ships.

On prepare:

1. **Inputs unchanged** (every recorded size and modification time matches) **and the host settings
   unchanged**: the entry is fresh. Nothing is read or hashed.
2. **Host settings changed**: resolve the settings again (a sidecar read at most), rebuild the key
   from the recorded input hashes, and keep the entry if the key is the same; else re-cook.
3. **An input changed**, or no record: re-cook. The cook reads the input and hashes it anyway, and
   the new record replaces the old one.

The **host-settings digest** hashes what the host sets for the whole provider: the default texture
and mesh settings, the name rules, `fastPreview` and `ProviderDesc::policyVersion` (a `CookPolicy`
is a function, so the host bumps this number when its choices change). It never deletes anything;
it only tells step 2 to recompute keys.

The cost: an edit that keeps both size and modification time is not seen. Editors and version
control always change the time. `kiln-cook --verify` re-hashes every input for CI and shipping
builds.

A lost input-record file only costs time: prepare then re-hashes the inputs once and rebuilds it.
Without a provider (shipping), the catalog is trusted as it is.

### 5. Writing: one writer, batched

- A writer (`install_provider` in Disk mode, `kiln-cook`) holds an OS file lock on
  `<store>/catalogs/<profile>.lock` for its lifetime. A second writer fails at once with K3009; the
  lock ends with the process, so a crash leaves nothing to clean.
- A cook first publishes its artifacts, then writes the new catalog and input records. A crash
  before the catalog rename leaves only unreferenced artifacts; the old catalog stays valid.
- A mesh and its embedded images enter the catalog together.
- The provider's source poller re-cooks all changed assets of one poll round, then writes the
  catalog once.

### 6. Cleanup

`kiln-cook --gc <store>` (with `--dry-run`) takes every catalog's lock, reads all catalogs, and
deletes the files under `artifacts/` that none references. It never touches other files. It runs
while no program uses the store. Retiring a profile is deleting its catalog, then running `--gc`.

## Binary catalog, format 0.1

Little-endian. Sections start at 8-byte boundaries; padding is zero. The exact major and minor
must match; reserved fields are zero. Readers decode fields from the bytes (no struct casts) and
check every offset and size against the file size.

**Header, 128 bytes:**

| Offset | Type | Field |
|---:|---|---|
| 0 | `u8[4]` | magic `KCAT` |
| 4 | `u16` | major, 0 |
| 6 | `u16` | minor, 1 |
| 8 | `u32` | header bytes, 128 |
| 12 | `u32` | flags, 0 |
| 16 | `u64` | total file bytes |
| 24 | `u64` | entry count (also the index count) |
| 32 | `u64` | entry section offset |
| 40 | `u64` | index section offset |
| 48 | `u64` | string section offset |
| 56 | `u64` | string section bytes |
| 64 | `u64` | profile hash (`hash_target`) |
| 72 | `u64` | profile block formats (`block_format_bit()` set) |
| 80 | `u32` | profile name offset, in the string section |
| 84 | `u32` | profile name bytes |
| 88 | `u8[16]` | catalog checksum: XXH3-128 of the file with these 16 bytes as zero |
| 104 | `u8[24]` | reserved |

**Entry, 56 bytes**, sorted by name bytes, then kind; a duplicate `(name, kind)` is an error:

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | name offset, in the string section |
| 4 | `u32` | name bytes (no terminator) |
| 8 | `u16` | kind: 1 mesh, 2 texture |
| 10 | `u16` | flags, 0 |
| 12 | `u32` | reserved |
| 16 | `u8[16]` | build key |
| 32 | `u8[16]` | output checksum: XXH3-128 of the artifact's bytes |
| 48 | `u64` | artifact bytes |

**Index, 16 bytes per entry**, sorted by `(name hash, entry number)`, every entry once:
`u64 hash_name(name)` (the existing FNV-1a 64), `u64 entry number`. A lookup binary-searches the
hash, then compares kind and the full name for each candidate; a hash alone never identifies an
asset.

No timestamps or absolute paths: the same contents give the same bytes. Size: about 1.1 MB for
10 000 assets (72 bytes per entry, plus names).

## Diagnostics

| Code | When |
|---|---|
| K3009 | another writer holds the catalog's lock |
| K3010 | a new cook produced other bytes for an existing build key (a nondeterministic cook) |
| K4201-K4209 | malformed catalog: magic, version, sizes and offsets, order, duplicates, index, checksum |
| K5018 | (exists) the catalog's profile has formats the adapter cannot sample |
| K5019 | a request missed and the profile's catalog is missing |

## Alternatives considered

- **A new immutable catalog file per change, with selection pointers** (the first draft of this
  note): lets readers memory-map a catalog while writers publish, and keeps every past state. But
  every hot-reload re-cook wrote a whole new catalog, the files piled up, and a retention rule was
  needed. Reading the catalog into memory makes a plain rename safe; memory mapping can bring
  versioned files back if it is ever needed.
- **Hashing every source on first use** to check freshness: correct even when a file keeps its
  size and time, but it reads every source of a project on each dev start. Size and time are what
  build tools use; `--verify` covers CI.
- **SHA-256 keys:** would be kiln's only cryptographic code; the keys need collision resistance, not
  security. XXH3-128 is in a vendored header and runs at about 30 GB/s.
- **Checking each named file's `cookHash` before use** (the interim R9 proposal): fixes staleness
  for kiln's own files, but needs every asset's settings resolved to know the expected hash, costs a
  header read per load, and does nothing for pass-through KTX2 or cleanup.
- **A store stamp that deletes stale files** (reverted in `8fc83fa`): deletion loses work before its
  replacement exists.
- **Several writers merging their catalog changes:** not needed while one process cooks; the lock
  makes a second writer fail clearly.

## Rollout

**Phase A (v0.6): fixes R9.**
1. XXH3-128 from the vendored xxhash; the build-key serializer; the cooker reports every input file
   it reads.
2. The catalog writer and reader (format 0.1), with full validation and a fuzz target.
3. Immutable artifact publication, the writer lock, catalog rewrite, input records.
4. The runtime: `StoreLayout::Catalog`, `ContextDesc::profile`, lookup, K5019, hot reload of the
   catalog.
5. The provider's prepare step on hits, with the size-and-time check and batched publication.
6. `kiln-cook` writes catalogs (the default for new stores); `--verify`; `--hashed` is retired.
7. The examples and the viewer use a catalog store.

**Phase B:** `kiln-cook --gc` and `--dry-run`; an export command that copies one profile's catalog
and its artifacts for shipping.

**Phase C, if needed:** memory-mapped catalogs through the IO backend; several writers.

The named layout was removed after phase A (owner, 2026-09-30): the catalog store is the only one.
A named store is cooked again into a new directory; kiln deletes no store.

## Open points

1. ~~Record format of `inputs/<profile>.kin`~~: binary, with a checksum (R11).
2. ~~The prepare callback~~: `CookProvider::prepare` (R11). Accepting an unverified entry when the
   provider finds no source is still open; such an entry is not used for now.
3. Whether `create()` should also accept a catalog file path directly (a shipping package that is
   not a store directory).
