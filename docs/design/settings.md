# Cook settings structs (v0.5 subset)

**Status:** Proposed (awaiting owner sign-off). Implemented in M2: `include/kiln/cook/settings.h`
(namespace `kiln::cook`), `src/cook/settings.cpp`.
**Decides:** The v0.5 cook settings structs, which resolution layers exist in v0.5, and how
resolved settings are hashed into the store key.

## Decision

The C++ structs are the real interface. Config files, presets and rules come in v0.6 as layers that
produce the same structs.

### Texture

`ColorSpace { Auto, Srgb, Linear }`, `TextureUsage { Auto, Color, Normal, Orm, Mask, Hdr, Ui, Lut,
Height }`, `CookShape { Auto, Tex2D, Cube, Array }`.

| `TextureCookSettings` field | Default | Meaning |
|---|---|---|
| `colorSpace` | `Auto` | Auto: sRGB for `Color` and `Ui`, else linear |
| `usage` | `Auto` | Auto: inferred from the glTF slot, else `Color` |
| `genMips` | true | |
| `normalRenormalize` | true | `Normal` only |
| `maxSize` | 0 | 0 = the target cap |
| `flipGreen` | false | DirectX-style normal maps; `Normal` only |
| `shape` | `Auto` | `Cube` and `Array` cut the source into a vertical strip of slices (`texture-shapes.md`). `Auto` stays `Auto` after resolution and means "from the source": a KTX2 source's own shape, else `Tex2D` |
| `slices` | 0 | `Array` only: layers in the strip; 0 = square slices. Cleared with a K3002 warning for other shapes |

Reserved: alphaMode, premultiply, dilation, encoding, supercompression, residentMips.
Texture settings schema: 2 (`shape`, `slices`).

### Mesh

`VertexProfile { Default, Precise }`, `CompressionScheme { None, Basic, Meshopt, MeshoptZstd }`.

| `MeshCookSettings` field | Default | Meaning |
|---|---|---|
| `profile` | `Default` | vertex format profile (mesh-format-spec §6) |
| `genTangents` | true | MikkTSpace |
| `optimize` | true | vertex cache, overdraw, vertex fetch |
| `useAuthoredLods` | true | pass through `_lodN` nodes |
| `genLods` | false | reserved: the simplifier lands in v0.6 |
| `posTolMm` | 0.1 | quantization tolerance before falling back to float positions |
| `weldTol` | 0 | 0 = exact-match welding only |
| `compression` | `None` | `None`: every blob codec `None`, the cooker sets `kPayloadRaw`. `Basic`: Zstd + ByteShuffle (vertex), Zstd (index). `Meshopt`: MeshoptVertex / MeshoptIndex. `MeshoptZstd`: Meshopt + `kBlobOuterZstd` |
| `zstdLevel` | 0 | 0 = library default; used by `Basic` and `MeshoptZstd` |
| `blobChunkSize` | 0 | reserved: decoded bytes per split blob (spec §5.9 split rule); 0 = one blob per stream / index buffer per LOD |

Reserved: indexWidthPolicy, unit/axis override, name prefixes to strip.

**Compression group (Proposed):**

- The fields exist from v0.5 so the struct and the hash layout do not change when codecs land.
- Only `None` is accepted in v0.5; other schemes and a non-zero `blobChunkSize` are K3001 errors.
  A non-zero `zstdLevel` is a K3002 warning and is ignored.
- The schemes map to the candidate schemes in mesh-format-spec §5.9.
- Later the scheme becomes selectable **per target** (a default in `TargetProfile`) and **per
  asset** (presets, rules, sidecars in v0.6).
- The **default scheme is picked by measurement** (ratio and decode MB/s on real assets) in
  v0.6-0.7. Until then the default stays `None`.

### Target and session

- `TargetProfile { name = "desktop"; maxTextureSize = 16384; maxVertexProfile = Float; }`. v0.5
  has one implicit target, `desktop`, with raw formats.
- `StoreMode { Disk, Memory, None }`: store, cache-less, validate only.
- `CookSession { storeMode = Disk; fastPreview = false; }`. `fastPreview` turns off `optimize` and
  `genTangents` for meshes and changes nothing for textures. It changes resolved values, so it
  changes the hash; it is not a hidden side channel.

### Resolution layers

*Decided (owner, 2026-09-28).* General to specific, then the host's enforcement. Data layers set
values; code layers fill or override them. Each layer beats the ones above it.

