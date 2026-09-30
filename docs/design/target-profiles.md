# Target profiles

**Status:** Decided (owner, 2026-09-29) and implemented (all four rollout steps). The owner asked for named profiles with
a most compatible default, and for a strict rule: a client never re-cooks or overwrites a store
cooked for another profile. Open points: a store without a descriptor is a mismatch; the runtime
check fails `create()`, with an escape hatch; the full-BC profile stays `desktop`.
**Decides:** What a target profile is, the built-in profiles and the default, how a store records
its profile, what happens on a mismatch, and what replaces the per-adapter format sets and the
cook-time fallback chain.
**Related:** [bcn-encoding.md](bcn-encoding.md), [settings.md](settings.md),
[store-manifest.md](store-manifest.md), [adapter.md](adapter.md).

## Summary

- A **profile** is a named set of the block formats a target samples, plus the usage table that
  picks one of them for each texture usage. It is plain data: a `TargetProfile` value.
- Built-in profiles: **`compat`** (the default: BC3, BC5, BC6H, BC7, which every example backend
  samples), **`desktop`** (all of BC, with BC4 masks and explicit BC1) and **`uncompressed`**.
- A store belongs to **one** profile. It records it in a small descriptor file. A cook provider or
  a `kiln-cook` run with another profile does not write to that store: it reports the mismatch and
  stops. Nothing is deleted, overwritten or re-cooked because of a profile.
- The runtime checks once, when it opens a store, that the adapter samples every format of the
  store's profile. If not, `create()` fails, instead of each asset failing later (K5004).
- The cooker never substitutes a format. A profile's table decides the format for each usage; an
  explicit `encoding` outside the profile is an error. This replaces the fallback chain.
- The examples use the default profile and share one `example-store` again.

## What existed before

Unreleased work after v0.5.0, which this note replaced:

- `TargetProfile::excludedBlockFormats`: the formats a target cannot sample.
  `unsampled_block_formats(adapter)` reads such a set from an adapter.
- A fallback chain in the texture cooker (BC1/BC3 to BC7, BC4 to BC5 to BC7, BC5 to BC7, BC7 to
  BC3, BC6H to RGBA16F), with a K3003 warning or info.
- Every integration example cooks with its adapter's set, into `example-store-<set in hex>`,
  because a store file is used as long as it exists (open-questions R9). Each desktop adapter
  excludes ASTC and ETC2, so every example got its own store.

Released in v0.5.0: `TargetProfile { name, blockFamily, maxTextureSize, maxVertexProfile,
maxArrayLayers }`, the default target `desktop` with `blockFamily = BC`, and `kiln-cook --target
desktop --block none|bc`.

## What the backends sample

| Backend | BC1 | BC3 | BC4 | BC5 | BC6H | BC7 |
|---|---|---|---|---|---|---|
| Vulkan, D3D11/12 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| GL 4.6 with S3TC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| sokol | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| NoGraphicsAPI | – | ✓ | – | ✓ | ✓ | ✓ |
| **all of them** | – | ✓ | – | ✓ | ✓ | ✓ |

The default usage table never picks BC1 (only a sidecar asks for it). So the common set costs one
thing: a one-channel mask is BC5 (8 bits per texel) instead of BC4 (4). Color, ORM, normal and HDR
textures are the same as with `desktop`.

## Decision

### 1. A profile is data

```cpp
struct TargetProfile {
    StrView name                   = "compat";
    u64 blockFormats               = kCompatFormats; ///< block_format_bit() set the target samples
    u32 maxTextureSize             = 16384;
    VertexProfile maxVertexProfile = VertexProfile::Float;
    u32 maxArrayLayers             = 2048;
};
```

- `blockFormats` is a positive list: the formats the target samples. It replaces both
  `blockFamily` and `excludedBlockFormats`. An empty set means uncompressed textures.
- The usage table is fixed code, not data. For each usage it lists the formats in order of
  preference; the cooker takes the first one in `blockFormats`, else uncompressed:

  | Usage | Preference |
  |---|---|
  | Color, UI | BC7 |
  | ORM | BC7 |
  | Normal | BC5, BC7 |
  | Mask, 1 channel | BC4, BC5 |
  | Mask, 2 channels | BC5 |
  | HDR | BC6H |
  | Height, LUT | uncompressed |

  This is what the fallback chain did, but as the profile's normal choice: no diagnostic, because
  nothing was changed.
- An explicit `encoding` (sidecar, host settings) that is not in `blockFormats` is a K3002 error.
  The cooker never silently writes another format than the one asked for.
- A host may define its own profile: any `TargetProfile` value with its own name.

### 2. Built-in profiles

| Name | `blockFormats` | For |
|---|---|---|
| `compat` (default) | BC3, BC5, BC6H, BC7 (UNORM and sRGB where they exist) | every example backend; the safe default |
| `desktop` | all BC formats the cooker writes | Vulkan and D3D hosts that want BC4 masks, or BC1 by request |
| `uncompressed` | none | adapters without block formats; debugging |

- `kiln::cook::target_profile(StrView name)` returns a built-in profile, or nothing for an unknown
  name. `kiln-cook --target compat|desktop|uncompressed` selects one. `--block` and
  `--exclude-format` go away.
- ASTC and ETC2 profiles (`mobile`, `web`) come with the v0.9 cross-cooking work; the shape
  already fits them.

### 3. A store belongs to one profile

*Superseded (2026-09-30):* the store manifest (`store-manifest.md`) records the profile in each
manifest, one per profile; `kiln-store.txt` and `bind_store_profile()` are gone. K3008 is now a
provider whose target is not the context's profile. The text below is the first version.

