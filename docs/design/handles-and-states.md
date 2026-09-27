# Handles, asset ids and states

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** Handle identity, asset ids, load states and transitions (including hot reload), request refcounting, events, placeholders and load groups.

Aligned to HANDOFF v2 (2026-09-26): `MetaReady`, kind-specific texture placeholders, `has_meta` /
`is_ready` / `gpu()`, load groups.

## Decision

### `Handle<Tag>` (implemented in `core.h`)

```cpp
template <class Tag>
struct Handle {
    u32 index      = kInvalid;
    u32 generation = 0;          // 0 = null, never issued
    bool is_null() const;        // generation == 0
    u64  bits() const;           // (generation << 32) | index
    static Handle from_bits(u64);
};
```

- `Tag` is a phantom type (`struct Mesh; struct Texture;`), so a mesh handle cannot be passed
  where a texture handle is expected.
- The registry keeps one slot array per asset kind. Each slot stores its current generation.
- **Slot generation** increments when a slot is freed and reused for a different asset. A handle
  whose generation does not match its slot is **stale**. Generation 0 is skipped on wrap.
- `bits()` / `from_bits()` let hosts store handles in a `u64` (ECS components, events).

### Proposal: two separate counters

**Status: Proposed, owner undecided.**

| Counter | Where | Starts at | Changes when | Meaning |
|---|---|---|---|---|
| **Slot generation** | `Handle::generation` and the slot | 1 | slot reuse only | identity; fixed for the life of a loaded asset |
| **Content version** | asset record (`u32`), exposed by `view()`, events and `publish()` | 1 | each successful hot reload swap | which payload the asset currently serves |

**Hot reload must not bump the handle generation.** The host stores handles in its own
structures (components, material tables, draw lists). If a reload changed the generation, every
stored handle would become stale at once, and the host would have to re-request every asset after
every edit. With a separate content version, stored handles keep working. Hosts that cache
derived data (descriptor sets, GPU addresses) compare `view.version` or react to `Changed` events.

**`publish()`'s `generation` parameter and `Changed` events carry the content version**, not the
handle generation. HANDOFF v2 §6.4 names the parameter `generation`; kiln's `adapter.h` calls it
`version` (see `adapter.md`).

Why:

- Bindless slots exist so that materials never refresh. The adapter overwrites the same slot on
  `publish()`, and the material keeps its slot index.
- Bumping the handle generation on reload would force every holder of the handle to refresh, which
  is exactly the churn the slot model avoids.
- The cost is one `u32` per registry entry.

> This departs from HANDOFF §6.3, which says hot reload "bumps the generation". The proposal keeps
> that intent (a detectable change) but moves it to the content version.

### Proposal: `AssetId`

```cpp
using AssetId = u64;   // FNV-1a 64 of the normalized cooked asset path
```

- Normalization: forward slashes, no leading `./` or `/`, **no extension**, case-sensitive.
  Example: `"meshes/ship_hauler_a/hull_albedo"`.
- Matches mesh-format-spec §3 ("Asset / texture IDs: FNV-1a 64 of the cooked asset path"), so
  `ModelInfo.assetId` and `TextureBinding.textureId` in `.mesh` files are directly usable as
  `AssetId`s.
- Computed with `fnv1a64(StrView)` / `"..."_h` in `hash.h` (constexpr).
- Id 0 is reserved as "none". Ids 1 to 15 are reserved for built-in placeholders (they need an id
  for `begin_upload`). A path that hashes into 0..15 is a cook error (practically never occurs).
- Collision handling in v0.5: the registry stores the path next to the id in debug builds and
  panics on a mismatch. Release builds trust the hash.

### States

```cpp
enum class State : u8 { Unloaded, Pending, MetaReady, Ready, Failed, Partial /* reserved, post-v0.5 */ };
```

`MetaReady` applies to **both meshes and textures** (owner decision on HANDOFF §13 Q10: yes,
uniform state machine).

| State | Meaning | Mesh `view()` | Texture `view()` | `gpu()` |
|---|---|---|---|---|
| `Unloaded` | no live request, or stale handle | empty, `hasMeta = false` | empty, `hasMeta = false` | texture: Failed placeholder; mesh: null |
| `Pending` | requested; reading, cooking or decoding metadata | empty | empty | texture: kind placeholder; mesh: null |
| `MetaReady` | metadata loaded and validated; payload still in flight | parts, LODs, submeshes, mounts, bounds, material names | extent, format, levels, layers | texture: kind placeholder; mesh: null |
| `Ready` | payload uploaded and complete | full view | full view | real object |
| `Failed` | recoverable error; one diagnostic emitted | empty | empty | texture: Failed placeholder; mesh: null |
| `Partial` | reserved for progressive loads (v0.8) | never produced in v0.5 | | |