| # | Layer | Kind | Sets | v0.5 |
|---|---|---|---|---|
| 1 | built-in defaults (the member initializers) | data | every field | **yes** |
| 2 | host settings: `ProviderDesc::textureDefaults` / `meshDefaults`, `kiln-cook` flags | data, base | what the host changes | **yes** |
| 3 | project config: presets, path rules (globs), target encodings | data, patch | the keys it names | reserved (v0.6) |
| 4 | per-asset sidecar (`<source>.kiln`) | data, patch | the keys it names | **yes** (see "Sidecar files") |
| 5 | inference: glTF slot, else name rules, else Color | code | `usage` and `shape`, only if `Auto` | **yes** |
| 6 | `CookPolicy` (optional) | code | anything; may refuse the asset (K3007) | **yes** |
| - | resolve: derived fields, validation, `CookSession`, target caps | kiln | `colorSpace` if `Auto`; clears; errors | **yes** |
| - | `kiln-cook --explain` (which layer set each field) | | | reserved (v0.6) |

Why this order:

- Layer 2 is a default, not a rule. "This project cooks without mips" yields to a sidecar that
  asks for mips on one asset. A rule the host enforces ("never mips") belongs in the policy.
- Layer 2 is a full struct applied as the base, so it needs no "unset" state. Layers 3 and 4 are
  patches: a key they do not name is unset.
- Inference runs after the sidecar, so an explicit `usage` wins, and before the policy, so the
  policy sees the real usage (e.g. "normal maps at most 2K").
- Fields derived from the usage (`colorSpace` when `Auto`, the Normal-only flags) are set after the
  policy. A policy that changes the usage gets a consistent result.
- The policy runs before validation and target caps: it cannot produce invalid settings or exceed
  the target.

`resolve_texture_layers(base, ResolveDesc)` and `resolve_mesh_layers(base, ResolveDesc)` run layers
4 to 6 and the resolve step over `base` (layers 1 to 3). The provider and `kiln-cook` both call
them, so a sidecar means the same in both.

**`CookPolicy`** (`kiln/cook/settings.h`): a texture function and a mesh function plus `void*
user`. It gets the asset name, source path, glTF slot and target, and changes the settings in
place or refuses the asset with a failed `Status`. It runs on worker threads, possibly
concurrently, so it must be thread-safe. It must be deterministic: the same asset, settings and
target give the same result. The store hashes the resolved settings, so a changed policy is
visible to future staleness checks. The policy is set in `ProviderDesc::policy`; offline, a
project's own cook tool passes it to `cook_cli_main()` (`kiln/cook/cli.h`), which is `kiln-cook`
with a policy.

Inference (layer 5, `usage_from_slot`, `usage_from_name`) and the derived color space (`color_space_for`):

| glTF slot | usage | colorSpace |
|---|---|---|
| `baseColorTexture` | Color | Srgb |
| `emissiveTexture` | Color | Srgb |
| `normalTexture` | Normal | Linear |
| `metallicRoughnessTexture` | Orm | Linear |
| `occlusionTexture` | Orm | Linear |
| standalone image (PNG, JPEG, WebP), no slot | name rule, else Color | from the usage |
| Radiance `.hdr`, no slot | Hdr (by the extension, before name rules) | Linear |

Name rules (`NameRule { suffix, usage, shape }`, `hints_from_name`) match suffixes of the file stem
that **stack**: after a match the suffix is removed and the rules match again, each rule at most
once. `rock_array_n.png` is an Array of Normal, `sky_cube.png` a Cube of Color. In each round the
first matching rule wins, and the first rule that sets a field wins it. The default rules add
`_cube` (Cube) and `_array` (Array). An embedded image has a slot and no name hints.

An image bound to two slots with different inferred usages is cooked with the first slot's usage
and a K1017 warning.

`resolve_texture(overrides, SlotHint, TargetProfile, CookSession, diag, asset)` and
`resolve_mesh(overrides, TargetProfile, CookSession, diag, asset)` are the resolve step on their
own; the layered functions call them last. All are pure functions. After resolution no field is
`Auto`. The cook functions receive resolved structs only.

### Sidecar files

A sidecar is `<source file name>.kiln` next to its source, e.g. `wall_n.png.kiln` or
`chair.glb.kiln`. It is part of the source: the provider's source poller re-cooks when a sidecar is
added, edited or removed. `apply_sidecar()` (`kiln/cook/sidecar.h`) applies one to a settings
struct; the provider and `kiln-cook` call it for standalone textures and for meshes. Embedded
images have no sidecar of their own.

**Syntax: a strict subset of TOML**, parsed by kiln (`src/cook/toml_subset.cpp`, no dependency).
Every accepted file is valid TOML.

- `key = value` lines, bare keys (`A-Z a-z 0-9 _ -`), `#` comments, blank lines, LF or CRLF, an
  optional UTF-8 BOM.
- Values: basic strings `"…"` (escapes `\b \t \n \f \r \" \\ \uXXXX \UXXXXXXXX`), literal
  strings `'…'`, decimal integers, floats with a fraction and/or exponent, `true`, `false`.
- `[a]` and `[a.b]` table headers are parsed; no key uses them yet, so a key inside a table is a
  K3006 error.
- Not supported (K3005): dotted and quoted keys, arrays, inline tables, arrays of tables,
  multi-line strings, dates and times, hex/octal/binary, underscores in numbers, `inf`, `nan`.
