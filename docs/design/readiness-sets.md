# Group readiness

**Status:** Proposed (2026-09-29), revised 2026-10-04: the readiness set is dropped and `Group` is
extended instead (owner, 2026-10-04). Not implemented. The owner requested a signal for a set of
textures, or a mesh and its textures, becoming usable or failing, and selected **live readiness**
over one-time completion.
**Decides:** Several groups per asset, a sealed group's aggregate state and its events, and the
boundary between readiness and rendering policy.
**Related:** [handles-and-states.md](handles-and-states.md) (load groups, R5g),
[hot-reload.md](hot-reload.md). Open points: `../open-questions.md` R25.

The file keeps its name so that links stay valid.

## Need

A host should be able to hide an object until its mesh and required PBR textures are usable,
then enable rendering with one aggregate state check or notification. The collection can instead
cover only a material's required textures. LODs and independently progressive resources need not
join it.

## Why Group, not a second concept

`Group` already counts `ready`, `failed` and `pending` members, moves a member from `failed` to
`ready` when a reload recovers it, and has a blocking `wait()`. `pending == 0 && failed == 0` is
already the success test. Three things are missing:

1. **One group per asset** (R5g). `join_group` in `src/runtime/registry.cpp` gives an asset to its
   first live group only. A texture shared by two materials cannot count in both.
2. **No aggregate event.** The host must poll `progress()`.
3. **No "membership is complete" marker.** While a mesh's textures are still being discovered,
   `pending == 0` can be true too early.

The first version of this note added a second aggregate (`ReadinessSet`: ten functions, a handle
type, a second event stream) beside `Group`. This revision closes the three gaps in `Group`:
one new function, one new status field, three new event kinds. One mechanism serves loading
screens, `wait()` and per-object readiness.

## Proposed surface

Names are illustrative, not a public API commitment:

```cpp
enum class GroupState : u8 {
    Open,    // not sealed: membership may be incomplete; never Ready
    Pending, // sealed; a member has no usable payload yet
    Ready,   // sealed; every member is Ready
    Failed,  // sealed; a member is Failed, or a join failed
};

struct GroupStatus {
    u32 ready, failed, pending;  // as today
    u64 bytesDone, bytesTotal;   // as today
    GroupState state;            // new
    bool settled() const;        // as today: pending == 0
};

void seal(Context*, Group);      // new: the host has requested every member

enum class EventKind : u8 { MetaReady, Ready, Changed, Failed,
                            GroupPending, GroupReady, GroupFailed }; // three new
```

`group()`, `release(ctx, Group)`, `progress()`, `wait()` and `RequestOptions::group` keep their
signatures. A group that is never sealed behaves exactly as today and emits no events.

### Membership

- An asset joins a group through `request_*(..., { .group = g })`, `register_*` or
  `TextureArrayDesc::group`, as today. **The first-group-wins rule (R5g) goes:** each request that
  names a group joins the asset to that group. One request call still names one group.
- Joining the same group twice is a no-op for the group. The request still takes its reference.
- An asset that is already `Ready` or `Failed` joins with that state, as today.
- **A membership holds no reference**, as today. The host keeps its request reference; it needs
  the handle for `gpu_object()` each frame anyway. Handles follow the usual refcount rule and do
  not depend on the group.
- A member whose last reference is released leaves all its groups, as today. In a sealed group
  this changes the result: releasing a `Failed` member can turn the group `Ready`. Rule for the
  host: release the group before the references of its members.
- `release(ctx, Group)` frees the record and its memberships. Members stay requested.
- There is no call to remove one member, and no call to reopen a sealed group. A changed set of
  requirements gets a new group ("Reload and update").
- A join after `seal()` is allowed. The group evaluates again and can go back to `Pending`.

### State rules

- A group is `Open` until `seal()`. `Open` is never `Ready`.
- A sealed group is `Failed` as soon as one member is `Failed`; other members may still load.
- Otherwise it is `Pending` while a member lacks a usable GPU payload. Placeholders and
  `MetaReady` do not satisfy a member.
