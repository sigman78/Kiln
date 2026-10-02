# Cooked artifacts and a store manifest

**Status:** Decided (owner, 2026-09-29, revised 2026-09-30); phases A and B shipped in v0.6 (choices
made on the way in open-questions R11; phase C set aside). The owner asked for hashed
artifacts and a binary index with its own lookup, then chose: XXH3-128 keys, no source re-hashing
for freshness, an index rewritten in place (not a new file per change), and a single writer. On
2026-09-30 the owner removed the named layout (no backward compatibility), flattened the store,
and chose one manifest shared by every profile, input records without paths in a file of their
own, one lock per store, artifacts named by the key in base32 with no extension and no
subdirectories, and hashing a file before a new time counts as a change. Then (2026-09-30) a root
table in the input records, so `kiln-cook` can run without inputs and scans the recorded roots by
default.
**Decides:** How cooked files are named, how the runtime finds them, how a dev cook knows a file is
still fresh, and how the store is written and cleaned. Replaces the named store's "a file is used
while it exists" rule (open-questions R9).
**Related:** [target-profiles.md](target-profiles.md), [settings.md](settings.md),
[hot-reload.md](hot-reload.md), [shipping-split.md](shipping-split.md),
[asset-model-next.md](asset-model-next.md).

## Summary

- A cooked file (an **artifact**) is named by its **build key**: an XXH3-128 hash of everything that
  produced it: cooker version, asset kind and name, target profile, resolved settings, and the
  content of every input file the cook read. A change to any of them gives a new name, so a stale
  file is never picked by accident, and nothing is deleted to force a re-cook.
- Artifacts are immutable. One **manifest** per store maps, for each target profile, each asset name
  to its artifact. It is a small binary file with a sorted index, read into memory; the runtime
  looks names up in it without building a table.
- The cook keeps its **input records** (what each cook read) in a second file that never ships.
- Both files are rewritten in place (a temporary file, then a rename) when a cook publishes.
- In dev, each asset is checked once per session by the size and modification time of its recorded
  inputs. A file whose size or time differs is hashed; only a content change cooks.
- One process writes a store at a time (an OS file lock, fail fast). Hot reload batches its
  re-cooks into one write.
- Cleanup is an explicit, offline command that deletes artifacts no profile references.

## The store

```
<store>/manifest.dir     the runtime reads it: profiles, entries, index (ships)
<store>/manifest.in      the root table and the input records of every profile (cook only, never ships)
<store>/manifest.lock    held by the one process that writes the store
<store>/2x3ukt7xoax4fpn5ueaazr6h4i       an artifact: the build key in base32, no extension
```

- The two manifest files are shared by every profile: several profiles cook into one store, and
  each profile's entries sit next to the others'. A context reads its profile's entries.
- Shipping (and the phase B export) takes `manifest.dir` and the artifacts. It never carries
  build data: that is why the input records are a file of their own.
- An artifact's name is the 128-bit build key in base32 (RFC 4648 alphabet in lower case, `a-z2-7`,
  no padding): 26 characters, in the store's root. The key already covers the asset's kind, so a
  mesh and a texture never share a name; the manifest gives the kind, and `kiln-info` recognizes a
  file by its first bytes. Keys are printed in hex everywhere else (`kiln-info`, `--map`,
  diagnostics).
- The store is flat, with no fan-out directories. A store stays under about 1000 artifacts (packed
  storage replaces a file per artifact beyond that), and NTFS, ext4 and btrfs index their
  directories, so one directory of a few thousand files is the cheapest layout; splitting pays only
  past about 10 000 files per directory.

## Decision

### 1. Artifacts are named by their build key

The build key is XXH3-128 over a field-by-field, versioned serialization (little-endian, a tag per
field, lengths before variable fields; never struct memory):

- key schema, `kCookerVersion`, asset kind, canonical asset name (a cooked mesh embeds names);
- the target profile (`hash_target`) and the resolved settings (`hash_settings`);
- for each input file the cook read, in a fixed order: its role and name, then the XXH3-128 of its
  content. Inputs are the source, its sidecar (or its absence), and a `.gltf`'s external buffers.
  A texture that a mesh only references by URI is its own asset, not an input;
- for an embedded image (`mesh.glb#image`), the image's name within its owner.

Sidecars and name rules act through the resolved settings, and their files are inputs too. File
times and directories never enter the key. XXH3-128 comes from the xxhash that zstd already vendors
(XXH3 is compiled in for this); it is a checksum, not a signature.

Artifacts are written once (temporary file, then rename) and never changed. The same key means the
same bytes; if a new cook produces other bytes for an existing key, the cooker is not deterministic,
and that is reported (K3010), never overwritten. A pass-through KTX2 source is published unchanged
under its build key.

