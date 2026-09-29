# Readiness sets and aggregate signals

**Status:** Proposed (2026-09-29), not implemented. The owner requested a signal for a set of
textures, or a mesh and its textures, becoming usable or failing, and selected **live readiness**
over one-time completion. The interface and remaining semantics below are the proposed design.
**Decides:** Explicit required membership, overlapping sets, aggregate state and notifications,
and the boundary between readiness and rendering policy.
**Related:** [handles-and-states.md](handles-and-states.md),
[project terminology](../../CONTEXT.md).

## Need and existing API

A host should be able to hide an object until its mesh and required PBR textures are usable,
then enable rendering with one aggregate state check or notification. The set can instead cover
only a material's required textures. LODs and independently progressive resources need not join it.

Existing `Group` supports progress counts and blocking `wait()`. It has no aggregate event, and
`join_group` in `src/runtime/registry.cpp` assigns each asset to its first live group only. Reusing
a texture in a second material therefore cannot express a second independent readiness condition.
`pending == 0` also does not distinguish successful loading from failures or unfinished dependency
discovery. This proposal adds a separate concept; it does not silently change group semantics.

## Proposed surface

Names are illustrative, not a public API commitment:

```cpp
enum class ReadinessState : u8 { Building, Pending, Ready, Failed };

struct ReadinessStatus {
    ReadinessState state;
    u32 revision;
    u32 required, ready, pending, failed;
};

Result<ReadinessSet> readiness_set(Context*);
Status add_required(Context*, ReadinessSet, TextureHandle);
Status add_required(Context*, ReadinessSet, MeshHandle);
Status seal(Context*, ReadinessSet);
Status begin_update(Context*, ReadinessSet); // reopen membership, start a new revision
Status remove_required(Context*, ReadinessSet, TextureHandle);
Status remove_required(Context*, ReadinessSet, MeshHandle);
ReadinessStatus readiness(Context*, ReadinessSet);
Span<ReadinessEvent const> readiness_events(Context*);
void release(Context*, ReadinessSet);
```

`ReadinessEvent` identifies the set (including handle generation), membership revision, transition
sequence, old/new state, and a status snapshot. All operations are pump-thread operations. Signals
are events consumed by the host, not worker callbacks into rendering code. Capacity and invalid-
handle errors are returned by membership operations and must be handled before sealing.

The implementation must support many sets per asset and deduplicate members by asset kind plus
full handle. Adding the same member twice is a no-op, not an additional reference or count.
Only live handles from the same context are accepted; null/stale handles are rejected. If a request
itself returns null, the host must treat that as setup failure rather than omitting the requirement
and sealing an incomplete set. An already-Failed live handle is a valid required member.

## State and notification rules

- Newly created or reopened sets are `Building`: discovery is incomplete, so they cannot be Ready.
- Sealing declares that the host has supplied all requirements for this membership revision.
- A sealed set is `Failed` as soon as any required member is Failed; other members may still load.
- Otherwise it is `Pending` while any required member lacks a usable GPU payload.
- Otherwise it is `Ready`. Placeholders and `MetaReady` do not satisfy a requirement.
- A sealed empty set is Ready. An empty Building set is not Ready.
- `settled` can be derived separately as sealed with `pending == 0`; fail-fast notification must
  not require waiting for every other texture to finish.

Aggregate transitions are evaluated and emitted at the end of `pump()`, after asset transitions.
Membership edits immediately make the query state Building; sealing is scheduled for evaluation
at the next pump. An already-loaded collection therefore produces a Ready signal on that next
pump, not a synchronous callback from `seal()`.

Emit on state changes; a new revision receives its own initial evaluated state even when equal
to the previous revision's state. Coalesce transitions within one pump to the final state.
Every event carries its revision so hosts can ignore superseded notifications. The persistent
status query is authoritative if events are missed; document event capacity/overflow and expose
an overflow count. A failure snapshot reports counts and allows inspection of failed members;
existing per-asset diagnostics remain the source of detailed load errors.

