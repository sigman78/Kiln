# Streaming: level-limited texture loads (v0.8)

**Status:** Decided (owner, 2026-10-09): open points (a), (c) to (g) as recorded below; (b) stays
proposed. The direction is the owner's (2026-10-05): an object is loaded whole or not at all, and
the goal is latency at the level of the runtime library. Steps 1 (measure), 2 (the level-limited
first load), 3 (the size change) and 4 (resident bytes) are done; step 5 is not implemented.
**Decides:** What "streaming" means in kiln v0.8, what it drops from the earlier plan, the request
and adapter surface for a texture that is resident at a smaller size, and how its size changes.
**Related:** [handles-and-states.md](handles-and-states.md), [adapter.md](adapter.md),
[hot-reload.md](hot-reload.md), [async-read-path.md](async-read-path.md),
[readiness-sets.md](readiness-sets.md), [runtime-texture-arrays.md](runtime-texture-arrays.md).
Open points: `../open-questions.md` R30. Figures: "Measure first" (2026-10-09).

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

### Surface

Step 2 is implemented (2026-10-09): `RequestOptions::maxExtent` and `TextureArrayDesc::maxExtent`
(`include/kiln/assets.h`), the first level and the resident layout in the metadata step
(`first_level`, `resident_desc`, `texture_meta` and `run_array_meta` in `src/runtime/loader.cpp`),
`TextureDesc::firstLevel` at the adapter, `TextureInfo::firstLevel`. A memory-registered texture
and a hot reload load at the slot's extent. Step 3 (2026-10-09): `set_texture_extent` and
`EventKind::Resized` (`include/kiln/assets.h`); the size change is a reload that starts in
`resize_slot` and is told apart by `Slot::resizing` (`src/runtime/pump.cpp`); the job reads the
extent fixed at its submit (`JobInput::maxExtent`), and `settle()` starts one more load when the
wanted extent differs from the one the current object has (`MetaSet::texExtent`). Step 4
(2026-10-09): `TextureInfo::residentBytes` and `ContextStats::residentTextureBytes` /
`residentMeshBytes`, summed over the Ready objects in `stats()` (`src/runtime/context.cpp`).