- A key or table defined twice is K3005, as in TOML.

**Keys are the struct field names.** An unknown key, a value of the wrong type, an unknown enum
name or an out-of-range number is K3006. An integer is accepted where a float is expected.

| Texture key | Value |
|---|---|
| `usage` | `"auto"`, `"color"`, `"normal"`, `"orm"`, `"mask"`, `"hdr"`, `"ui"`, `"lut"`, `"height"` |
| `colorSpace` | `"auto"`, `"srgb"`, `"linear"` |
| `genMips`, `normalRenormalize`, `flipGreen` | boolean |
| `maxSize` | integer, 0 to 2^32 - 1 |
| `shape` | `"auto"`, `"2d"`, `"cube"`, `"array"` |
| `slices` | integer, 0 to 2^32 - 1 |

| Mesh key | Value |
|---|---|
| `profile` | `"default"`, `"precise"`, `"float"` |
| `genTangents`, `optimize`, `useAuthoredLods` | boolean |
| `posTolMm`, `weldTol` | number |

Reserved fields (compression, `genLods`, …) are not sidecar keys yet.

A sidecar is layer 4: its keys beat the host settings, and only the policy beats a sidecar.

### Validation

| Rule | Result |
|---|---|
| enum field out of range | K3004 error |
| `flipGreen` with a usage other than `Normal` | K3002 warning; cleared. `normalRenormalize` is cleared silently |
| `maxSize` above the target cap | K3003 warning; clamped |
| `genLods = true` | K3001 error, unsupported |
| `compression` other than `None`, or `blobChunkSize != 0` | K3001 error, unsupported |
| `profile` above the target's `maxVertexProfile` | K3003 warning; clamped |
| `posTolMm <= 0` or NaN | K3002 error |
| `weldTol < 0` or NaN | K3002 error |
| `maxSize` not a power of two (when non-zero) | proposed error; not implemented |

### Hashing rule

Resolved settings are hashed with a **versioned, field-by-field serializer**: `hash_settings()`
for each struct and `hash_target()`, each starting with its schema constant
(`kTextureSettingsSchema`, `kMeshSettingsSchema`, `kTargetSchema`), bumped when a field is added or
changes meaning.

- **Never** hash the whole struct: padding bytes are indeterminate.
- Floats are hashed by bit pattern after folding `-0.0f` to `0.0f`.
- Strings (target name) are hashed as length followed by bytes.
- `test_settings.cpp` pins the hash of a default-constructed struct, so a silent layout change
  fails CI.
- Fields that the resolved settings do not use are hashed as 0 (`zstdLevel` unless the scheme uses
  Zstd), so changing an unused field does not miss the store.

### Store key

```
cookHash = hash_combine(hash_combine(settingsHash, targetHash), kCookerVersion)
key      = hash_combine(hash_combine(hash_combine(sourceHash, settingsHash), targetHash), kCookerVersion)
```

- `sourceHash` is `xxh64(source bytes)`. `kCookerVersion` (`u32`, `cook.h`) is bumped whenever
  cooker output can change for the same input.
- Both hashes are stored inside each cooked file: the `.mesh` header's `sourceHash` and `cookHash`
  (open-questions B3), and the KTX2 key/value entries `kiln.sourceHash` / `kiln.cookHash`.
- v0.5 stores files by name, `<storeDir>/<name>.mesh|.ktx2` (a named root's `m:` prefix becomes
  the top-level directory `@m/`), and invalidates by the stored hashes (R4). The hashed layout (16 lowercase
  hex digits of `key` plus the extension) is `kiln-cook --hashed`; it returns as the default with
  the index file in v0.6.

## Rationale

- Designated initializers with defaults let callers set only what they mean.
- `Auto` values make inference explicit and let resolution report which layer set a field later.
- Field-by-field hashing is deterministic across compilers and is the only safe way to hash
  structs with padding.

## Alternatives considered

Hashing the raw struct bytes (padding, silent key changes on reorder), a key/value string map (no
type checking, allocates), and `optional<T>` per field (heavy; `Auto` covers v0.5): rejected.

## Consequences / what this constrains later

- Adding a field means: default value, serializer line, schema version bump. Old store entries then
  miss and re-cook, which is correct.
- Reserved groups (alpha, encoding, supercompression, shape) are added as new fields, never by
  changing the meaning of existing ones.
- Enabling a compression scheme later needs no struct or schema change, only accepting the enum
  value. `kCookerVersion` still bumps if defaults change.
- Layers 3 and 4 are patches, so they know which fields they set. `--explain` can record the
  layer per field while patching, with no API change.

## Open points for the owner

- Confirm the field list and defaults above.
- Confirm `usage` includes `Ui`, `Lut`, `Height` in v0.5 even though they only affect color space,
  channel layout and mips for now.
- Confirm the inference table (`emissiveTexture` as sRGB color).
- Confirm the Compression group fields are declared in v0.5 with only `None` / 0 accepted.