- Otherwise it is `Ready`. A sealed empty group is `Ready`.
- `settled()` stays `pending == 0`. It differs from the state: a group can be `Failed` and not yet
  settled. `wait()` still returns when the group settles, sealed or not.
- **A join that fails makes the group `Failed`** and emits K5005 (membership storage full, see
  "Storage"). A group that lacks a member must never report `Ready`. If a request itself returns a
  null handle, the host treats that as setup failure and does not seal.

`progress()` computes the state from the counters, so the query is always current: a group whose
members are all loaded reads `Ready` immediately after `seal()`.

### Events

Group events use the existing stream, `events(ctx)`. There is no second stream.

- `Event::kind` is `GroupPending`, `GroupReady` or `GroupFailed`. `Event::handle` is
  `Group::bits()`. `Event::asset` and `Event::version` carry no meaning; a host reads `kind` first.
- Only sealed groups emit. A group emits when its state differs from the state it last reported.
  Sealing reports the first state on the next `pump()`, also when that state is `Ready`.
- Group events come at the end of `pump()`, after every asset event of that pump. Several changes
  in one pump become one event with the final state.
- The ring is shared, so the existing overflow rule applies: the oldest event is dropped, K5006,
  `PumpStats::eventsDropped`. `progress()` is authoritative when events are missed.
- An event for a released group carries a stale handle; the host compares it with the group it
  holds now.

For rendering, the predicate is `progress(ctx, g).state == GroupState::Ready`. The renderer
chooses whether failure means hiding the object, drawing an error representation, or accepting a
fallback. Kiln does not toggle renderer objects itself. Per-asset diagnostics remain the source of
detailed load errors.

`wait()` and the events of a sealed group work together: the events of every pump inside one
`wait()` accumulate, as today.

## Storage

One membership table replaces the four group fields of `Slot` (`groupIndex`, `groupGen`,
`groupBytes`, `groupAs`):

- A pool of links, sized at `create()` by a new `ContextDesc::maxGroupMembers`. Each link is in two
  intrusive lists: its group's members and its asset's groups. `groupBytes` and `groupAs` move
  into the link.
- A slot transition walks the asset's links and updates each group's counters. A group whose
  counters change goes on a dirty list; `pump()` evaluates only that list. Nothing scans every
  group, and nothing allocates in steady state.
- `wait()`'s priority raise (`boost_group`) walks the group's member list. Today it scans every slot.
- `ContextDesc::maxGroups` defaults to 64, which suits loading screens. A host with one group per
  renderable object raises it.

## Mesh and its textures

Kiln still loads only what the host requests. A mesh load does not load its textures, and the
renderer decides which PBR inputs it requires.

A helper carries most of the work, so it is part of this proposal:

```cpp
struct MeshTexturesDesc {
    Group group       = {};
    Priority priority = Priority::Normal;
    u32 slots         = ~0u; // one bit per mesh::TextureSlot: the slots the renderer requires
};
/// Requests the texture of each binding of the mesh whose slot is in `slots`, with the kind
/// texture_kind_for_slot() gives, into `group`. Each name is requested once. Writes the handles to
/// `out` and returns their count; the host releases them. Needs has_meta(mesh).
Result<u32> request_mesh_textures(Context*, MeshHandle, MeshTexturesDesc const&, Span<TextureHandle> out);
```

It is a loop over `mesh_view()->textures()` with `texture_asset_name()` and `request_texture()`.
The host calls it; kiln does not follow the references on its own (`asset-model-next.md`).

The host sequence:

1. Create a group and request the mesh into it.
2. When the mesh has metadata (`has_meta()`, or its `MetaReady` event), call
   `request_mesh_textures` with the same group.
3. `seal()` the group.
4. Draw when `progress()` reports `Ready`. React to `Failed` according to host policy.

The mesh may already have metadata at step 1, so check `has_meta()` and do not rely on the event
alone.

