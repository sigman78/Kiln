# Handles, asset ids and states

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** Handle identity, asset ids, load states and transitions (including hot reload), request refcounting, events and placeholders.

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

| Counter | Where | Starts at | Changes when | Meaning |
|---|---|---|---|---|
| **Slot generation** | `Handle::generation` and the slot | 1 | slot reuse only | identity; stable for the life of a loaded asset |
| **Content version** | asset record, exposed by `view()` and events | 1 | each successful hot reload swap | which payload the view shows |

**Hot reload must not bump the handle generation.** The host stores handles in its own
structures (components, material tables, draw lists). If a reload changed the generation, every
stored handle would become stale at once, and the host would have to re-request every asset after
every edit. With a separate content version, stored handles keep working. Hosts that cache
derived data (descriptor sets, GPU addresses) compare `view.version` or react to `Changed` events.

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
- Id 0 is reserved as "none". A path that hashes to 0 is a cook error (practically never occurs).
- Collision handling in v0.5: the registry stores the path next to the id in debug builds and
  panics on a mismatch. Release builds trust the hash.

### States

```cpp
enum class State : u8 { Unloaded, Pending, Ready, Failed, Partial /* reserved, post-v0.5 */ };
```

| State | Meaning | `view()` returns |
|---|---|---|
| `Unloaded` | no live request, or stale handle | placeholder, `isPlaceholder = true` |
| `Pending` | requested; reading, cooking, decoding or uploading | placeholder |
| `Ready` | payload uploaded and complete | real payload |
| `Failed` | recoverable error; one diagnostic emitted | placeholder |
| `Partial` | reserved for progressive loads (v0.8) | never produced in v0.5 |

### Transitions

All transitions become visible **only in `pump()`**, on the pumping thread.

| From | Trigger | To | Event |
|---|---|---|---|
| (none) | `request()` of a new id | `Pending` | none |
| `Pending` | upload complete (polled in `pump()`) | `Ready` | `Ready` |
| `Pending` | recoverable error | `Failed` | `Failed` |
| `Ready` / `Failed` / `Pending` | refcount reaches 0 | `Unloaded` (slot freed) | none |
| `Ready` | source changed, new version cooks and uploads | `Ready` (version + 1) | `Changed` |
| `Ready` | source changed, new version fails | `Ready` (old version kept) | none; one Error diagnostic |
| `Failed` | source changed, new version succeeds | `Ready` (version + 1) | `Ready` |
| `Failed` | source changed, new version fails again | `Failed` | `Failed` (with new status) |

**Hot reload, proposed behavior (decision to confirm):**

- The old payload **stays `Ready` and servable** while the new version cooks and loads in the
  background. The state never goes back to `Pending`, so there is no placeholder flash.
- When the new payload's upload completes, `pump()` swaps it in atomically, increments the
  content version, emits `Changed`, and passes the old payload to `destroy_deferred`.
- If the reload fails, the old payload stays, the state stays `Ready`, and one `Severity::Error`
  diagnostic is emitted. No `Failed` event, because the asset is still usable.

### Requests

```cpp
Handle<Mesh> request<Mesh>(Context*, AssetId or StrView path, RequestOptions const& = {});
void         release(Context*, Handle<Mesh>);
```

- `request()` increments a refcount. Requesting an id that is already live returns **the same
  handle** and increments its refcount.
- `release()` decrements. At 0 the asset is eligible for unload. In v0.5 unload is immediate: the
  payload goes to `destroy_deferred` and the slot is freed (its generation increments on reuse).
  Later versions may defer or keep an LRU of released assets.
- Releasing a `Pending` asset cancels in-flight work where possible (IO and cook work finishes but
  its result is discarded).
- `RequestOptions` has `priority` (two levels in v0.5: `Normal`, `High`) and a **reserved** range
  field for partial loads.

### Events

```cpp
enum class EventKind : u8 { Ready, Changed, Failed };
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
- The event buffer is a preallocated ring; overflow is prevented by sizing it to the slot count
  (each asset produces at most one event per pump).
- Raw `u64` bits are used instead of a typed handle because one event stream carries all kinds.

### Placeholders

- One placeholder per asset kind. Built-in defaults: a **1x1 white** RGBA8 texture and a **unit
  cube** mesh (one part, one LOD, one submesh, one material slot).
- The host can replace them at context creation or later with its own (for example, a magenta
  checkerboard) via in-memory registration.
- Placeholders are uploaded through the adapter once at startup and never unloaded.
- Served through the same view API. Views carry `bool isPlaceholder`.

### Stale-handle behavior

| API | Stale or null handle |
|---|---|
| `state()` | returns `Unloaded` |
| `view()` | returns the placeholder view, `isPlaceholder = true` |
| `release()` | no-op; `KILN_ASSERT` fires in debug builds (likely a double release) |
| `request()` | n/a (takes an id, not a handle) |

## Rationale

- Separate identity and content version lets hot reload work without touching host data.
- Keeping the old payload during reload avoids flicker and keeps a working asset if the new
  source is broken mid-edit, which is common while an artist saves.
- Same handle for duplicate requests makes refcounting the only ownership rule.
- Delivering everything through `pump()` keeps host code single-threaded.

## Alternatives considered

- **Bump the handle generation on reload** (HANDOFF §6.3 wording): simple, but invalidates every
  host-stored handle on each edit.
- **Reload goes `Ready` to `Pending`**: placeholder flash on every edit.
- **Reload failure sets `Failed`**: loses a working asset because of a half-saved file.
- **Distinct handles per request**: needs a second indirection and makes `release()` ambiguous.
- **Callbacks instead of events**: forbidden from worker threads (HANDOFF §2); a polled array is
  simpler and allocation-free.

## Consequences / what this constrains later

- Views must carry `version` and `isPlaceholder` from v0.5 on.
- Hot reload briefly needs memory for two payloads of the same asset.
- `Partial` must fit between `Pending` and `Ready` without changing existing transitions.
- `AssetId` = path hash means renaming a cooked path changes the id; hosts must use paths, not
  ids, in their own saved data if they want rename tolerance.

## Open points for the owner

- Confirm two counters (slot generation vs content version), departing from HANDOFF §6.3 wording.
- Confirm hot reload keeps the old payload `Ready`, and that reload failure does not change state.
- Confirm immediate unload at refcount 0 for v0.5.
- Confirm built-in placeholders (1x1 white, unit cube).
- Confirm `AssetId` normalization: no extension, case-sensitive.
