# Cook settings structs (v0.5 subset)

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** The v0.5 cook settings structs, which resolution layers exist in v0.5, and how resolved settings are hashed into the store key.

## Decision

The C++ structs are the real interface (HANDOFF §5.1). Config files, presets and rules come in
v0.6 as layers that produce the same structs. The structs live in `include/kiln/settings.h`.

### Texture

```cpp
enum class ColorSpace   : u8 { Auto, Srgb, Linear };
enum class TextureUsage : u8 { Auto, Color, Normal, Orm, Mask, Hdr, Ui, Lut, Height };

struct TextureCookSettings {
    ColorSpace   colorSpace        = ColorSpace::Auto;    // Auto: derived from usage
    TextureUsage usage             = TextureUsage::Auto;  // Auto: inferred from glTF slot, else Color
    bool         genMips           = true;
    bool         normalRenormalize = true;                // only for usage == Normal
    u32          maxSize           = 0;                   // 0 = no limit (target cap still applies)
    bool         flipGreen         = false;               // DirectX-style normal maps
    // reserved: alphaMode, premultiply, dilation, encoding, supercompression,
    //           residentMips, shape (2D / array / cube / 3D)
};
```

### Mesh

```cpp
enum class VertexProfile : u8 { Default, Precise };

struct MeshCookSettings {
    VertexProfile profile         = VertexProfile::Default;
    bool          genTangents     = true;
    bool          optimize        = true;    // vertex cache, overdraw, vertex fetch
    bool          useAuthoredLods = true;    // pass through `_lodN` nodes
    bool          genLods         = false;   // reserved: simplifier lands in v0.6; true is a K3xxx error in v0.5
    float         posTolMm        = 0.1f;    // quantization tolerance before falling back to float positions
    float         weldTol         = 0.0f;    // 0 = exact-match welding only
    // reserved: indexWidthPolicy, compression, unit/axis override, name prefixes to strip
};
```

### Target and session

```cpp
struct TargetProfile {
    StrView       name              = "desktop";
    u32           maxTextureSize    = 16384;
    VertexProfile maxVertexProfile  = VertexProfile::Precise;   // highest profile the target accepts
};

enum class StoreMode : u8 { Disk, Memory, None };   // store / cache-less / validate only (HANDOFF §4.3)

struct CookSession {
    StoreMode storeMode   = StoreMode::Disk;
    bool      fastPreview = false;   // cheaper settings for cache-less previews (e.g. no optimize)
};
```

- v0.5 has one implicit target, `desktop`, with raw formats.
- `fastPreview` is a session override: it changes resolved values (so it changes the hash), it is
  not a hidden side channel.

### Resolution layers

From HANDOFF §5.1, weakest to strongest:

| # | Layer | v0.5 |
|---|---|---|
| 1 | built-in defaults (the member initializers above) | **yes** |
| 2 | inferred usage from glTF material slot | **yes** |
| 3 | project presets (named intents) | reserved (v0.6) |
| 4 | target encodings per preset | reserved (v0.6, v0.9 for mobile) |
| 5 | path rules (glob to preset/overrides) | reserved (v0.6) |
| 6 | per-asset sidecar (`*.kiln`) | reserved (v0.6) |
| 7 | session overrides: C++ structs passed by the host, `CookSession`, CLI flags | **yes** |
| - | `kiln-cook --explain` (which layer set each field) | reserved (v0.6) |

Inference in v0.5 (layer 2):

| glTF slot | usage | colorSpace |
|---|---|---|
| `baseColorTexture` | Color | Srgb |
| `emissiveTexture` | Color | Srgb |
| `normalTexture` | Normal | Linear |
| `metallicRoughnessTexture` | Orm | Linear |
| `occlusionTexture` | Orm | Linear |
| standalone PNG, no slot | Color | Srgb |

