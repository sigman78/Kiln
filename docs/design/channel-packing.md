# Channel packing

**Status:** Proposed (2026-10-01). Nothing is implemented.
**Decides:** how the mesh cook merges a glTF material's separate occlusion and
metallic-roughness images into one ORM texture (part 1, v0.7), and the shape of per-image
channel operations (part 2, a follow-up).

## Problem

glTF gives occlusion and metallic-roughness their own texture slots. Occlusion reads the R
channel; metallic-roughness reads G (roughness) and B (metallic). Many exporters write one ORM
image and reference it from both slots, and the cooker already handles that: both bindings name
one texture. But many models carry AO as a separate image. Then the cooker writes two textures,
both with usage `Orm`:

- The host loads, binds and samples two textures where one would do.
- The AO texture keeps one channel of data in a 4-channel format (BC7 on `desktop` and `compat`),
  so three channels are wasted.

Sources outside glTF have other layouts (gloss instead of roughness, Unity's mask map with
smoothness in alpha). A renderer wants one layout. That is part 2.

## Part 1: ORM packing in the mesh cook (v0.7)

### Rule

When a material references an occlusion image and a different metallic-roughness image, the
mesh cook writes one packed texture and points both bindings at it, if all of these hold:

1. Both images are embedded (in a `.glb`, or a `.gltf` data URI). External images are assets of
   their own, so the mesh cook does not cook them (`asset-model-next.md`).
2. Both bindings use the same UV set.
3. Both images decode to the same width and height.

If a condition fails, the two images are cooked separately, as now, and the cook emits an Info
diagnostic that names the material and the reason. A failed condition is never an error.

### Channels

| Output | From |
|---|---|
| R | occlusion image, R |
| G | metallic-roughness image, G |
| B | metallic-roughness image, B |
| A | 1.0 |

This is glTF's ORM layout, so the packed texture is correct for both slots without any host
change: the occlusion binding still reads R, the metallic-roughness binding still reads G and B.
The packed texture has usage `Orm`, linear, and is encoded as any ORM texture of the target
(BC7 on `desktop` and `compat`).

### Names and outputs

- The packed texture is an embedded output of the mesh, named `<mesh>#<mr name>+<ao name>`,
  where the names are the glTF image names that sub-assets already use (I4 in
  `asset-model-next.md`). A real image with that name is the duplicate-name error (K1019) that
  exists today.
- One packed output per distinct (occlusion, metallic-roughness) pair. Two materials with the same
  pair share it.
- An image is still cooked on its own when some binding uses it unpacked: for example a
  material with metallic-roughness and no occlusion, or a pair that failed a condition.
- An image that every binding uses packed is **not** written as an output of its own. The mesh's
  outputs stay "what its bindings name". A direct request for `<mesh>#<ao name>` then fails with
  NotFound, the same as for an image no material uses today.

### Settings

- `MeshCookSettings::packOrm`, default `true`. The sidecar key is `packOrm`. `kiln-cook` gets
  `--pack-orm on|off`.
- The flag is in the mesh settings hash. Turning it on changes the outputs of models with separate
  AO, so those models cook again once. Models without separate AO get new keys too, because the
  hash changes. That is acceptable before 1.0; the CHANGELOG says so.
- The packed texture's settings resolve like any embedded image's, with slot hint
  `MetallicRoughness`. Its settings hash also covers the channel map (part 2's form, below), so a
  packed texture and an unpacked one never share a key.

### Cook path

- `TextureRef` (`kiln/cook/cook.h`) gains a second input: the occlusion image's bytes and MIME
  type, and the channel map. `cook_embedded_images` decodes both inputs and composes them before
  mips, alpha coverage and encoding. Composition is a new kernel step that takes the decoded
  images and a map. Part 2 reuses it.
- Both inputs are LDR. A 16-bit PNG input decodes as today; the composer works on the decoder's
  output format and converts only when the two inputs differ.
- Both images come from the same source file, so the unit's records and hot reload need no change:
  an edit to the glb re-cooks the unit.

### What does not change

- The `.mesh` format: two bindings may already name one texture. The sRGB bit is 0 on both.
- The runtime, the adapters, the examples and the placeholders (`TextureKind::Orm`: AO 1,
  roughness 1, metallic 0 is already the ORM layout).
- Texture arrays, the store and the manifest.

### Diagnostics

| Code | Severity | Meaning |
|---|---|---|
| K10xx (assigned at implementation) | Info | occlusion and metallic-roughness of material M stay separate: an image is external, the UV sets differ, or the sizes differ |

### Tests and measurement

- A generated glb with separate AO and metallic-roughness images: both bindings name the packed
  texture; on the `uncompressed` profile its texels equal the composed channels byte for byte.
- Each failed condition gives two textures and the Info diagnostic.
- Two materials with the same pair share one output; an image used both packed and unpacked is
  written once on its own and once inside the packed texture.
- Golden files that change are listed in the commit.
- Measure on the Khronos demo models and the Pacer: textures per material and bytes cooked,
  before and after. The note records the numbers.

### Rejected

- **Resize the smaller image to fit.** Hides an authoring mistake and needs a resampler choice;
  keep separate images with an Info diagnostic instead.
- **Lone AO as a `Mask` texture (BC4).** Saves half on `desktop`, but `compat` has no BC4 and
  would use BC5, which saves nothing. The host also requests that texture with kind `Orm`.
  Revisit with the target usage tables if a model needs it.
- **Pack metallic-roughness into two channels (BC5).** Moves G and B to R and G, which changes what
  the host shader reads. Not a cook-only change.
- **Pack external images of a `.gltf`.** Needs the mesh cook to cook other assets; that is a
  recipe file (below).

## Part 2: channel operations on one image (follow-up)

Not scheduled for v0.7. Recorded so part 1's composer takes a shape part 2 can reuse.

- A texture setting `channels`: four characters, one per output channel R, G, B, A. Each is
  `r`, `g`, `b`, `a` (an input channel) or `0`, `1` (a constant). The default `rgba` is the
  identity and leaves the settings hash unchanged, as `alphaCutoff` off does.
- A texture setting `invert`: the output channels to replace with `1 - value`, for example `"g"`.
- Both work in sidecars, so they apply to standalone images. Examples: gloss to roughness
  (`invert = "g"`); Unity's HDRP mask map (R metallic, G AO, A smoothness) to ORM
  (`channels = "gab1"`, `invert = "g"`).
- The operations run after decode, before mips and alpha coverage, in the same step as part 1's
  composition. Part 1's map is the two-input form of the same thing.
- Embedded images have no sidecar, so part 2 reaches them only through project config path rules
  once those exist (`cook-settings.md`).

Open for the follow-up: whether `channels` also accepts a usage-level preset (`"orm-from-mask-map"`),
and whether an inverted or swizzled image may keep an sRGB color space (proposed: no, error).

## Later: recipe textures

A texture made from several image files (for example `rock_orm.pack.kiln` naming one input per
channel) is a new source type with several recorded inputs. It needs the project config file
format (open question 7), so it waits for that. It would reuse part 2's channel syntax and part
1's composer.

## Open points for the owner

1. `packOrm` on by default (proposed), or opt-in.
2. Do not write an image as its own output when every binding uses it packed (proposed), or
   always write it.
3. The packed name `<mesh>#<mr name>+<ao name>`.
4. Mismatched sizes: Info and keep the images separate (proposed), or Warning.
