# Handles, asset ids and states

**Status:** Proposed (awaiting owner sign-off). Implemented in M3 (`include/kiln/assets.h`,
`src/runtime/`), except hot reload (M5). Implementation details are recorded as R5a-R5l in
`../open-questions.md`.
**Decides:** Handle identity, asset ids, load states and transitions (including hot reload),
request refcounting, events, placeholders and load groups.

## Decision

### `Handle<Tag>` (`core.h`)

- `{ u32 index; u32 generation; }`. Generation 0 is null and never issued.
- `Tag` is a phantom type (`Mesh`, `Texture`, `GroupTag`), so a mesh handle cannot be passed where
  a texture handle is expected.
- `bits()` / `from_bits()` let hosts store handles in a `u64` (ECS components, events).
- The registry has one slot array for all assets and one id map per kind (`meshMap`, `texMap`,
  R5h). Each slot stores its generation. The generation increments when the asset is unloaded, so
  its handles go stale at once; 0 is skipped on wrap.

### Two counters

**Owner undecided (R2); implemented as proposed.**

| Counter | Where | Starts at | Changes when | Meaning |
|---|---|---|---|---|
| **Slot generation** | `Handle::generation` and the slot | 1 | unload | identity; fixed for the life of a loaded asset |
| **Content version** | the slot (`version()`, `TextureInfo.version`, events, `publish()`) | 1 | each successful hot reload swap (M5) | which payload the asset currently serves |

**Hot reload must not bump the handle generation.** The host stores handles in its own structures.
If a reload changed the generation, every stored handle would go stale after every edit. Hosts that
cache derived data compare the version or react to `Changed` events. `publish()`'s last parameter
and `Changed` events carry the content version. This departs from HANDOFF's "bumps the
generation" wording but keeps its intent.

### `AssetId`

- An asset name is `root:path/file.ext#sub`. The extension is part of the name; the runtime does
  not normalize names (no stripped extension, no `./` removal, no slash collapsing). A name is
  checked once where it enters kiln (`check_asset_name()`) and compared byte for byte after that.
  See `docs/design/asset-model-next.md` Part 2 (decision I3) and `include/kiln/assets.h`.
  Example: `"meshes/ship_hauler_a.glb#hull_albedo"`. A name is at most 255 bytes
  (`kMaxAssetNameLen`), case-sensitive.
- `using AssetId = u64`: FNV-1a 64 of the full name, root and extension included (`asset_id()`);
  returns 0 for an invalid name.
- Matches mesh-format-spec §3, so `ModelInfo.assetId` and `TextureBinding.textureId` are usable as
  `AssetId`s.
- Id 0 is "none". Ids 1..15 are reserved for built-in placeholders: 1..12 are the four kinds for each
  shape (Tex2D 1..4, Cube 5..8, Array 9..12), 13..15 the Failed checker per shape. A request whose path hashes
  into 0..15 fails with K5003 (R5i).
- Collisions: every slot stores its path, and a request whose id matches a live slot with a
  different path panics, in all builds.

### States

`enum class State : u8 { Unloaded, Pending, MetaReady, Ready, Failed, Partial }`. `MetaReady`
applies to **both meshes and textures** (owner decision, open-questions A10).

| State | Meaning | `mesh_view()` | `texture_info()` |
|---|---|---|---|
| `Unloaded` | no live request, or stale handle | nullptr | placeholder, `isPlaceholder = true` |
| `Pending` | requested; reading, cooking or decoding metadata | nullptr | placeholder |
| `MetaReady` | metadata loaded and validated; payload in flight | parts, LODs, submeshes, mounts, bounds, materials | extent, format, levels, layers; still samples as the placeholder |
| `Ready` | payload uploaded and published | full view | full info, `isPlaceholder = false` |
| `Failed` | recoverable error; one diagnostic emitted | nullptr | placeholder |
| `Partial` | reserved for progressive loads (v0.8) | never produced | |

`gpu()` per state is in `adapter.md`. `Failed` holds no metadata.

### Queries

`state`, `has_meta` (MetaReady or Ready), `is_ready`, `version`, `id_of`, `gpu`, `mesh_view`
(nullptr unless `has_meta`) and `texture_info` (`ktx2::TextureDesc`, per-level offsets and row
pitches, `gpu`, `version`, `isPlaceholder`).

- All are table lookups: allocation-free and callable every frame.
- They read state as of the last `pump()`. Nothing changes between two pumps.
- `is_ready` stays true during a hot reload, because the old payload keeps being served.

### Transitions

All transitions become visible **only in `pump()`**, on the pump thread.