- **Meshes at `MetaReady`:** the CPU region (header plus all metadata sections) arrives with the
  first small read. Gameplay can place entities, attach to mounts and cull by bounds before the
  geometry is drawable.
- **Textures at `MetaReady`:** the KTX2 header and level index are read. Extent, format and level
  count are known, for example to pre-size UI layout. The texture still samples as its placeholder.
- `Failed` holds no metadata. `has_meta()` is false for a failed asset.

### Queries

```cpp
State       state   (Context*, Handle<T>);
bool        has_meta(Context*, Handle<T>);   // MetaReady or Ready (later: Partial)
bool        is_ready(Context*, Handle<T>);   // Ready
MeshView    view    (Context*, Handle<Mesh>);
TextureView view    (Context*, Handle<Texture>);
GpuObject   gpu     (Context*, Handle<T>);   // see adapter.md
```

- All are table lookups: allocation-free and callable every frame.
- They read state as of the last `pump()`. Nothing changes between two pumps.
- `is_ready` stays true during a hot reload, because the old payload keeps being served.

### Transitions

All transitions become visible **only in `pump()`**, on the pumping thread.

| From | Trigger | To | Event |
|---|---|---|---|
| (none) | `request()` of a new id | `Pending` | none |
| `Pending` | metadata read and validated | `MetaReady` | `MetaReady` |
| `MetaReady` | upload complete (polled in `pump()`) | `Ready` | `Ready` |
| `Pending` / `MetaReady` | recoverable error | `Failed` | `Failed` |
| any live state | refcount reaches 0 | `Unloaded` (slot freed) | none |
| `Ready` | source changed, new version cooks and uploads | `Ready` (version + 1) | `Changed` |
| `Ready` | source changed, new version fails | `Ready` (old version kept) | none; one Error diagnostic |
| `Failed` | source changed, new version succeeds | `Ready` (version + 1) | `Ready` |
| `Failed` | source changed, new version fails again | `Failed` | `Failed` (with new status) |

- An asset never skips `MetaReady`. If metadata and payload both complete before the same pump
  (small files, in-memory registration, null adapter), that pump emits `MetaReady` then `Ready`,
  in that order, so hosts see one sequence for every asset.
- A reload loads the new version in the background. Its metadata is not exposed until the swap,
  so no `MetaReady` event is emitted for a reload.

**Hot reload, proposed behavior (decision to confirm):**

- The old payload **stays `Ready` and servable** while the new version cooks and loads in the
  background. The state never goes back to `Pending` or `MetaReady`, so there is no placeholder
  flash.
- When the new payload's upload completes, `pump()` swaps metadata and payload together,
  increments the content version, calls `publish(id, obj, version)`, emits `Changed`, and passes
  the old payload to `destroy_deferred`.
- If the reload fails, the old payload stays, the state stays `Ready`, and one `Severity::Error`
  diagnostic is emitted. No `Failed` event, because the asset is still usable.

### Requests

```cpp
Handle<Mesh> request<Mesh>(Context*, AssetId or StrView path, RequestOptions const& = {});
void         release(Context*, Handle<Mesh>);

struct RequestOptions {
    Priority    priority    = Priority::Normal;       // Normal, High
    Group       group       = {};                     // null = no group
    TextureKind textureKind = TextureKind::BaseColor; // textures only: selects the placeholder
    // reserved: range (partial loads, v0.8)
};
```

- `request()` increments a refcount. Requesting an id that is already live returns **the same
  handle** and increments its refcount.
- The first `request()` of an id calls the adapter's `acquire()` on the requesting thread, so a
  bindless adapter can hand out a placeholder-bound slot at once (see `adapter.md`). Repeat
  requests of a live id do not call it again.
- `release()` decrements. At 0 the asset is eligible for unload. In v0.5 unload is immediate: the
  payload goes to `destroy_deferred` and the slot is freed (its generation increments on reuse).
  Later versions may defer or keep an LRU of released assets.
- Releasing a `Pending` or `MetaReady` asset cancels in-flight work where possible (IO and cook
  work finishes but its result is discarded).
- Priority has two levels in v0.5 (`Normal`, `High`), plus an internal **boost** used by `wait()`.

### Events

