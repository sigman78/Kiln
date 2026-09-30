# Cook settings

See [`design/settings.md`](design/settings.md) for the settings structs as implemented, the
resolution layers (which ones exist today), sidecars, and the store-key hashing rule. This file
holds the full model that the v0.7 project-settings work fills in ([`ROADMAP.md`](ROADMAP.md)).

## Principles

- **Separate intent from encoding.** A preset says what a texture *is* (sRGB color with cutout
  alpha, tangent-space normal, packed ORM mask). A target says how that kind of texture is encoded
  on a platform. Adding a platform touches only target tables, never assets.
- **Inference covers most cases.** The glTF material slot already implies usage: baseColor is
  color (sRGB), normalTexture is normal, metallicRoughness is ORM (linear). Rules and sidecars are
  for exceptions.
- **Only resolved values are hashed.** Editing the mobile target does not invalidate desktop
  outputs.
- **The C++ structs are the real interface.** Config files are a thin layer parsed only by
  `kiln_cook`; a host can skip them and pass structs directly.
- **Unknown keys and invalid combinations are cook errors**, for example premultiplied alpha with
  no alpha, or BC5 on a color texture.
- **`kiln-cook --explain <asset> --target <t>`** prints which layer set each field.
- **Per-part mesh overrides** may come from glTF `extras` on nodes (for example `lod_ratio`,
  `no_simplify`), which artists set in their DCC tool.

## Planned layers

From weakest to strongest; `design/settings.md` maps them to what exists today.

```
1. built-in defaults
2. inferred usage       (glTF material slot -> color, normal, orm, ...)
3. project presets      (named intents: color, color_masked, normal, mask, hdr, ui, lut, height ...)
4. target encodings     (per target, per preset: desktop color -> BC7, mobile color -> ASTC 6x6 ...)
5. path rules           (glob -> preset/overrides: "ui/**" -> ui)
6. per-asset sidecar    (hull_albedo.png.kiln / ship_hauler_a.glb.kiln)
7. session overrides    (dev fast profile, CLI flags, host-supplied structs)
        |
resolved settings struct -> hashed into the build key
```

## Setting groups (full model)

**Texture:**

| Group | Settings |
|---|---|
| Semantics | color space; channel packing (for example ORM from separate images); swizzle; normal-map handling (renormalize, green flip, 2-channel storage) |
| Alpha | none / mask (cutoff, coverage-preserving mips) / blend; premultiplied or straight; UV-island dilation |
| Encoding | format per target (BC1/4/5/6H/7, ASTC block size, ETC2, raw); quality/effort; RDO lambda |
| Supercompression | none / Zstd level / Basis (ETC1S/UASTC) |
| Mips | full / none / max levels / min level size; filter kernel; wrap hint for edge filtering |
| Size | max size per target, downscale bias, power-of-two policy |
| Streaming | resident-minimum mip count; priority class |
| Shape | 2D / array / cube / 3D |
| Sampler hints | wrap and filter from glTF samplers, carried as metadata; the renderer may ignore them |

**Mesh:**

| Group | Settings |
|---|---|
| Encoding | vertex profile, quantization tolerances, index width policy |
| Processing | tangent generation, vertex cache / overdraw / fetch optimization, weld tolerance |
| LOD | generate yes/no; count or target ratios; error thresholds; attribute weights; lock borders; or authored `_lodN` only |
| Compression | none / basic (Zstd + byte shuffle) / meshopt / meshopt + Zstd; Zstd level; blob chunk size; per target and per asset |
| Import | unit/axis override, prefixes to strip, merge parts |
