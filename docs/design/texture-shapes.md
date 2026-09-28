# Texture shapes: cube maps, arrays and volumes

**Status:** Draft (2026-09-28), awaiting owner sign-off. Nothing is implemented.
**Decides:** How one source image becomes a cube map, a texture array or a volume texture; the
settings, name hints and sidecar keys for it; and what the runtime and the adapter need to carry a
texture's shape.

## Summary

A composite texture comes from **one source image cut into slices**: a vertical strip, slice 0 at
the top. One file still gives one cooked asset (`asset-model-next.md`). A KTX2 source that already
has faces, layers or depth passes through. The shape travels from the cook to the adapter, and a
request can say which shape it expects, so the placeholder has the right type from the start.

## Terms

| Shape | KTX2 | Slices are | Vulkan view |
|---|---|---|---|
| `Tex2D` | 1 face, no layers, depth 1 | one image | `2D` |
| `Cube` | 6 faces | faces `+X −X +Y −Y +Z −Z`, square | `CUBE` |
| `Array` | `layerCount = N` | layers 0 to N−1 | `2D_ARRAY` |
| `Volume` | `pixelDepth = N` | depth slices z = 0 to N−1 | `3D` |

Cube arrays are out of scope for now: they need a second slice axis.

## Decision

### 1. Source layout: a vertical strip

- The source is one image of `width × (height × N)`. Slice `i` is rows `i × height` to
  `(i + 1) × height − 1`.
- **Why vertical:** a decoded image is row-major, so each slice is one contiguous block of the
  pixel buffer. Cutting slices copies nothing; the mip chain of each slice can run as its own job.
- A cube strip is 6 square faces: `height = 6 × width`. Any other size is an error.
- An array or volume strip has `slices = N`. With `slices = 0`, square slices are assumed:
  `N = height / width`, and `height` must divide evenly.
- An invalid layout is the new error **K2010** (`kDiagImageSliceLayout`): the strip does not divide
  into the slices, or a cube's faces are not square.

**About "progressive":** PNG decodes top to bottom, so a later change can decode and convert one
slice at a time and keep less than the whole image in memory. That helps the cook. It does not
help loading: KTX2 stores data by mip level, with every face and layer inside each level, so a
loader cannot stream one slice at a time without a different file layout.

### 2. Settings, name hints, sidecar keys

New fields in `TextureCookSettings` (texture settings schema 2):

| Field | Default | Meaning |
|---|---|---|
| `shape` | `Auto` | `Auto`, `Tex2D`, `Cube`, `Array`, `Volume`. `Auto` resolves to the shape of a KTX2 source, else `Tex2D` |
| `slices` | 0 | layers or depth slices in the strip; 0 = square slices (ignored for `Tex2D` and `Cube`) |

- **Resolution:** `shape` follows the layer rules of `settings.md`. Name rules fill it only when it
  is `Auto` (layer 5), like `usage`.
- **Name hints:** a name rule gains an optional shape. The default rules add `_cube` (Cube),
  `_array` (Array) and `_vol` (Volume). Rules match **a chain of suffixes** from the end of the
  stem, each rule at most once: `sky_cube.png` is a Cube of usage Color, `rock_array_n.png` is an
  Array of normal maps. Today one suffix is matched; this is a behavior change.
- **Sidecar keys:** `shape = "cube"`, `slices = 16`.
- **Mips:** per slice, with the existing `build_mip_chain`. Cube faces are filtered on their own,
  with no seam correction; seamless filtering is a later option. A `Volume` needs a 2×2×2 filter,
  which comes later: until then `genMips` is cleared for a volume with a K3002 warning.
- **Limits:** `maxSize` and the target's `maxTextureSize` apply to each slice. New
  `TargetProfile` fields: `maxArrayLayers` (default 2048) and `maxVolumeSize` (default 2048, the
  largest of width, height and depth). A larger strip is an error, not a clamp: dropping slices
  would change what the texture means.

### 3. KTX2 writing and pass-through