| From | Trigger | To | Event |
|---|---|---|---|
| (none) | `request_*()` of a new path | `Pending` | none |
| `Pending` | metadata read, validated and formats supported | `MetaReady` | `MetaReady` |
| `MetaReady` | upload complete (polled in `pump()`) | `Ready` | `Ready` |
| `Pending` / `MetaReady` | recoverable error | `Failed` | `Failed` |
| any live state | refcount reaches 0 | `Unloaded` | none |
| `Ready` | source changed, new version cooks and uploads | `Ready` (version + 1) | `Changed` |
| `Ready` | source changed, new version fails | `Ready` (old version kept) | none; one Error diagnostic |
| `Failed` | source changed, new version succeeds | `Ready` (version + 1) | `Ready` |
| `Failed` | source changed, new version fails again | `Failed` | `Failed` (with new status) |

The last four rows are hot reload (M5, `hot-reload.md`). Load groups see only `Failed` -> `Ready`:
the member moves from `failed` to `ready` with its bytes; the other reload rows leave group
counters alone.

- An asset never skips `MetaReady`: hosts see one event sequence for every asset.
- A reload loads the new version in the background and emits no `MetaReady`.

**Hot reload (proposed, M5):** the old payload stays `Ready` and servable while the new version
cooks and loads, so there is no placeholder flash. When the new upload completes, `pump()` swaps
metadata and payload together, increments the content version, calls `publish`, emits `Changed`,
and passes the old payload to `destroy_deferred`. A failed reload keeps the old payload and state
and emits one `Severity::Error` diagnostic, no `Failed` event.

### Requests

`request_mesh` / `request_texture(Context*, StrView path, RequestOptions const& = {})`,
`release(Context*, handle)`, and `find_mesh` / `find_texture(Context*, AssetId)` for an already
live asset. There is no request by id and no find by path. `register_mesh` / `register_texture`
add in-memory cooked bytes under a path.

`RequestOptions { priority = Normal; group = {}; textureKind = BaseColor; textureShape = Tex2D; }`; a `range` field is
reserved for partial loads (v0.8).

- A request increments a refcount. Requesting a live path returns **the same handle**.
- The first request calls the adapter's `acquire()` on the requesting thread. Repeat requests do
  not. IO starts on the next `pump()`, never inside the request (R5c).
- `release()` decrements. At 0 the asset unloads at once: `publish(id, null)`, the payload goes to
  `destroy_deferred`, handles go stale. A slot with a job in flight returns to the free list only
  when the job's completion is processed (R5b). Deferred or LRU unload may come later.
- Priority has two levels, `Normal` and `High`. A `High` request of a live asset raises it.

### Events

`Event { EventKind kind; AssetKind asset; u64 handle; u32 version; Status status; }` with
`EventKind { MetaReady, Ready, Changed, Failed }`.

- Produced only inside `pump()`. `events(ctx)` returns a span valid until the next `pump()`,
  which clears it. The events of every pump inside one `wait()` accumulate, so `events()` after
  `wait()` returns all of them.
- One buffer of `ContextDesc::maxEvents` (default 1024). When full, the oldest event is dropped,
  `PumpStats::eventsDropped` counts it, and the first drop in a pump emits a K5006 Warning.
- `version` is the content version. `handle` is raw bits because one stream carries all kinds.

### Placeholders

**Textures only.** Every texture handle is usable from the moment it is requested. The
placeholder is chosen by **texture kind** (`TextureKind { BaseColor, Normal, Orm, Emissive }`),
so a partially loaded scene still looks plausibly lit.

| Kind | Built-in placeholder (1x1 RGBA8) |
|---|---|
| `BaseColor` | mid-grey (0.5, 0.5, 0.5, 1), sRGB |
| `Normal` | flat normal (0.5, 0.5, 1), linear |
| `Orm` | AO 1, roughness 1, metallic 0, linear |
| `Emissive` | black, sRGB |
| Failed (`devPlaceholders`) | magenta checker (8x8), sRGB |

- The shape comes from `RequestOptions.textureShape` (`texture-shapes.md`); each shape the adapter
  supports has its own set: a cube placeholder is six faces of the pixel, an array placeholder one
  layer. The first request wins, like the kind. A cooked texture of another shape fails with K5017.
- The kind comes from `RequestOptions.textureKind`. `texture_kind_for_slot()` maps a `.mesh`
  `TextureSlot` to a kind (`MetalRough` and `Occlusion` map to `Orm`). Other usages use
  `BaseColor`. If a texture is requested with two kinds, the first request wins.
- Created through the adapter at `create()` with reserved ids (`adapter.md`). Never unloaded.
- **Host-overridable per kind** via `ContextDesc.placeholders` (a span of `PlaceholderDesc`: kind,
  RGBA8 format, extent, pixels), for every shape. Kinds without an entry use the built-in one.
- **Failed placeholder:** `ContextDesc.devPlaceholders` (default `KILN_DEBUG != 0`). When true, a
  `Failed` texture and a stale handle serve the magenta checker. When false, a failed texture
  serves its kind placeholder.