```cpp
enum class EventKind : u8 { MetaReady, Ready, Changed, Failed };
enum class AssetKind : u8 { Mesh, Texture };

struct Event {
    EventKind kind;
    AssetKind asset;
    u64       handle;   // Handle<...>::bits(); rebuild with Handle<Tag>::from_bits()
    u32       version;  // content version after the event
    Status    status;   // non-Ok only for Failed
};
```

- Produced only inside `pump()`, on the pumping thread. `events(ctx)` returns a `Span<Event const>`
  valid until the next `pump()`. The host drains it each frame.
- The event buffer is a preallocated ring sized to **two events per slot**: an asset produces at
  most two events per pump (`MetaReady` then `Ready`).
- `version` is the content version, never the handle generation.
- Raw `u64` bits are used instead of a typed handle because one event stream carries all kinds.

### Placeholders

**Textures only.** Every texture handle is usable from the moment it is requested. The placeholder
is chosen by **texture kind**, so a partially loaded scene still looks plausibly lit.

```cpp
enum class TextureKind : u8 { BaseColor, Normal, Orm, Emissive };
```

| Kind | Built-in placeholder (1x1 RGBA8) |
|---|---|
| `BaseColor` | mid-grey (0.5, 0.5, 0.5, 1), sRGB |
| `Normal` | flat normal (0.5, 0.5, 1), linear |
| `Orm` | AO 1, roughness 1, metallic 0, linear |
| `Emissive` | black, sRGB |
| Failed (dev builds) | magenta checker (8x8), sRGB |

- The kind comes from `RequestOptions.textureKind`. A helper maps `.mesh` `TextureSlot` to a kind
  (`MetalRough` and `Occlusion` map to `Orm`), so a host that requests a mesh's textures from its
  `MTEX` bindings gets the right kind. Other usages (mask, UI, height) use `BaseColor`.
- If the same texture is requested with two different kinds, the first request's kind wins. Only
  the placeholder differs; the payload is the same.
- **Created through the adapter at context creation**, via the normal `begin_upload` /
  `commit_upload` path, with reserved ids 1..15. Never unloaded.
- **Host-overridable per kind** via `ContextDesc.placeholders[kind]` (format, extent, texels). An
  empty entry uses the built-in one. kiln uploads the host's data the same way.
- **Failed placeholder:** `ContextDesc.devPlaceholders` (default `KILN_DEBUG != 0`). When true, a
  `Failed` texture serves the magenta checker. When false, it serves its kind placeholder.
- Stale or null texture handles get the Failed placeholder from `gpu()`, so the bug is visible in
  dev builds.

**Open point: async adapters.** An adapter that submits uploads only inside the host's frame
completes the placeholder uploads after the first `pump()` at the earliest. Proposed:

- If the adapter sets `kSelfSubmitting`, `create()` spins on `is_upload_complete` until every
  placeholder is uploaded. Placeholders are then valid when `create()` returns.
- Otherwise `gpu()` returns a null `GpuObject` for textures until the first `pump()` that sees the
  placeholder uploads complete. Hosts must tolerate null for the first frame or two.

**Meshes have no placeholder.** `is_ready(ctx, mesh)` gates the draw: the host skips the mesh until
`Ready`. A dev-only bounding-box proxy drawn by the host from `MetaReady` bounds is optional.
Coarsest-LOD-first comes with progressive loading (`Partial`).

### Load groups

```cpp
using Group = Handle<GroupTag>;   // small handle, same index/generation rules

Group       group   (Context*);
void        release (Context*, Group);       // frees the group record; members stay requested
GroupStatus progress(Context*, Group);
GroupStatus wait    (Context*, Group, WaitOptions const&);

struct GroupStatus { u32 ready, failed, pending; u64 bytesDone, bytesTotal; };
struct WaitOptions { u32 timeoutMs; };
```

- Add requests with `request(..., { .group = g })`.
- **Membership is tracked per `request()` call, not per asset.** Each call that names a group adds
  its handle to that group. The same asset can appear in several groups through separate requests.
  A handle already in the group is not added twice.
- One group per `request()` call (HANDOFF §13 Q11, proposed answer: one group per request call is
  enough).
- A member whose refcount reaches 0 leaves the group.
- `progress()` counts members: `ready` (`Ready`), `failed` (`Failed`), `pending` (everything else,
  including `MetaReady`). `bytesTotal` grows as members reach `MetaReady` and their payload size
  becomes known. Loading screens should prefer counts until all pending members have meta.
- A group is **settled** when every member is `Ready` or `Failed`. Failures are not fatal to
  `wait()`; the caller decides.

**`wait(ctx, g, { .timeoutMs })`:**

1. Raises every unsettled member to boost priority (above `High`). Boost is cleared on return.
2. Loops `pump()` plus a short sleep on the calling thread until the group is settled or the
   timeout expires.