- `ktx2::WriteDesc` gains `layers`, `faces` and `depth`. Each level span holds all slices of that
  level in KTX2 order (layer, then face, then z).
- A KTX2 source whose shape matches the resolved shape passes through. With `shape = Auto` it passes
  through with its own shape. Today the cook rejects any KTX2 that is not plain 2D.

### 4. Runtime and adapter

- **The adapter learns the shape.** The runtime folds cube faces into `layers` at the adapter
  boundary today, so an adapter cannot tell a cube from a 6-layer array. `TextureDesc` in
  `kiln/adapter.h` gains `TextureShape shape`; `layers` keeps the folded count.
- **A request says the shape it expects.** `acquire()` runs at request time, before the metadata is
  read, and it picks the placeholder. A bindless slot of cube type cannot hold a 2D placeholder.
  So `RequestOptions` gains `shape` (default `Tex2D`), which works like `textureKind`: the first
  request wins, and `acquire()` receives it.
- **A mismatch fails the load.** When the cooked file has another shape than the request, the
  asset fails with the new **K5017** (`kDiagTextureShapeMismatch`). A hot reload that changes the
  shape fails with K5010 and keeps the old version.
- **Placeholders per shape.** One per (kind, shape): a 1×1 image of the right type (a cube of six
  1×1 faces, an array of one layer, a 1×1×1 volume). With the failed placeholders that is 20
  objects, more than the reserved ids 1..15 hold, so the reserved range grows to 1..63
  (`handles-and-states.md`).

## Rationale

- Strips keep "one file, one asset": the file is the unit that is cooked, reloaded and named.
  Six face files would need kiln to follow references between files, which the retrospective in
  `asset-model-next.md` rules out.
- A shape hint in the request keeps the placeholder guarantee: a texture is usable from the moment
  it is requested. Waiting for the metadata before `acquire()` would break that.
- Errors instead of clamps for the slice count and the shape: a wrong shape is a content bug that
  a clamp would hide.

## Alternatives considered

| Alternative | Why not now |
|---|---|
| Horizontal strips | Needs a strided copy per slice; otherwise the same. Common for color LUTs (a 256×16 strip is a 16³ volume). A `stripAxis` setting can add it later. |
| Cross layouts (4×3) for cubes | Another slicing rule; add when a real source needs it. |
| One file per face or layer | Breaks "one file, one asset" and needs reference tracking. |
| Equirectangular sources for cubes | A resampling step, not slicing. Usually HDR, and kiln has no HDR decoder yet. |
| `acquire()` after the metadata | No placeholder until the metadata is read; breaks the handles contract. |

## Rollout

1. **Runtime and adapter plumbing:** `TextureShape`, `shape` in the adapter's `TextureDesc`,
   `RequestOptions::shape`, K5017, placeholders per shape, the reserved id range. KTX2 cube, array
   and volume files already load; tests use the null adapter.
2. **KTX2 writer and pass-through** for all four shapes.
3. **Strip slicing in the texture cook:** `shape`, `slices`, K2010, per-slice mips, the target
   limits.
4. **Name hints and sidecar keys:** suffix chains, `_cube` `_array` `_vol`, `shape` and `slices`
   keys.
5. **Viewer:** cube display (a skybox pass) so that step 3 can be checked by eye. Until then the
   checks are unit tests and `kiln-info`.

Later: 3D mips for volumes, seamless cube filtering, horizontal strips, slice-by-slice decode.

## Open points for the owner

- Confirm vertical strips with slice 0 at the top, and the cube face order `+X −X +Y −Y +Z −Z`.
- Confirm `RequestOptions::shape` as a hint with "first request wins", and K5017 on a mismatch.
- Confirm suffix chains in the name rules (a behavior change) and the hints `_cube`, `_array`,
  `_vol`. Should `_lut` also mean Volume?
- Confirm errors, not clamps, for too many slices, and the defaults 2048 for `maxArrayLayers` and
  `maxVolumeSize`.
- Growing the reserved placeholder ids from 1..15 to 1..63.
