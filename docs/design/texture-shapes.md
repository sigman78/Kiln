# Texture shapes: cube maps and arrays

**Status:** Decided in part (owner, 2026-09-28): vertical strips, the cube face order below, no
volumes for now. Step 1 of the rollout is implemented; the other steps await sign-off where marked.
**Decides:** How one source image becomes a cube map or a texture array; the settings, name hints
and sidecar keys for it; and how the runtime and the adapter carry a texture's shape.

## Summary

A composite texture comes from **one source image cut into slices**: a vertical strip, slice 0 at
the top. One file still gives one cooked asset (`asset-model-next.md`). A KTX2 source that already
has faces or layers passes through. The shape travels from the cook to the adapter, and a request
says which shape it expects, so the placeholder has the right type from the start.

## Terms

| Shape | KTX2 | Slices are | Vulkan view |
|---|---|---|---|
| `Tex2D` | 1 face, no layers | one image | `2D` |
| `Cube` | 6 faces | faces `+X −X +Y −Y +Z −Z`, square | `CUBE` |
| `Array` | `layerCount = N` | layers 0 to N−1 | `2D_ARRAY` |

Volumes (3D textures) and cube arrays are out of scope. The KTX2 reader rejects 3D files (K4105).

## Decision

### 1. Source layout: a vertical strip *(decided)*

- The source is one image of `width × (height × N)`. Slice `i` is rows `i × height` to
  `(i + 1) × height − 1`.
- **Why vertical:** a decoded image is row-major, so each slice is one contiguous block of the
  pixel buffer. Cutting slices copies nothing; the mip chain of each slice can run as its own job.
- A cube strip is 6 square faces in the order `+X −X +Y −Y +Z −Z` (KTX2 and Vulkan order):
  `height = 6 × width`. Any other size is an error.
- An array strip has `slices = N`. With `slices = 0`, square slices are assumed:
  `N = height / width`, and `height` must divide evenly.
- An invalid layout is the new error **K2010** (`kDiagImageSliceLayout`): the strip does not divide
  into the slices, or a cube's faces are not square.

**About "progressive":** PNG decodes top to bottom, so a later change can decode and convert one
slice at a time and keep less than the whole image in memory. That helps the cook. It does not
help loading: KTX2 stores data by mip level, with every face and layer inside each level, so a
loader cannot stream one slice at a time without a different file layout.

### 2. Settings, name hints, sidecar keys *(proposed)*

New fields in `TextureCookSettings` (texture settings schema 2):

| Field | Default | Meaning |
|---|---|---|
| `shape` | `Auto` | `Auto`, `Tex2D`, `Cube`, `Array`. `Auto` resolves to the shape of a KTX2 source, else `Tex2D` |
| `slices` | 0 | layers in an array strip; 0 = square slices (ignored for `Tex2D` and `Cube`) |

- **Resolution:** `shape` follows the layer rules of `settings.md`. Name rules fill it only when it
  is `Auto` (layer 5), like `usage`.
- **Name hints:** a name rule gains an optional shape. The default rules add `_cube` (Cube) and
  `_array` (Array). Rules match **a chain of suffixes** from the end of the stem, each rule at most
  once: `sky_cube.png` is a Cube of usage Color, `rock_array_n.png` is an Array of normal maps.
  Today one suffix is matched; this is a behavior change.
- **Sidecar keys:** `shape = "cube"`, `slices = 16`.
- **Mips:** per slice, with the existing `build_mip_chain`. Cube faces are filtered on their own,
  with no seam correction; seamless filtering is a later option.
- **Limits:** `maxSize` and the target's `maxTextureSize` apply to each slice. A new
  `TargetProfile::maxArrayLayers` (default 2048). A larger strip is an error, not a clamp: dropping
  slices would change what the texture means.

### 3. KTX2 writing and pass-through *(proposed)*

- `ktx2::WriteDesc` gains `layers` and `faces`. Each level span holds all slices of that level in
  KTX2 order (layer, then face).
