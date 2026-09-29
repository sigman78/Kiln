# HDR textures

**Status:** Decided (owner, 2026-09-28): `.hdr` sources, RGBA16F, a warning for integer sources,
equirectangular to cube later. Rollout steps 1 to 3 are implemented.
**Decides:** Which HDR source format kiln reads, which GPU format it cooks HDR textures to, and why
OpenEXR sources and the packed HDR pixel formats are left out for now.

## Summary

kiln reads **Radiance `.hdr`** files with its own small decoder and cooks usage `Hdr` to
**`R16G16B16A16_SFLOAT`** (RGBA16F). No new dependency. A float KTX2 made by another tool
already passes through the cook unchanged.

## What exists

- `TextureUsage::Hdr` exists, but `cook_texture` treats it like `Color` (8-bit sRGB).
- `Format` has the float formats (`R16G16B16A16_SFLOAT`, `R32G32B32A32_SFLOAT`, …). The KTX2
  reader and writer handle them: the corpus round-trips `rgba16f.ktx2`, and `ktx validate`
  accepts the output.
- The image pipeline (`Image`, `prepare_image`, `build_mip_chain`) handles 8- and 16-bit integer
  channels only.

## Decision

### 1. Source format: Radiance `.hdr`

- It is the usual format for HDR environment maps: HDRI libraries publish it next to EXR.
- The format is small: a text header (`#?RADIANCE` or `#?RGBE`, `FORMAT=32-bit_rle_rgbe`, a
  resolution line), then 4 bytes per pixel (RGB mantissas and one shared exponent), each scanline
  flat or run-length encoded.
- kiln decodes it itself: `decode_hdr` in `src/cook/hdr_decode.cpp`, with bounds checks on every
  run. Only the standard orientation (`-Y H +X W`) is accepted; others are
  K2002 (Unsupported). A truncated or malformed file is K2001.
- Signature detection joins `decode_image`, and `.hdr` joins the source extensions of the provider
  and `kiln-cook` (and the kind table of `asset-model-next.md` I5).

### 2. Cooked format: RGBA16F

- Every desktop GPU samples and filters it (Vulkan mandatory support); it keeps negative values
  and alpha; it is exact enough for lighting (10-bit mantissa per channel).
- The `.hdr` source has no alpha: alpha is 1.
- Float to half conversion is kiln's own (`float_to_half`, `half_to_float` in
  `kiln/cook/image.h`), integer-only, round to nearest even, subnormals included. Overflow
  saturates to the largest half value, not infinity, so a very bright texel does not poison
  filtering; NaN becomes 0.

### 3. The float image path

- `Image` gains 32-bit float channels (`bitsPerChannel = 32`, float). Decoding produces RGB32F.
- `prepare_image` and `build_mip_chain` gain a float kernel: a box filter, already in linear space.
  The kernel stays plain arithmetic so that every compiler produces the same bytes (no contracted
  multiply-add); a golden file checks that.
- Conversion to half happens once, per level, after the mip chain.

### 4. Resolution

- A `.hdr` source implies usage `Hdr` and color space Linear when both are `Auto` (inference,
  layer 5 of `settings.md`). No name rule is needed.
- Usage `Hdr` cooks to RGBA16F. An 8-bit or 16-bit integer source with usage `Hdr` also cooks to
  RGBA16F (its values in 0..1, read as linear); a warning K2011 notes that it has no HDR range. A
  float source with any other usage is K2002: kiln does not tonemap into 8 bits.
- Shapes work unchanged: a `.hdr` vertical strip with `shape = cube` gives an HDR cube.

## Why not OpenEXR now

EXR is the other common HDR format, and the richer one. It stays out for now:

- **The format is large.** Scanline and tiled files, multi-part and deep images, arbitrary
  channel sets, half/float/uint channels, and many compressions: NONE, RLE, ZIPS, ZIP, PIZ (wavelet
  plus Huffman), PXR24, B44, B44A, DWAA, DWAB. Exporters use several of them.
- **The libraries cost more than they give now.** The reference OpenEXR library is C++ with
  Imath and exceptions, a large build, and every call would need a try/catch to meet kiln's
  no-exception rule. tinyexr is a single header but needs miniz and covers the codecs only in
  part; it has had several fuzzing-found memory bugs, which matters for a tool that reads files
  from anywhere.
- **`.hdr` covers the need now.** Environment maps are the main HDR input, and the libraries that
  publish them offer `.hdr` too.
- **A later path without a new dependency exists.** kiln's vendored wuffs already compiles its
  zlib and deflate decoders (for PNG). A kiln-owned EXR subset (single part, scanline, half or
  float RGB(A), compression NONE, ZIPS or ZIP) would need no new dependency. PIZ and the lossy
  codecs would still need a library. Add it when a real project needs EXR.

## Why not the packed HDR formats now

Vulkan has two 4-byte HDR formats, half the size of RGBA16F:

| Format | Layout | Loss |
|---|---|---|
| `B10G11R11_UFLOAT_PACK32` | three small floats: 6-bit mantissa for R and G, 5-bit for B | no alpha, no negative values; visible banding in smooth gradients such as skies |
| `E5B9G9R9_UFLOAT_PACK32` | 9-bit mantissas with one shared exponent (like RGBE) | no alpha, no negative values; a saturated bright color loses its dim channels |

They stay out of the first version:

- **Their loss depends on the content.** Whether banding or a lost channel is visible is a
  per-asset judgment, so they belong behind an explicit setting, not in the default.
- **Their encoders need care.** Rounding into 11- and 10-bit floats, and choosing the shared
  exponent, follow exact rules that need their own tests for determinism and edge cases (zero,
  overflow, denormals).
- **BC6H is the real answer to size.** It is 1 byte per texel, 8 times smaller than RGBA16F, and
  it is what shipping titles use for HDR. It needs an encoder, so it belongs to the v0.6 texture
  compression work, where BC1-BC7 are decided together. The packed formats would be an interim
  step with a narrower benefit.
- **Size matters mostly for skies.** A 2048 x 2048 cube in RGBA16F is 192 MiB, 256 MiB with mips.
  Until BC6H, a sky can use a smaller face size (`maxSize`).

When they come, it is as a setting, e.g. `hdrFormat = "rgba16f" | "b10g11r11" | "e5b9g9r9"`,
with RGBA16F as the default.

## Out of scope, next steps

- **Equirectangular to cube.** HDR skies come as 2:1 latitude-longitude panoramas, not 6-face
  strips. Converting them is a resampling step in the cook (e.g. `projection = "equirect"` with
  `shape = cube`), about the size of the decoder. It is the next step if HDR skies are the goal.
- **Tonemapping in the viewer.** The viewer writes an sRGB image, so values above 1 clip. A simple
  tonemap in the sky and mesh shaders fixes the look; it does not affect cooking.
- **Prefiltered environment maps** (irradiance, specular mips for image-based lighting). This is
  renderer policy: a host can produce them from the cooked cube.

## Rollout

1. *(done)* `decode_hdr` with tests on valid and malformed files (truncated runs, bad header,
   orientations, huge sizes).
2. *(done)* Float `Image` channels, the float mip kernel, float to half (checked exhaustively over
   all finite halves); a golden RGBA16F texture.
3. *(done)* Resolution and cook: `.hdr` implies `Hdr` and Linear, `Hdr` cooks to RGBA16F, K2011;
   `.hdr` in the provider and `kiln-cook`; `ktx validate` on the output.
4. Optional: viewer tonemapping, so HDR skies can be checked by eye.