If the mesh fails before it has metadata, seal the group with the failed mesh in it. It reports
`Failed`, so the host does not wait in `Open` forever. If the mesh recovers later (a `Ready` event
with a new version), its textures were never requested: the host requests them into the same group
when it handles that event, and the group goes to `Pending` until they load. Handle the asset
events of a pump before the draw decision, and take the decision from `progress()`: a `GroupReady`
event from that pump can already be out of date.

## Reload and update

A group observes its members continuously (owner-selected behavior). The existing counters already
do this:

- A reload of a `Ready` member leaves the group `Ready`: the old payload stays usable. A failed
  reload leaves it `Ready` too.
- A `Failed` member that reloads successfully moves from `failed` to `ready`, so the group can go
  from `Failed` to `Ready`.

**A mesh version with other dependencies gets a new group.** The host keeps the current group and
keeps drawing with it, builds a second group for the new version (the mesh, then
`request_mesh_textures`, then `seal()`), and when the second group is `Ready` it switches,
releases the old group and then the references it no longer needs. Shared textures are requested
again, which costs one reference each. The object is never hidden during the update, which follows
hot reload's rule of serving the current version. The first version of this note reopened the set
and hid the object until the new textures loaded.

Readiness is not an atomic multi-asset publication. Kiln still swaps each asset version on its
own: after a mesh reload the new mesh draws with the old textures until the new group is `Ready`.
The proposal promises that every member is usable, not that all members come from the same cook.
Ordering a mesh reload after its re-keyed embedded textures (left to this note by R21) needs
version pinning. This note does not provide it; R25 (b) holds the question.

## v0.8: partial payloads and eviction

`State::Partial` and eviction are not designed yet. The proposed rule for a group:

- A member counts as ready only in `State::Ready`. A `Partial` member counts as `pending`.
- A member that loses its payload (eviction) leaves `ready`, so a sealed group can go from `Ready`
  back to `Pending`. The `GroupPending` event exists for this case and for a join after `seal()`.
- A host that draws with a partial payload (coarse mips, coarse LOD) leaves that asset out of the
  group, as the Need section says for LODs.

The v0.8 design must confirm or replace this rule (R25 (c)).

## Validation

- Texture-only group: not `Ready` until every member's upload is `Ready`; placeholders never count.
- An unsealed group emits no events and reports the same counts as today (existing group tests).
- Mesh discovery: an `Open` group is never `Ready`; failure before metadata gives `Failed`.
- A texture in several groups counts in each; a duplicate join; joins of `Ready` / `Failed` assets.
- One member fails while another is pending: `Failed` state, `settled()` false.
- A member's last reference released in a sealed group; a group released before its members.
- `Failed` to `Ready` recovery; a failed reload of a `Ready` member; the two-group mesh update.
- A join after `seal()`; an empty sealed group; a stale group; membership storage full (`Failed`).
- Group events after asset events in one pump; one event per group per pump; event overflow, then
  `progress()`.
- `wait()` on a sealed and on an unsealed group.
- No steady-state allocation; `pump()` touches only dirty groups.

## API breaks when implemented

To record in `CHANGELOG.md` at that time:

- A request that names a second group for a live asset now joins it (was: ignored, R5g).
- `EventKind` gains three values; a host's `switch` over it needs the new cases. For group events
  `Event::asset` carries no meaning.
- `GroupStatus` gains `state`; `ContextDesc` gains `maxGroupMembers`.

## Open points for the owner

- Confirm that a membership holds no reference. The alternative (the group owns a reference to each
  member) removes the "release the group first" rule, but changes `release(ctx, Group)` for every
  existing host.
- Confirm that a join after `seal()` is allowed. The alternative is to treat it as misuse.
- `request_mesh_textures` in `kiln_runtime` (proposed, beside `texture_asset_name`) or in
  `examples/adapter_support/`.
- The default of `maxGroupMembers` (proposed: `maxAssets`), and whether `maxGroups` rises.
- A once-only subscription (stop observing after the first terminal state) is optional future work.