### 2. One manifest for every profile

`manifest.dir` maps, per target profile, `(asset name, kind)` to a build key. Each profile records
its name, `hash_target` and block formats, so `create()` checks the adapter against the context's
profile once (K5018). Two profiles never share an artifact: the target is part of the key.

### 3. The runtime

- `ContextDesc::profile` selects the profile. `create()` reads `manifest.dir` into one buffer and
  validates it fully. A missing manifest is no error (a provider or `kiln-cook` may write it later);
  a manifest without the profile reads the same way.
- A request looks the name up (a binary search of the profile's index, no allocation). The path of
  the artifact comes from the key. The loader takes the key on the pump thread when it dispatches
  the job, so a later manifest swap does not affect jobs in flight.
- A name the profile lacks goes to the cook provider; without a provider it fails with K5001, or
  K5019 when there is no manifest or no such profile.
- Hot reload watches `manifest.dir`. When it changes, the runtime loads it, swaps it in on the pump
  thread, and reloads each loaded asset whose key changed; a load in flight is compared when it
  settles. An asset that left the manifest stays loaded; new requests for it miss.

### 4. Freshness in dev: size and time first, then content

The provider takes part on **hits** too: each load asks it to **prepare** the asset
(`CookProvider::prepare`); after the first check in a session it answers from memory. A reload the
host asks for (`request_reload`, for example from its own file watcher) passes
`PrepareMode::Recheck`: the provider checks the sources again (steps 1 to 4), whatever it checked
earlier. A reload after a manifest change does not: the store's writer checked the sources.

The **input records** (`manifest.in`) keep, for each unit (a source and the outputs its cook made):
the role, name, size, modification time and content hash of every input, the outputs with the
build key each one got, and the host-settings digest. They keep **no paths**: an input's file is found from the unit's source as it
is found now: the source itself, `<source>.kiln`, or a buffer URI relative to the source's
directory. So a store stays valid when the sources move (another checkout, a renamed root
directory): only the names count.

On prepare:

1. **Every input has its recorded size and time, and the host settings are unchanged**: the entry
   is fresh. Nothing is read.
2. **A size or time differs**: that file is read and hashed. The same content is no change: the
   record takes the new size and time, and nothing cooks (a checkout or a save without edits).
3. **Host settings changed**: resolve the settings again (a sidecar read at most), rebuild the keys
   from the recorded input hashes, and keep the entries if the keys are the same; else re-cook.
4. **A content changed**, a file appeared or went missing, or there is no record: re-cook. The new
   record replaces the old one.

The **host-settings digest** hashes what the host sets for the whole provider: the cooker version,
the target, the default texture and mesh settings, the name rules, `fastPreview` and
`ProviderDesc::policyVersion` (a `CookPolicy` is a function, so the host bumps this number when
its choices change). It never deletes anything; it only tells step 3 to recompute keys. When a
record another cooker version wrote cooks again, the writer logs one warning per session: two
builds of kiln that share a store undo each other's cooks (seen 2026-09-30 with a stale example
binary).

The cost: an edit that keeps both size and modification time is not seen. `kiln-cook --verify`
hashes every input for CI and shipping builds. A lost or damaged `manifest.in` only costs time:
the inputs are checked again and the records rebuilt. Without a provider (shipping), the manifest
is trusted as it is.

### 5. Writing: one writer, batched

- A writer (`install_provider` in Disk mode, `kiln-cook`) holds an OS file lock on
  `manifest.lock` for its lifetime. A second writer fails at once with K3009; the lock ends with
  the process, so a crash leaves nothing to clean.
- A writer edits its own profile and keeps every other profile's entries and records as it read
  them. When the manifest holds its profile cooked for another definition of it (another
  `hash_target`), those entries and records are dropped; the other profiles stay.