- A KTX2 source whose shape matches the resolved shape passes through. With `shape = Auto` it passes
  through with its own shape. Today the cook rejects any KTX2 that is not plain 2D.

### 4. Runtime and adapter *(implemented)*

- **`TextureShape { Tex2D, Cube, Array }`** in `kiln/adapter.h`, with `texture_shape_name()`.
- **The adapter learns the shape.** `TextureDesc::shape` in `kiln/adapter.h`; `layers` keeps the
  folded count (6 for a cube).
- **The adapter declares the shapes it takes:** `AdapterCaps::kCubeTextures`, `kArrayTextures`.
  Without the bit, kiln uploads no placeholder of that shape and a request for it fails with K5004.
  The null adapter sets both; the example Vulkan adapter sets neither until it can bind cube and
  array views.
- **A request says the shape it expects.** `RequestOptions::textureShape` (default `Tex2D`) works
  like `textureKind`: the first request wins, and `acquire()` receives it. `acquire()` runs at
  request time, before the metadata, and a bindless slot of cube type cannot hold a 2D placeholder.
- **A mismatch fails the load** with the new **K5017** (`kDiagTextureShapeMismatch`), checked at
  the meta stage before the level checks. A hot reload that changes the shape fails with K5010 and
  keeps the old version.
- **Placeholders per shape:** one per (kind, shape) plus the Failed checker per shape: a cube of six
  1×1 faces, an array of one layer. They fill the reserved ids exactly: 1..12 are the kinds (four
  per shape), 13..15 the Failed checkers (`placeholder_asset_id(kind, shape)`,
  `failed_placeholder_id(shape)` in `kiln/placeholders.h`).

## Rationale

- Strips keep "one file, one asset": the file is the unit that is cooked, reloaded and named.
  Six face files would need kiln to follow references between files, which the retrospective in
  `asset-model-next.md` rules out.
- A shape hint in the request keeps the placeholder guarantee: a texture is usable from the moment
  it is requested. Waiting for the metadata before `acquire()` would break that.
- Capability bits let an adapter that binds only 2D views keep working unchanged.
- Errors instead of clamps for the slice count and the shape: a wrong shape is a content bug that
  a clamp would hide.

## Alternatives considered

| Alternative | Why not now |
|---|---|
| Horizontal strips | Needs a strided copy per slice; otherwise the same. A `stripAxis` setting can add it later. |
| Cross layouts (4×3) for cubes | Another slicing rule; add when a real source needs it. |
| One file per face or layer | Breaks "one file, one asset" and needs reference tracking. |
| Equirectangular sources for cubes | A resampling step, not slicing. Usually HDR, and kiln has no HDR decoder yet. |
| `acquire()` after the metadata | No placeholder until the metadata is read; breaks the handles contract. |
| Volumes | Not needed now (owner). Would need 3D mips and a reader change. |

## Rollout

1. **Runtime and adapter plumbing** *(done)*: `TextureShape`, the adapter's `TextureDesc::shape`,
   `kCubeTextures` / `kArrayTextures`, `RequestOptions::textureShape`, K5017, placeholders per
   shape. KTX2 cube and array files load through the null adapter.
2. **KTX2 writer and pass-through** for cubes and arrays.
3. **Strip slicing in the texture cook:** `shape`, `slices`, K2010, per-slice mips, the array limit.
4. **Name hints and sidecar keys:** suffix chains, `_cube` and `_array`, the `shape` and `slices`
   keys.
5. **Viewer:** cube and array views in the example adapter, and a skybox pass, so that step 3 can
   be checked by eye. Until then the checks are unit tests and `kiln-info`.

Later: seamless cube filtering, horizontal strips, slice-by-slice decode.

## Open points for the owner

- Confirm suffix chains in the name rules (a behavior change) and the hints `_cube`, `_array`.
- Confirm errors, not clamps, for too many slices, and the default 2048 for `maxArrayLayers`.
