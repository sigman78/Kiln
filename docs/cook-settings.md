# Cook settings

The detailed layered design (presets, target encodings, path rules, sidecars, config files and
`kiln-cook --explain`) lands in **v0.6**.

For v0.5, the settings are plain C++ structs with defaults, inference from glTF material slots,
and host-supplied overrides only. See [`design/settings.md`](design/settings.md) for the v0.5
structs, the resolution rules and the store-key hashing rule.

## Layer model

From HANDOFF §5.1, weakest to strongest:

```
1. built-in defaults
2. inferred usage       (glTF material slot -> "color", "normal", "orm", ...)
3. project presets      (named intents: color, color_masked, normal, mask, hdr, ui, lut, height ...)
4. target encodings     (per target, per preset: desktop color -> BC7, mobile color -> ASTC 6x6 ...)
5. path rules           (glob -> preset/overrides: "ui/**" -> ui)
6. per-asset sidecar    (hull_albedo.png.kiln / ship_hauler_a.glb.kiln)
7. session overrides    (dev fast profile, CLI flags, host-supplied structs)
        |
        v
resolved settings struct -> hashed into the store key
```

v0.5 implements layers 1, 2 and 7. Layers 3 to 6 are reserved.