- A cook first publishes its artifacts, then writes `manifest.dir`, then `manifest.in`. A crash in
  between leaves unreferenced artifacts, or records of another cook than the manifest's entries.
  Each record keeps its outputs' build keys, and a record whose keys differ from the entries is
  dropped when the store opens: its unit cooks again, since its fingerprints may describe other
  sources than the entries (the owner's audit of PR #3).
- The entries of a unit are the unit's own name and `<unit>#<image>`. A cook removes the unit's
  entries it did not make (a glb that lost an image), from the names alone, so a lost record
  cannot leave them behind.
- A mesh and its embedded images enter the manifest together.
- The provider's source poller re-cooks all changed units of one poll round, then writes once. A
  cook on a request writes at most once a second; the poller and the provider's release write the
  rest. `kiln-cook` writes at most once a second while it cooks, and at the end.

### 6. Cleanup

`kiln-cook --gc -o <store>` (with `--dry-run`, which lists and deletes nothing) takes the store's
lock (K3009 while a writer runs), reads `manifest.dir`, and deletes the artifacts no profile
references. Artifacts have no directory of their own, so it deletes only files in the store's root
whose names are exactly 26 base32 characters, and the temporary files a store write leaves after a
crash (`<name>.tmp.<16 hex digits>`); it never touches other files. A store without a manifest is
refused: a lost manifest must not empty the store. Retiring a profile is dropping it from the manifest, then running `--gc`.

### 7. Roots, and runs without inputs

`manifest.in` also holds a **root table**: for each root a writer used, its name (empty for the
default root) and its directory. The directory is stored relative to the store when there is a
relative path (the same drive), else absolute, so a project that moves as a whole keeps its table.
Every writer records its roots: `kiln-cook` its `--root` entries and its default root (`--root
<dir>`, or the one directory its inputs imply; inputs in different directories record none), the
cook provider the context's roots. A writer's entries replace those of the same name.

`kiln-cook -o <store>` with no inputs scans every recorded root, as a first run scans its inputs:
new and changed sources cook, unchanged ones are checked by size and time. `--watch` works the same
way. Given `--root` entries override recorded ones of the same name. A store with no table needs
inputs once. This matches a provider, which cooks any name a request gives.

A run drops the units whose source lay under a directory it scanned and is gone: their entries and
records leave the manifest, and `--gc` deletes their artifacts later. A directory counts as scanned
only when it exists and was listed in full, and a source counts as gone only when looking it up says
"not found" (not another IO error). A missing or unreadable root drops nothing, and the run exits 2
after cooking the rest. Units of roots a run did not scan stay.

### 8. Export

`kiln-cook --export <dir> -o <store> [--target <profile>]` (any profile of the store, not only a
built-in one: `--target` accepts any valid profile name, and only cooking needs a built-in one)
writes a runtime-only store into an
empty (or new) `<dir>`: a `manifest.dir` with that profile (every profile without `--target`) and
the artifacts it names, each checked against its checksum while it is copied. No input records and
no lock. The manifest is written last, so a failed export never names a missing artifact. The
export only reads the store and takes no lock; a `--gc` at the same time can make it fail, never
make it wrong.

## Manifest format 0.1 (`manifest.dir`)

Little-endian. Sections start at 8-byte boundaries; padding is zero. The exact major and minor
must match; reserved fields are zero. Readers decode fields from the bytes (no struct casts) and
check every count, offset and size against the file size before using it.

**Header, 128 bytes:**

| Offset | Type | Field |
|---:|---|---|
| 0 | `u8[4]` | magic `KMAN` |
| 4 | `u16` | major, 0 |
| 6 | `u16` | minor, 1 |
| 8 | `u32` | header bytes, 128 |
| 12 | `u32` | flags, 0 |
| 16 | `u64` | total file bytes |
| 24 | `u32` | profile count |
| 28 | `u32` | reserved |
| 32 | `u64` | entry count (also the index count) |
| 40 | `u64` | profile section offset (128) |
| 48 | `u64` | entry section offset |
| 56 | `u64` | index section offset |
| 64 | `u64` | string section offset |
| 72 | `u64` | string section bytes |
| 80 | `u8[16]` | checksum: XXH3-128 of the file with these 16 bytes as zero |
| 96 | `u8[32]` | reserved |

The sections follow each other in this order, with no gaps: profiles, entries, index, strings,
then padding to 8 bytes.

**Profile, 40 bytes**, sorted by name bytes, names unique and valid (`[a-z0-9_-]`, 1 to 63):

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | name offset, in the string section |
| 4 | `u32` | name bytes |
| 8 | `u64` | `hash_target` |
| 16 | `u64` | block formats (`block_format_bit()` set) |
| 24 | `u64` | first entry |
| 32 | `u64` | entry count |

The profiles' entry ranges follow each other from entry 0 and cover every entry.

**Entry, 56 bytes**, within a profile sorted by name bytes, then kind; a duplicate `(name, kind)`
in one profile is an error:

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

**Index, 16 bytes per entry**, over the same ranges as the entries; within a profile sorted by
`(name hash, entry number)`, every entry of the profile once: `u64 hash_name(name)` (the existing
FNV-1a 64), `u64 entry number`. A lookup binary-searches the hash, then compares kind and the full
name for each candidate; a hash alone never identifies an asset.

No timestamps or paths: the same contents give the same bytes, whatever order they were added in.
Size: about 1.1 MB for 10 000 entries (72 bytes per entry, plus names).

## Input records (`manifest.in`)

The cook's own file, never read by the runtime: magic `KMIN`, major 0, minor 5, a record count, a
root count, the roots (name, directory), the last writer of each profile (minor 5: a count, then the
profile name, the writer kind (`kiln-cook` or a cook provider) and five u64 digests of its settings:
target, host defaults, name rules, policy version, project file), then each record as its profile name and its body (unit
name, kind, the `kCookerVersion` that wrote it in one byte (0 in files from before 2026-10-01, whose
readers skip the byte), host digest, the inputs with role, presence, name, size, time and content hash, the
outputs with kind, glTF slot, the Mask material's alpha cutoff (f32 bits; minor 4), name and build
key, zero for an output that failed), then
an XXH3-128 of everything before it. Records are sorted by profile name, then unit name. A file
that does not decode is dropped with an info log; minor 4 still reads, without writers.

When a record cooks again because its keys changed, and the profile's last writer was the other kind
of program with other settings, the writer warns once per session and names the settings that differ
(open-questions R19): `kiln-cook` and a provider that disagree undo each other's cooks. A provider's
quality cap is left out, since `kiln-cook` upgrading capped entries is intended.

## Diagnostics

| Code | When |
|---|---|
| K3009 | another process writes the store (`manifest.lock`) |
| K3010 | a new cook produced other bytes for an existing build key (a nondeterministic cook) |
| K4201-K4209 | malformed manifest: magic, version, sizes and offsets, reserved bytes, names, order, duplicates, index, checksum |
| K5018 | (exists) the context's profile has formats the adapter cannot sample |
| K5019 | a request missed and the store has no manifest, or no entries for the profile |

## Alternatives considered

- **A file per profile** (the first implementation): `catalogs/<profile>.kcat` and
  `inputs/<profile>.kin`, a lock per profile. Two profiles could be cooked at once, but a store
  had three directories of small files, and the owner preferred one flat store.
- **The input records inside the manifest:** one file fewer, but shipping would carry build data,
  and a records-only update (a new file time) would rewrite the file every running app watches.
- **Paths in the records:** needs no source lookup, but a store stops matching when the sources
  move, so a new checkout or another machine cooks everything again.
- **A new immutable index file per change, with selection pointers** (the first draft of this
  note): every hot-reload re-cook wrote a whole new file, the files piled up, and a retention rule
  was needed. Reading the manifest into memory makes a plain rename safe.
- **Hashing every source on first use:** correct even when a file keeps its size and time, but it
  reads every source of a project on each dev start. Size and time first, then the content of
  what differs, is what build tools do; `--verify` covers CI.
- **SHA-256 keys:** would be kiln's only cryptographic code; the keys need collision resistance, not
  security. XXH3-128 is in a vendored header and runs at about 30 GB/s.
- **Hex artifact names with an extension under `artifacts/<2 hex>/`:** longer names, and
  directories that only group; the kind is in the key and the manifest, and a flat directory is
  cheaper at the store's size (see "The store").
- **A store stamp that deletes stale files** (reverted in `8fc83fa`): deletion loses work before its
  replacement exists.
- **Several writers merging their changes:** not needed while one process cooks; the lock makes a
  second writer fail clearly.

## Rollout

**Phase A (v0.6): fixes R9.**
1. XXH3-128 from the vendored xxhash; the build-key serializer; the cooker reports every input file
   it reads.
2. The manifest writer and reader, with full validation and a fuzz target.
3. Immutable artifact publication, the writer lock, the manifest rewrite, input records.
4. The runtime: `ContextDesc::profile`, lookup, K5019, hot reload of the manifest.
5. The provider's prepare step on hits, with the size-and-time check and batched publication.
6. `kiln-cook` writes the store; `--verify`; `--watch`.
7. The examples and the viewer use it.

**Phase B (done):** `kiln-cook --gc` and `--dry-run`; `kiln-cook --export`, which copies
`manifest.dir` (one profile or all) and the artifacts it names, for shipping. With them, the root
table and runs without inputs (decision 7).

**Phase C, if needed (set aside, owner 2026-09-30):** a memory-mapped manifest through the IO backend; several writers.

The named layout was removed after phase A (owner, 2026-09-30). A store from before is cooked again
into a new directory; kiln deletes no store.

## Open points

1. ~~Record format~~: `manifest.in`, binary, with a checksum (above).
2. ~~The prepare callback~~: `CookProvider::prepare` (R11). Accepting an unverified entry when the
   provider finds no source is not supported (owner, 2026-09-30); add an option when a host needs it.
3. Whether `create()` should also accept a manifest file path directly (a shipping package that is
   not a store directory). Open until pack files or a split package need it: any copy of a store
   directory loads, since the runtime reads only `manifest.dir` and the artifacts.