- Placeholder readiness depends on `kSelfSubmitting` (`adapter.md`). Without it, hosts must
  tolerate a null texture object for the first frame or two.

**Meshes have no placeholder.** `is_ready(ctx, mesh)` gates the draw. Coarsest-LOD-first comes
with progressive loading (`Partial`).

### Load groups

`group(ctx)`, `release(ctx, Group)` (frees the record; members stay requested),
`progress(ctx, g)` and `wait(ctx, g, WaitOptions)` return
`GroupStatus { ready, failed, pending, bytesDone, bytesTotal }`.

- Add requests with `request_*(..., { .group = g })`.
- **A slot belongs to the first live group it joins** (R5g). A later request that names another
  group does not change it. This replaces the proposed "one group per request call"
  (open-questions A11).
- A member whose refcount reaches 0 leaves the group. A failed member contributes no bytes.
- `pending` counts everything neither `Ready` nor `Failed`, including `MetaReady`. `bytesTotal`
  grows as members reach `MetaReady`. Loading screens should prefer counts until then.
- A group is **settled** when `pending == 0`. Failures are not fatal to `wait()`.

**`wait(ctx, g, { .timeoutMs, .uploadBytesPerPump })`:**

1. Raises every member to `Priority::High`. The raise is permanent; there is no level above `High`.
2. Loops `pump()` plus a 1 ms sleep on the calling thread until the group settles or the timeout
   expires (`timeoutMs = 0`: no timeout).
3. Returns the `GroupStatus`; on timeout, partial results. It never fails the members.

`wait()` **panics** (never hangs, K5007) when called off the pump thread or when the adapter lacks
`kSelfSubmitting`. Use it only on tiny critical groups (fonts, loading-screen art). For level
loads, keep the frame loop running, call `pump()` and show `progress()`.

The pump thread is the first thread that calls `pump()` or `wait()`. `pump()` from another thread
is a debug-only `KILN_ASSERT` (R5i).

### Stale-handle behavior

| API | Stale or null handle |
|---|---|
| `state()` | `Unloaded` |
| `has_meta()` / `is_ready()` | false |
| `mesh_view()` | nullptr |
| `texture_info()` / `gpu()` | the placeholder a stale handle serves (`adapter.md`); mesh `gpu()` is null |
| `release()` | no-op; `KILN_ASSERT` in debug builds for a non-null stale handle (likely a double release) |
| `progress()` / `wait()` on a stale group | all-zero `GroupStatus` |

## Rationale

- Separate identity and content version lets hot reload work without touching host data, and
  lets bindless slots stay fixed.
- `MetaReady` for both kinds keeps one state machine. Gameplay gets mesh metadata one small read
  early, and UI gets texture extents early.
- Kind-specific placeholders keep a partially loaded scene plausibly lit. No mesh placeholder: a
  stand-in cube is more distracting than an empty spot, and `is_ready()` is a cheap gate.
- Keeping the old payload during reload avoids flicker and keeps a working asset if the new
  source is half-saved.
- Same handle for duplicate requests makes refcounting the only ownership rule.
- Delivering everything through `pump()` keeps host code single-threaded.

## Alternatives considered

Bump the handle generation on reload, reload through `Pending`, reload failure sets `Failed`,
distinct handles per request, callbacks instead of events, 1x1 white / unit-cube placeholders,
textures without `MetaReady`, several groups per request, and `wait()` on any thread: all
rejected for the reasons above.

## Consequences / what this constrains later

- Metadata queries carry `version` and `isPlaceholder` from v0.5 on.
- Hot reload briefly needs memory for two payloads of the same asset.
- `Partial` must fit between `MetaReady` and `Ready` without changing existing transitions.
- `AssetId` = path hash, so renaming a cooked path changes the id. Hosts that want rename
  tolerance store paths, not ids.
- Ids 0..15 are unusable for assets.
- The event buffer is bounded; a burst larger than `maxEvents` between two drains loses the oldest
  events.

## Open points for the owner

- Two counters (slot generation vs content version), with `publish()` and `Changed` carrying the
  content version (R2).
- Confirm hot reload keeps the old payload `Ready`, and that reload failure does not change state.
- Confirm immediate unload at refcount 0 for v0.5.
- Confirm the placeholder table and the `devPlaceholders` default (`KILN_DEBUG != 0`).
- Confirm placeholder readiness: `create()` spins when `kSelfSubmitting`; otherwise `gpu()` is null
  until the first completing `pump()`.
- Confirm `RequestOptions.textureKind` and "first request's kind wins".
- Confirm reserved ids 1..15 for placeholders.
- **Decided (owner, asset-model-next I3):** the extension is part of the asset name; the runtime
  does not normalize names.
- Confirm the implemented deviations: first live group wins (R5g), `wait()`'s permanent raise to
  `High`, `pump()` off-thread as a debug assert only (R5i).
