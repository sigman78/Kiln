# Streaming: level-limited texture loads (v0.8)

**Status:** Proposed (2026-10-05). The direction is the owner's (2026-10-05): an object is loaded
whole or not at all, and the goal is latency at the level of the runtime library. The details and
the open points await sign-off. Nothing here is implemented.
**Decides:** What "streaming" means in kiln v0.8, what it drops from the earlier plan, the request
and adapter surface for a texture that is resident at a smaller size, and how its size changes.
**Related:** [handles-and-states.md](handles-and-states.md), [adapter.md](adapter.md),
[hot-reload.md](hot-reload.md), [async-read-path.md](async-read-path.md),
[readiness-sets.md](readiness-sets.md), [runtime-texture-arrays.md](runtime-texture-arrays.md).
Open points: `../open-questions.md` R30.

## Summary

- **All or none.** kiln never fills a GPU object in steps. Every upload makes one complete object,
  through today's adapter contract. `State::Partial` and range uploads are not needed for streaming.
- **A texture can be resident at a smaller size.** The host gives a largest extent; kiln loads the
  mip levels from the first one that fits, as a complete, smaller texture.
- **A size change is a swap.** kiln loads a new object at the new size and swaps it in, as a hot
  reload does. The host sees `Changed`.
- **The host decides what is resident.** kiln has no view of the screen. It reports sizes and does
  the loads; the host owns the policy and the budget.
- **Meshes load whole.** No LOD streaming and no split blobs in v0.8.
- **Latency work is part of v0.8:** one-pass texture loads (R27) and group readiness (R25).

## Why not progressive fill

The earlier plan reserved `State::Partial`, a range field on requests, `TextureDesc::firstLevel`
and an adapter contract for range uploads into an existing object. Its model: allocate the whole
mip chain, upload the coarse levels, bind, then upload the rest.

That model fits one case only: every level is wanted soon. It does not fit the others.

| Case | What it needs | Progressive fill |
|---|---|---|
| Regular textures and meshes, a level or a scene load | The whole object, soon | No gain that a placeholder does not give; the load is 0.5 s for 891 assets (`async-read-path.md` §9) |
| Top-mip streaming by screen size (open worlds) | The top levels absent for a long time, and their memory free | Allocates the top levels at once, so it saves no memory |
| Clipmaps, virtual textures | Tiles or regions chosen by the host, not a mip chain of one texture | Does not apply |
| Mesh LOD streaming for very large meshes | Clusters or LODs chosen by the host | Does not apply; cooked meshes have no generated LODs yet |

A host that wants a coarse texture first and the sharp one later gets it from two whole loads (see
"The coarse-first pattern"). So one mechanism covers the latency case and the memory case, and the
adapter contract does not change.

## The level-limited texture

A texture has a **wanted extent** in pixels, 0 for no limit. The **first level** `N` is the lowest
level whose width and height are both at most the wanted extent; if no level fits, the last level.
kiln resolves `N` when the metadata is known. An extent, not a level index, because the host knows
texel density before it knows the level count.

The object kiln uploads is a complete texture of levels `N` to the last:

- `TextureDesc::width`, `height` and `depth` are the extents of level `N`; `levels` is the count
  that is resident; `firstLevel` is `N`. The adapter needs no new code: it sees a smaller texture.
- Cubes and array layers keep their count; every face and layer starts at level `N`. A runtime
  texture array reads each layer from level `N`.
- A texture with one level, and a placeholder, ignore the limit.

The load reads less. A KTX2 file stores its levels smallest first and kiln compresses each level
on its own (`zstd-supercompression.md`), so levels `N` to the last are one byte range at the start
of the level data. Level 0 is about 75% of a 2D chain, so a texture at half its extent reads and
decodes a quarter of the bytes.

### Surface (sketch; names are open)

```cpp
struct RequestOptions {
    // ...
    u32 maxExtent = 0; ///< textures: largest width or height to load; 0 = the full texture
};

/// Changes the wanted extent of a live texture. The current object stays in use until the new one
/// is uploaded; then the texture emits Changed with a new version.
KILN_API void set_texture_extent(Context* ctx, TextureHandle h, u32 maxExtent);

struct TextureInfo {
    // desc: the cooked file's description, full extents and level count, as today.
    u32 firstLevel    = 0; ///< the level of the file that is level 0 of the GPU object
    u64 residentBytes = 0; ///< bytes of the upload that made the object
};
```

`levelOffsets` and `levelRowPitches` cover the resident levels. `ContextStats` gains the sum of
`residentBytes` over live textures, and the same for mesh payloads, so a host can hold a budget.

### One wanted extent per asset

A path has one handle and one object, so it has one wanted extent. `RequestOptions::maxExtent`
applies when the request makes the asset live; a request for a live path does not change it.
`set_texture_extent` is the one way to change it, and the last call wins. A host with several users
of one texture combines their needs itself. This is an open point (R30 a).

## A size change is a swap

`set_texture_extent` uses the reload path (`hot-reload.md`): the load writes the `next` set and a new
target object, and `pump()` swaps it in.

- The texture stays `Ready` and keeps its object while the new one loads.
- The swap increments the content version and emits `Changed`. The old object goes to `destroy`
  after the frames that used it.