A texture referenced from two slots with conflicting inferred usage (for example color and normal)
is a K3xxx error in v0.5, unless the host passes an explicit override.

Resolution is a pure function:

```cpp
Result<TextureCookSettings> resolve_texture(TextureCookSettings const& overrides, SlotHint hint,
                                            TargetProfile const&, CookSession const&, DiagSink const*);
```

After resolution no field is `Auto`. The cook functions receive resolved structs only.

### Validation

Unknown or invalid combinations are cook errors with K3xxx diagnostics, for example:

- `normalRenormalize` or `flipGreen` set with a usage other than `Normal`: warning (ignored).
- `genLods = true` in v0.5: error, unsupported.
- `maxSize` not a power of two (when non-zero): error.
- `posTolMm <= 0`: error.
- `profile = Precise` on a target whose `maxVertexProfile` is `Default`: error.

### Hashing rule

Resolved settings are hashed with a **versioned, field-by-field serializer**:

```cpp
inline constexpr u32 kTextureSettingsSchema = 1;   // bump when a field is added or its meaning changes

u64 hash_settings(TextureCookSettings const& s) {
    Xxh64State h;
    h.update_value(kTextureSettingsSchema);
    h.update_value(u8(s.colorSpace));
    h.update_value(u8(s.usage));
    h.update_value(u8(s.genMips));
    h.update_value(u8(s.normalRenormalize));
    h.update_value(s.maxSize);
    h.update_value(u8(s.flipGreen));
    return h.digest();
}
```

- **Never** `memcpy` or `update_value` the whole struct: padding bytes are indeterminate and would
  make the hash non-deterministic.
- Floats are hashed by bit pattern after normalizing `-0.0f` to `0.0f`.
- Strings (target name) are hashed as length followed by bytes.
- A unit test pins the hash of a default-constructed struct, so a silent layout change fails CI.

### Store key

```
key = hash_combine(hash_combine(hash_combine(
          xxh64(source bytes), settingsHash), targetHash), kCookerVersion)
file name = 16 lowercase hex digits of key + extension (".mesh" or ".ktx2")
```

- `hash_combine` and `xxh64` are the ones in `hash.h`.
- `kCookerVersion` is a `u64` constant bumped whenever cooker output can change for the same input.
- The `.mesh` header's `sourceHash` is `xxh64(source bytes)`. Its `cookHash` is the combination of
  settings hash, target hash and cooker version, that is the store key without the source part
  (see `docs/open-questions.md` B3).

## Rationale

- Designated initializers with defaults let callers set only what they mean (HANDOFF §2).
- `Auto` values make inference explicit and let resolution report which layer set a field later.
- Field-by-field hashing is deterministic across compilers and is the only safe way to hash
  structs with padding.

## Alternatives considered

- **Hash the raw struct bytes**: padding makes it non-deterministic; field reorder silently
  changes keys.
- **Key/value settings (string map)**: flexible but loses type checking and allocates; the
  struct is the interface and config files map into it later.
- **Separate `optional<T>` per field for overrides**: heavy; `Auto` sentinels cover the v0.5 needs.
  v0.6 may need a per-field "set" mask for layering (see open points).

## Consequences / what this constrains later

- Adding a field means: default value, serializer line, schema version bump. Old store entries then
  miss and re-cook, which is correct.
- Reserved groups (alpha, encoding, supercompression, shape) must be added as new fields, not by
  changing the meaning of existing ones.
- Layering in v0.6 needs to know which fields a layer set. `Auto` works for enums; bools and
  numbers will need a set mask or an overrides struct. Decide in v0.6.

## Open points for the owner

- Confirm the field list and defaults above.
- Confirm `usage` includes `Ui`, `Lut`, `Height` in v0.5 even though they only affect color space
  and mips for now.
- Confirm the inference table (`emissiveTexture` as sRGB color).
- How overrides express "unset" for bools and numbers in v0.6 (set mask vs parallel overrides
  struct).