```cpp
struct RequestOptions {
    // ...
    u32 maxExtent = 0; ///< textures: largest width or height to load; 0 = the full texture
};

/// Changes the wanted extent of a live texture. The current object stays in use until the new one
/// is uploaded; then the texture emits Resized with a new version.
KILN_API void set_texture_extent(Context* ctx, TextureHandle h, u32 maxExtent);

/// Resized: the same artifact at other resident levels. A host that does not tell them apart
/// rebinds on every event with a new version, as for Changed.
enum class EventKind : u8 { MetaReady = 0, Ready, Changed, Failed, Resized };

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
- The swap increments the content version and emits `Resized` (R30 c): the same swap as a reload,
  with its own event kind, so a host can tell "same pixels, other levels" from a content change.
  The old object goes to `destroy` after the frames that used it.
- A failed change keeps the current object and emits one diagnostic, as a failed reload does.
- A new call before the load starts replaces the wanted extent. A call while a load runs starts
  one more load after it, as a second reload request does today.
- A hot reload loads at the wanted extent. A reload and a size change that are both due are one
  load, and it emits `Changed`: the pixels may differ. A swap emits one event.
- A memory-registered texture takes no size change in this version (one K5012, as a reload): kiln
  gives its bytes up once they are on the GPU, and keeping them would double the memory of every
  registered texture (open point (h)).

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
| `State::Partial` | reserved for progressive loads | Removed from the enum with step 2 (owner, 2026-10-09; an API break for the changelog). Texture arrays never used it: an array is one upload and `Ready` as one object |
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

### Measured (2026-10-09)

Corpus and machine as in `async-read-path.md` §9: 891 assets (154 meshes, 90 MiB; 737 textures,
622 MiB), i7-9700K, NVMe, MSVC release. `kiln_bench_load --max-extent N` computes the first figure
from the probed level tables (`tests/bench_load.cpp`, in the repo). The other two come from a
prototype of the level limit in the loader: one extent per context, the metadata step drops the
levels above the first that fits, and the upload plan starts there. The patch is kept outside the
repo (`C:\tmp\kiln-bench\level-limited-prototype.patch`, with the runs under `ll\`); it is not the
API of this note.

**Bytes.** "Read" is the stored bytes of the levels loaded; "Zstd" the bytes that come out of the
decoder; "upload" the texel bytes handed to the adapter; "store" adds the meshes, which do not
change.

| Extent | Textures limited | Read | Zstd out | Upload | Store read |
|---|---|---|---|---|---|
| full | 0 of 737 | 622 MiB | 1389 MiB | 1440 MiB | 712 MiB |
| 1024 | 248 | 261 MiB (42%) | 486 MiB (35%) | 531 MiB (37%) | 350 MiB (49%) |
| 256 | 519 | 25 MiB (4%) | 42 MiB (3%) | 46 MiB (3%) | 115 MiB (16%) |
| 64 | 709 | 2.3 MiB (0.4%) | 3.1 MiB (0.2%) | 3.4 MiB (0.2%) | 92 MiB (13%) |

**Time to `Ready` of the textures,** p50 / max in ms from the request, every asset requested at once,
pump at 60 Hz with the default 64 MiB upload budget, the fastest of 3 runs. The meshes load whole in
every row, so at 64 and 256 they bound the wall time (89.6 MiB of payload; warm, 7 workers: meshes
`Ready` max 84 ms at extent 64).

| Extent | Warm, 7 workers | Cold, 7 workers | Warm, 2 workers | Cold, 2 workers |
|---|---|---|---|---|
| full | 300 / 467 | 367 / 567 | 617 / 933 | 950 / 1400 |
| 1024 | 135 / 217 | 267 / 400 | 250 / 417 | 517 / 817 |
| 256 | 70 / 86 | 134 / 167 | 117 / 167 | 334 / 400 |
| 64 | 51 / 67 | 117 / 150 | 84 / 117 | 234 / 283 |

At 1000 Hz with no upload limit (warm, 7 workers): full 225 / 376, 1024 86 / 152, 256 33 / 60,
64 28 / 45 ms. The 60 Hz figures above hold a pump's granularity (16.7 ms) per step.

**A change from 64 to full,** measured as two whole loads in two contexts, which is an upper bound (a
swap keeps the handle, and the metadata step and the coarse levels are 0.4% of the bytes): warm, 7
workers, 60 Hz: 84 + 467 = 551 ms against 467 ms for one full load (+18%); at 1000 Hz 61 + 377 = 438
against 377 ms (+16%).

What the numbers say:

- **The bytes follow the level geometry.** Half the extent is a quarter of the bytes: 1024 reads 42%
  of the texture bytes, 256 reads 4%. On this corpus 248 textures exceed 1024 and 709 exceed 64.
- **Time follows the bytes while the workers are the limit.** Warm with 7 workers, 1024 is 2.2x
  faster to the last texture and 64 is 7x; with 2 workers 2.2x and 8x. Cold, the per-file costs
  stay: at 64 a load still opens every file twice and reads 737 prefixes (the meta job is 0.56 ms per
  asset cold against 0.06 ms warm), so 64 is 3.8x faster, not 7x. One-pass loads (R27) would take
  that cost out.
- **Below 256 the textures stop being the limit.** At 64 the upload jobs take 314 ms of worker time
  for 891 assets, the meshes are the last `Ready`, and the pump rate sets the floor: 61 ms at
  1000 Hz against 84 ms at 60 Hz.
- **The coarse-first pattern costs about a sixth more work** than one full load, for a first frame
  at 51 ms instead of 300 ms (p50, warm). Worth documenting for hosts; whether the second load
  should skip the metadata step is R27's question.
- **Memory.** At 64 the payload and staging peak falls from 1616 MiB to 180 MiB (the meshes, and
  the null adapter holds every object).

## Delivery sequence

1. The benchmark option and the figures above. **Done 2026-10-09** (`--max-extent`; the timings
   from a prototype kept outside the repo, see "Measured").
2. A level-limited first load: `RequestOptions::maxExtent`, the upload plan from level `N`,
   `TextureDesc::firstLevel`, `TextureInfo`, the null adapter, texture arrays. Tests compare the
   upload with the golden file's levels byte for byte. **Done 2026-10-09** (`State::Partial`
   removed with it; `kiln_bench_load --max-extent` now loads at the extent, and the figures under
   "Measured" hold).
3. A size change as a swap: `set_texture_extent`, `Resized`, the rules for calls that overlap a
   load or a reload. Tests with a held job system. **Done 2026-10-09.**
4. `residentBytes` and the `ContextStats` sums. **Done 2026-10-09.**
5. One example: `kiln-viewer` loads coarse first, then full.

Beside it, as their own decisions: one-pass texture loads from manifest metadata (R27) and group
readiness (R25).

## Open points (R30; owner, 2026-10-09)

- **(a) One wanted extent per asset, last call wins.** Decided as proposed. The alternative keeps a
  want per request and loads the largest, which needs a token per request.
- **(b) An extent in pixels at the API, a level at the adapter.** Stays proposed (owner: a tricky
  question; keep the proposed shape for now). The alternative is a level index at both, which a
  host cannot choose before `MetaReady`.
- **(c) A size change emits its own event kind, `Resized`,** on the same swap path as a reload:
  the content version increments, one event per swap, and a reload that coincides with a size
  change emits `Changed`. Decided (owner asked for the hybrid): a host that does not care rebinds
  on both; a host that cares can tell them apart.
- **(d) `TextureInfo::desc` stays the file's description** and `firstLevel` tells what is resident.
  Decided as proposed.
- **(e) Remove `State::Partial`** with step 2. Decided: a value that is never produced invites a
  case for it in every host, and progressive fill is no longer planned. Texture arrays are not
  affected: an array is one upload and `Ready` as one object, and every layer starts at level `N`.
- **(f) Range uploads for texture arrays move to Unscheduled.** Decided. Arrays stay as in R12: one
  aggregate upload, a whole-array reload on any layer change (117 ms for 64 layers on the
  benchmark).
- **(g) No budget enforcement and no eviction policy in kiln.** Decided as proposed.
- **(h) A memory-registered texture takes no size change** (found in step 3, 2026-10-09). kiln
  releases the registered bytes once the upload is done, so there is nothing to load the other
  levels from. The alternative keeps the bytes of every registered texture for its lifetime, or
  keeps them only when the host asks. Proposed: no size change, one K5012, until a host needs it.
