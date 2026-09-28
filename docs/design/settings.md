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
Height }`.

| `TextureCookSettings` field | Default | Meaning |
|---|---|---|
| `colorSpace` | `Auto` | Auto: sRGB for `Color` and `Ui`, else linear |
| `usage` | `Auto` | Auto: inferred from the glTF slot, else `Color` |
| `genMips` | true | |
| `normalRenormalize` | true | `Normal` only |
| `maxSize` | 0 | 0 = the target cap |
| `flipGreen` | false | DirectX-style normal maps; `Normal` only |

Reserved: alphaMode, premultiply, dilation, encoding, supercompression, residentMips, shape
(2D / array / cube / 3D).

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

- `TargetProfile { name = "desktop"; maxTextureSize = 16384; maxVertexProfile = Precise; }`. v0.5
  has one implicit target, `desktop`, with raw formats.
- `StoreMode { Disk, Memory, None }`: store, cache-less, validate only.
- `CookSession { storeMode = Disk; fastPreview = false; }`. `fastPreview` turns off `optimize` and
  `genTangents` for meshes and changes nothing for textures. It changes resolved values, so it
  changes the hash; it is not a hidden side channel.

### Resolution layers

Weakest to strongest:

| # | Layer | v0.5 |
|---|---|---|
| 1 | built-in defaults (the member initializers) | **yes** |
| 2 | inferred usage from the glTF material slot | **yes** |
| 3 | project presets (named intents) | reserved (v0.6) |
| 4 | target encodings per preset | reserved (v0.6, v0.9 for mobile) |
| 5 | path rules (glob to preset/overrides) | **name rules only**: a file-stem suffix gives the usage of a standalone texture (`NameRule`, `kDefaultNameRules`, `ProviderDesc::nameRules`); globs and presets v0.6 |
| 6 | per-asset sidecar (`*.kiln`) | reserved (v0.6) |
| 7 | session overrides: structs passed by the host, `CookSession`, CLI flags | **yes** |
| - | `kiln-cook --explain` (which layer set each field) | reserved (v0.6) |

Inference (layer 2, `usage_from_slot`, `color_space_for`):

| glTF slot | usage | colorSpace |
|---|---|---|
| `baseColorTexture` | Color | Srgb |
| `emissiveTexture` | Color | Srgb |
| `normalTexture` | Normal | Linear |
| `metallicRoughnessTexture` | Orm | Linear |
| `occlusionTexture` | Orm | Linear |
| standalone image (PNG, JPEG, WebP), no slot | name rule (layer 5), else Color | from the usage |

An image bound to two slots with different inferred usages is cooked with the first slot's usage
and a K1017 warning.

`resolve_texture(overrides, SlotHint, TargetProfile, CookSession, diag, asset)` and
`resolve_mesh(overrides, TargetProfile, CookSession, diag, asset)` are pure functions. After
resolution no field is `Auto`. The cook functions receive resolved structs only.

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
- v0.5 stores files by name, `<storeDir>/<assetPath>.mesh|.ktx2`, and invalidates by the stored
  hashes (R4). The hashed layout (16 lowercase hex digits of `key` plus the extension) is
  `kiln-cook --hashed`; it returns as the default with the index file in v0.6.

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
- Layering in v0.6 needs to know which fields a layer set. `Auto` works for enums; bools and
  numbers will need a set mask or an overrides struct.

## Open points for the owner

- Confirm the field list and defaults above.
- Confirm `usage` includes `Ui`, `Lut`, `Height` in v0.5 even though they only affect color space,
  channel layout and mips for now.
- Confirm the inference table (`emissiveTexture` as sRGB color).
- How overrides express "unset" for bools and numbers in v0.6 (set mask vs parallel overrides
  struct).
- Confirm the Compression group fields are declared in v0.5 with only `None` / 0 accepted.