- The store directory holds a descriptor, `kiln-store.txt`, written by the first cook into an
  empty store:

  ```
  kiln-store 1
  profile compat
  hash 3f0c9a41d2b7e655
  formats 137 138 141 143 145 146
  ```

  - `hash` is `hash_target` of the profile, so two profiles with one name but different contents
    differ.
  - `formats` lists the VkFormat values the profile may write, so the runtime can check them
    without the cook library.
  - Text, one key per line: the runtime parses it with a few lines of code. The store manifest
    (`store-manifest.md`) can take it over later.
- **Mismatch:** a cook provider (`install_provider`) or a `kiln-cook` run whose profile hash
  differs from the descriptor's does not write to the store. `install_provider` returns
  `InvalidArgument` with a new diagnostic (K3008, "store cooked for profile X, this cook is Y");
  `kiln-cook` exits with an error. The host picks another store directory, or deletes this one.
- **Missing descriptor:** an empty store gets one on the first write. A store that has cooked files
  but no descriptor (made before this change) is treated as a mismatch, with a message that says to
  delete it once.
- Nothing is ever deleted, overwritten or re-cooked because of a profile.

### 4. The runtime checks the adapter once

- `create()` reads the store's descriptor, if any, and calls the adapter's `supports_format` for
  each listed format. A format the adapter cannot sample is one K5018 error naming the formats and
  the profile, and `create()` fails with `Unsupported`: the host chose a profile its adapter cannot
  take, which is a configuration error.
- Escape hatch: `ContextDesc::allowUnsampledFormats` (default `false`) makes K5018 a warning and
  `create()` succeeds; each asset of such a format then fails as today (K5004, Failed placeholder).
  For tools and debugging.
- The check uses the formats the profile allows, not the formats the store holds. A host whose
  adapter lacks a format it never uses (BC6H on a GL driver without BPTC, with no HDR textures)
  defines a profile without it, rather than disabling the check.
- `unsampled_block_formats(adapter)` stays: it is the tool for this check and for a host that
  builds its own profile from an adapter.
- A shipping build loads a pre-cooked store the same way, so a store cooked for the wrong device
  class shows up at startup.

### 5. The examples

- They use the default profile (`compat`) and share one `example-store` again.
  `use_target_store()` and the per-adapter sets go away.
- They keep the adapter check of section 4, so an adapter that cannot take `compat` says so at
  start.

## What it changes for users

- The default target changes from `desktop` (full BC) to `compat`, and its hash changes, so every
  default store key and `cookHash` changes once. One-channel masks become BC5 by default.
- A host that wants BC4 masks or BC1 selects `desktop`.
- A store cooked before this change has no descriptor: the provider refuses to write to it until it
  is deleted (a CHANGELOG migration note says so).
- Two programs with different profiles cannot share a store directory. That is the point: they
  would need different files.

## Alternatives considered

- **A store per adapter format set** (what existed, `example-store-<hex>`): correct, but every
  example gets its own store, and the names mean nothing to a person.
- **A fallback chain at cook time** (existed): the cooker writes another format than the one
  asked for, and the store's contents then depend on who cooked first. A profile's table gives the
  same results without the surprise.
- **Re-cooking a mismatched store in place:** it mixes files of two profiles, which is the R9
  problem again; and deleting files (the reverted store stamp) loses work.
- **Dropping BC1 and BC4 from the cooker:** they are the formats that halve memory (4 bits per
  texel against 8), and BC4 is the right format for one-channel data. They cost no code the
  project does not already have, so they stay, in `desktop` only.
- **Transcoding at load** (Basis): a different codec and a runtime transcoder; see
  `zstd-supercompression.md`, "Alternatives considered".

## Implementation

- `include/kiln/cook/settings.h`: `TargetProfile::blockFormats`, `kCompatBlockFormats`,
  `kDesktopBlockFormats`, `kCompatTarget` / `kDesktopTarget` / `kUncompressedTarget`,
  `target_profile()`, K3008. The usage table is `block_format()` in `src/cook/texture_cook.cpp`.
- The store descriptor (`bind_store_profile()`, `StoreProfile`, `read_store_profile()`) was replaced
  by the manifest's profile fields (2026-09-30). `ContextDesc::allowUnsampledFormats`, K5018 and
  `diag_sink()` stay; the check is `check_formats()` in `src/runtime/context.cpp`.
- `kiln-cook --target compat|desktop|uncompressed` (default `compat`).

## Rollout

1. `TargetProfile::blockFormats`, the built-in profiles, the usage table, K3002 for an explicit
   encoding outside the profile; remove `excludedBlockFormats`, the fallback chain, `--block` and
   `--exclude-format`. Goldens and store keys change once.
2. The store descriptor: written by the first cook, checked by `install_provider` and `kiln-cook`
   (K3008).
3. The runtime check at `create()` (K5018).
4. Examples back on one `example-store` with the default profile.

## Owner decisions (2026-09-29)

1. A store with cooked files and no descriptor is a mismatch; the message says to delete it once.
2. The runtime's adapter check fails `create()`. Only reporting would let the app run with holes
   (each asset in an unsampled format fails on its own), and the one early error is easy to miss.
   The downsides of failing are a false failure when the store never uses the missing format
   (answered by a narrower host profile) and debugging on an adapter that cannot take the profile
   (answered by `ContextDesc::allowUnsampledFormats`).
3. The full-BC profile stays `desktop` for now.