For rendering, the predicate is simply `readiness(ctx, set).state == ReadinessState::Ready`.
The renderer chooses whether failure means hiding the object, drawing an error representation,
or accepting a fallback. Kiln does not toggle renderer objects itself.

## Ownership

Each unique membership retains one reference to its asset. The caller may release its original
request reference after successful insertion. Releasing a set drops only the references it owns;
shared assets remain alive through other sets or request references. Removing a member does the
same. All membership mutations are allowed only while Building.

This avoids a released failed member silently disappearing and turning the set into a success.
It also gives the host one lifetime owner for a renderable asset bundle. Cycles do not arise in the
initial design: sets contain asset handles only, not other sets.

Use separate bounded membership storage and reverse asset-to-set links. Mark affected sets dirty
when members change, then evaluate them during pump. Avoid scanning every set every frame.
Expose limits for set count and total memberships; partial setup must remain Building on errors.

## Mesh and its textures

Kiln still loads the assets the host requests. The mesh does not automatically load every image
reference or decide which PBR inputs are required by the renderer.

The host sequence is:

1. Create a Building set, request the mesh, and add it as required.
2. Once metadata is available, inspect the material bindings relevant to this renderable object.
3. Resolve names with `texture_asset_name`, request the selected textures with the appropriate
   texture kind/shape, and add them as required. Deduplicate shared textures.
4. Seal only after all required requests/memberships have been established successfully.
5. Enable rendering when the set reports Ready; react to Failed according to host policy.

If the mesh fails before metadata is available, seal the set containing that failed mesh. It then
reports Failed: discovery cannot proceed and must not leave the host waiting forever in Building.
If that mesh subsequently recovers, rebuild membership before drawing; the recovered mesh may
introduce textures that were never discovered during the failed attempt.

The host must inspect current mesh state/version as well as asset events, since the mesh may
already be Ready at insertion and events may have been missed. Track the mesh version used for
discovery. A helper for this workflow may follow, but its required-material selection must remain
explicit and it must process a successful Failed-to-Ready recovery as well as MetaReady/Changed.

## Reload and update semantics

The set observes usable current assets continuously (owner-selected behavior). A texture reload keeps the set
Ready while the old successful payload remains usable. A failed replacement also leaves it Ready,
matching kiln's existing per-asset behavior. Failed-to-Ready recovery can move a sealed set back to
Ready once all requirements are satisfied. A separate once-only subscription can later stop
observing after its first terminal outcome.

If a mesh version changes its dependencies, the host calls `begin_update`, reconciles required
members, and seals again. Process mesh version changes before making this frame's visibility
decision; an event for the preceding set revision must not enable the new dependency configuration.
Beginning an update makes the set Building immediately, so it gates drawing while new requirements
load. Keep the old membership references until explicitly removed or the set is released.

Readiness is not an atomic multi-asset publication transaction. Kiln still swaps individual asset
versions independently. Keeping an entire previous mesh/material generation visible until an
entire new generation is ready would require version pinning and a separate transaction design.
This proposal promises that every declared required asset is usable, not that all assets came
from the same cook/reload generation.

## Validation before implementation is complete

- Texture-only set: no Ready signal until all required uploads are Ready; placeholders never count.
- Mesh discovery: sealing prevents premature success; failure before metadata produces Failed.
- Shared textures in multiple sets; deduplication; insertion of already-Ready/Failed assets.
- One member fails while another remains pending; fail-fast and settled are distinguished.
- Caller releases its request refs; set retains its members; releasing one set preserves others.
- Failed-to-Ready recovery, failed hot reload retaining old content, and mesh dependency updates.
- Empty sets, invalid handles, capacity exhaustion, revision changes, and stale events.
- Event overflow followed by status resynchronization; no worker callbacks or steady-state
  allocation from evaluating unchanged sets.

## Decision and remaining design scope

The owner selected continuous readiness observation because the use case controls rendering over
time. A once-only subscription is optional future work. The proposed names, retaining ownership,
sealing/update operations, and event representation remain reviewable API design; they are not
implemented by this document.