- A failed change keeps the current object and emits one diagnostic, as a failed reload does.
- A new call before the load starts replaces the wanted extent. A call while a load runs starts
  one more load after it, as a second reload request does today.
- A hot reload loads at the wanted extent. A reload and a size change that are both due are one
  load.
- A memory-registered texture takes a size change (unlike a reload, which it cannot do): its bytes
  are in memory.

The first version runs both stages of a load for a size change: the provider's `prepare`, the
metadata step, then the upload. A size change of an unchanged artifact needs only the upload; that
is an optimisation for later, together with R27.

**Cost of the swap.** A change to a larger size reads the coarse levels again, at most a third of
the new upload. A copy from the old object on the GPU would avoid it, but needs a copy contract in
every adapter. Not in v0.8.

### The coarse-first pattern

For latency, a host requests textures with a small `maxExtent` (64, say), draws when they are
`Ready`, then calls `set_texture_extent(h, 0)`. Each step is a whole load. Coarse loads are small,
so many run per pump. kiln does not do this by itself: which textures deserve a second load, and
when, is the host's knowledge.

## Residency, budget and eviction

kiln provides the mechanism and the numbers, not the policy:

- **Residency:** the wanted extent per texture, and request and release for whole assets.
- **Budget:** `residentBytes` per texture and the sums in `ContextStats`. kiln enforces no budget.
- **Eviction:** the host lowers a texture's extent or releases the asset.

No adapter hook is needed, so `Adapter::reserved` stays reserved. Deferred unload at refcount 0
(the reserved-space register) is a separate question and stays unscheduled.

## Groups and states

No new state. A level-limited texture that is uploaded is `Ready`, and a size change keeps it
`Ready`. For group readiness (`readiness-sets.md`, R25 c): no member is ever `Partial`; a size change
does not move a group; eviction by release removes the member as any release does.

## Meshes

Meshes load whole. Progressive LODs need generated LODs (postponed, roadmap) and split blobs
(spec B16), and a host that streams very large meshes needs its own cluster scheme. All three stay
out of v0.8.

## What v0.8 drops, and where it goes

| Item | Was | Now |
|---|---|---|
| `State::Partial` | reserved for progressive loads | Removed from the enum when this note is accepted (an API break for the changelog) |
| `RequestOptions` range field | reserved for partial loads | Becomes `maxExtent` |
| `TextureDesc::firstLevel` | reserved for partial mip uploads | Used: the first resident level of a complete, smaller texture |
| Range uploads into an existing object | v0.8, shared with texture arrays | Unscheduled; only texture arrays want them (bounded staging, a reload of one layer, R12) |
| `Adapter::reserved` residency hooks | v0.8 | Not needed; stays reserved |
| `TextureCookSettings::residentMips` | v0.8 | Not needed: the host gives the extent; stays reserved, unscheduled |
| `MeshCookSettings::blobChunkSize`, split blobs | v0.8 | Unscheduled, with mesh LOD streaming |

When this note is accepted, the lines in `adapter.md`, `handles-and-states.md`, `hot-reload.md` and
`viewer.md` that describe progressive mips through further `bind` calls change to match.

## Not covered

- Clipmaps and virtual textures. Their tiles are separate assets, or need reads of a region of one
  file, which kiln does not have.
- Automatic selection of sizes from screen coverage. That is engine code.
- A GPU copy between the old and the new object.
- Sparse or partially resident textures.

## Measure first

Before the loader changes, `kiln_bench_load` gets `--max-extent N` and reports, on the benchmark
corpus:

- the share of bytes read and decoded at extents 64, 256 and 1024 against the full load;
- time to `Ready` for every texture at each extent, warm and cold, with 7 and with 2 workers;
- the time of a change from 64 to full for all textures, against one full load.

The first figure can be computed from the level tables today. No gate is set yet; the figures
decide whether the coarse-first pattern is worth documenting for hosts.

## Delivery sequence

1. The benchmark option and the figures above.
2. A level-limited first load: `RequestOptions::maxExtent`, the upload plan from level `N`,
   `TextureDesc::firstLevel`, `TextureInfo`, the null adapter, texture arrays. Tests compare the
   upload with the golden file's levels byte for byte.
3. A size change as a swap: `set_texture_extent`, `Changed`, the rules for calls that overlap a
   load or a reload. Tests with a held job system.
4. `residentBytes` and the `ContextStats` sums.
5. One example: `kiln-viewer` loads coarse first, then full.

Beside it, as their own decisions: one-pass texture loads from manifest metadata (R27) and group
readiness (R25).

## Open points for the owner (R30)

- **(a) One wanted extent per asset, last call wins.** The alternative keeps a want per request and
  loads the largest, which needs a token per request.
- **(b) An extent in pixels at the API, a level at the adapter.** The alternative is a level index
  at both, which a host cannot choose before `MetaReady`.
- **(c) A size change increments the content version and emits `Changed`,** the same event as a
  content change. The alternative is a new event kind.
- **(d) `TextureInfo::desc` stays the file's description** and `firstLevel` tells what is resident.
  The alternative: `desc` describes the resident object.
- **(e) Remove `State::Partial`** now, or keep it reserved.
- **(f) Range uploads for texture arrays move to Unscheduled.**
- **(g) No budget enforcement and no eviction policy in kiln.**