3. Returns the `GroupStatus`. On timeout it returns partial results; it never fails the members.

`wait()` **panics** (never hangs) when:

- it is called off the pump thread (the thread that calls `pump()`), or
- the adapter lacks `AdapterCaps::kSelfSubmitting`, because uploads would never complete without
  the host recording a frame.

Recommended usage: `wait()` only on tiny critical groups (fonts, loading-screen art). For level
loads, keep the frame loop running, call `pump()`, show `progress()`, and stream everything else
behind placeholders.

### Stale-handle behavior

| API | Stale or null handle |
|---|---|
| `state()` | returns `Unloaded` |
| `has_meta()` / `is_ready()` | return false |
| `view()` | returns an empty view, `hasMeta = false` |
| `gpu()` | texture: Failed placeholder; mesh: null `GpuObject` |
| `release()` | no-op; `KILN_ASSERT` fires in debug builds (likely a double release) |
| `request()` | n/a (takes an id, not a handle) |
| `progress()` / `wait()` on a stale group | all-zero `GroupStatus`; `KILN_ASSERT` in debug builds |

## Rationale

- Separate identity and content version lets hot reload work without touching host data, and
  lets bindless slots stay fixed.
- `MetaReady` for both kinds keeps one state machine. Gameplay gets mesh metadata one small read
  early, and UI gets texture extents early.
- Kind-specific placeholders keep a partially loaded scene plausibly lit. A white placeholder makes
  normal and ORM slots look wrong.
- No mesh placeholder: a unit cube in place of every missing mesh is more distracting than an
  empty spot, and `is_ready()` is a cheap gate.
- Keeping the old payload during reload avoids flicker and keeps a working asset if the new
  source is broken mid-edit, which is common while an artist saves.
- Same handle for duplicate requests makes refcounting the only ownership rule.
- Delivering everything through `pump()` keeps host code single-threaded. `wait()` is just `pump()`
  in a loop on the same thread.

## Alternatives considered

- **Bump the handle generation on reload** (HANDOFF §6.3 wording): simple, but invalidates every
  host-stored handle on each edit and forces every bindless holder to refresh.
- **Reload goes `Ready` to `Pending`**: placeholder flash on every edit.
- **Reload failure sets `Failed`**: loses a working asset because of a half-saved file.
- **Distinct handles per request**: needs a second indirection and makes `release()` ambiguous.
- **Callbacks instead of events**: forbidden from worker threads (HANDOFF §2); a polled array is
  simpler and allocation-free.
- **One placeholder per asset kind (1x1 white texture, unit cube mesh)**: the earlier proposal.
  Replaced by kind-specific texture placeholders and no mesh placeholder (HANDOFF v2).
- **Textures go `Pending` to `Ready` directly**: fewer states, but two state machines and no early
  extent.
- **Several groups per request**: more bookkeeping per member; separate requests already cover it.
- **`wait()` on any thread**: needs cross-thread pumping and locking in `pump()`.

## Consequences / what this constrains later

- Views must carry `version`, `hasMeta` and `isPlaceholder` from v0.5 on.
- Hot reload briefly needs memory for two payloads of the same asset.
- `Partial` must fit between `MetaReady` and `Ready` without changing existing transitions.
- `AssetId` = path hash means renaming a cooked path changes the id. Hosts must use paths, not
  ids, in their own saved data if they want rename tolerance.
- Ids 1..15 are unusable for assets.
- The event ring holds two events per slot. A new event kind that can coincide in one pump needs a
  larger bound.

## Open points for the owner

- Two counters (slot generation vs content version), with `publish()` and `Changed` carrying the
  content version. **Owner undecided;** proposed: content version.
- Confirm hot reload keeps the old payload `Ready`, and that reload failure does not change state.
- Confirm immediate unload at refcount 0 for v0.5.
- Confirm the placeholder table and the `devPlaceholders` default (`KILN_DEBUG != 0`).
- Confirm placeholder readiness: `create()` spins when `kSelfSubmitting`; otherwise `gpu()` is null
  until the first completing `pump()`.
- Confirm `RequestOptions.textureKind` and "first request's kind wins".
- Confirm reserved ids 1..15 for placeholders.
- Events produced by the `pump()` calls inside `wait()`: proposed, they accumulate and are returned
  by `events()` after `wait()` returns (coalesced per asset so the bound holds).
- Pump thread identity: proposed, bound on the first `pump()` or `wait()`; a later call from
  another thread panics.
- Confirm `AssetId` normalization: no extension, case-sensitive.
