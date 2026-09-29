# BCn texture encoding

**Status:** Proposed (2026-09-29), awaiting the owner's choices in "Open points". Nothing is
implemented.
**Decides:** Which block-compressed formats the cooker writes for each texture usage, which encoder
libraries do it, the settings that control it, how block formats reach the adapters, and the order
of the work. Zstd supercompression and RDO are covered as a later step of the same work.

## Summary

The cooker learns to write **BC1, BC3, BC4, BC5, BC6H and BC7** into KTX2. Each texture usage has a
default format for the desktop target (color and ORM → BC7, normal → BC5, one-channel mask → BC4,
HDR → BC6H). Two small vendored libraries do the encoding:
- **bc7enc_rdo** (`rgbcx` for BC1/3/4/5, `bc7enc` for BC7, `ert` for RDO later);
- **Compressonator's CMP_Core** for BC6H only.

The runtime needs no new code for block formats: the reader, the upload layout and `texture_info`
already work in blocks. The example adapters and their shaders need small changes (compressed
uploads, the Z of a BC5 normal). Zstd supercompression and RDO come after, as a separate step,
because they bring the runtime's first third-party dependency.

## What exists

- `Format` has BC1–BC7 (values 131–146) with their `FormatInfo` rows: 4×4 blocks, 8 or 16 bytes.
- The KTX2 **reader** accepts block formats (the corpus has BC files). The KTX2 **writer** rejects
  them ("not supported by the v0.5 writer") and writes no Data Format Descriptor for them.
- The reader rejects every supercompression scheme.
- `texture_level_layout()` and the loader compute offsets, row pitches and row counts in blocks, so
  a BC texture already uploads through every adapter path that kiln drives.
- The cooker picks RGBA8 / R8 / RG8 / R16 / RGBA16F in `plan_for` (`texture_cook.cpp`).
  `TextureCookSettings` reserves `encoding` and `supercompression`; `TargetProfile` has no
  per-target format policy yet.
- HDR sky cubes cook to RGBA16F: a 2048² cube with mips is 256 MiB. `hdr-textures.md` names BC6H
  as the fix.

## Decision (proposed)

### 1. Formats per usage (desktop target)

| Usage | Today | With BCn | Size vs today |
|---|---|---|---|
| `Color`, `Ui` with alpha or without | RGBA8 (sRGB) | **BC7** (sRGB) | 1/4 |
| `Normal` | RGBA8 | **BC5** (X, Y; the shader rebuilds Z) | 1/4 |
| `Orm` | RGBA8 | **BC7** (linear) | 1/4 |
| `Mask`, 1 channel, 8-bit | R8 | **BC4** | 1/2 |
| `Mask`, 2 channels | RG8 | **BC5** | 1/2 |
| `Height`, 16-bit | R16 | R16 (unchanged: BC4 has 8-bit precision) | — |
| `Lut` | RGBA8 / R8 | unchanged (exact values) | — |
| `Hdr` | RGBA16F | **BC6H** unsigned (`UFLOAT`) | 1/8 |

- BC1 (4 bits per texel) and BC3 stay available through the `encoding` setting, for hosts that want
  size over quality. They are not a default: BC7 is the same size as BC3 and better for every
  usage above.
- BC6H `SFLOAT` is only for sources with negative values, which kiln's `.hdr` decoder never makes.
- A cube or array encodes each face or layer on its own; a mip level smaller than 4×4 is one block,
  its missing texels repeated from the edge.

### 2. Encoders

| | bc7enc_rdo | CMP_Core (Compressonator) | ISPC Texture Compressor | DirectXTex |
|---|---|---|---|---|
| Formats | BC1/3/4/5 (`rgbcx`), BC7 | BC1–BC7 incl. BC6H | BC1/3/6H/7, ETC1, ASTC | BC1–BC7 incl. BC6H |
| License | MIT or public domain (`bc7e.ispc`: Apache-2.0) | MIT | MIT | MIT |
| State (2026-09) | active (last push 2026-07) | dormant (last release 2024-01) | **archived** | active |
| Build | plain C++ files | C++ files, runtime SIMD dispatch (SSE/AVX/AVX-512) | needs the ISPC compiler | needs DirectXMath; Windows or GCC |
| RDO | yes (`ert`, for any BC format) | no | no | no |
| BC7 quality / speed | `bc7enc` (CPU): 4 modes, good and fast; `bc7e.ispc`: best, needs ISPC | good, slower | very good, fast | reference quality, slow |

Proposed: **bc7enc_rdo for BC1/3/4/5/7 and RDO, CMP_Core for BC6H only.**
- bc7enc_rdo covers everything but BC6H with plain C++ and the RDO kiln needs later. Its CPU BC7
  encoder is weaker than `bc7e.ispc` but good for desktop assets. `bc7e.ispc` can come later behind
  a CMake option: it needs the ISPC compiler, downloaded and hash-checked like Slang.
- BC6H has no maintained, small, plain-C++ encoder. CMP_Core's `bc6_encode_kernel.cpp` is one file
  (plus common headers) under MIT; kiln calls it with SIMD dispatch off (see determinism).
- Both go into `third_party/` (vendored sources, pinned commits) and link into `kiln_cook` only. The
  shipping build gains nothing.

### 3. Determinism

kiln's goldens are byte-exact on every compiler and OS in CI, so the encoders must be too.
- Compile the encoder sources with contraction off (`-ffp-contract=off`; MSVC's default
  `/fp:precise`) and without `-march` flags, so no FMA or wider SIMD changes a rounding.
- Call CMP_Core's scalar paths only: its runtime dispatch picks SSE, AVX or AVX-512 by the machine,
  which would make the output depend on the CPU.
- Encode blocks in parallel (`parallel_for` over block rows, `CookEnv`): each block is independent,
  so the thread count cannot change the bytes. Encoder-internal threading (OpenMP) stays off.
- The rollout's spike checks this on the corpus in CI before goldens are committed. If an encoder
  cannot be made byte-exact, its goldens compare decoded texels against a PSNR floor instead, and
  the golden README says which.

### 4. Settings

`TextureCookSettings` gains:
- `encoding`: `Auto` (the table above, gated by the target), `Uncompressed`, `BC1`, `BC3`, `BC4`,
  `BC5`, `BC6H`, `BC7`. An explicit encoding that does not fit the usage (BC6H for a mask, BC4 for
  RGB) is K3002; one the target rules out is clamped to the target's choice with K3003.
- `quality`: `Fast`, `Normal` (default), `High`. It maps to the encoders' levels (for example
  bc7enc's `uber` level, rgbcx's level). `CookSession::fastPreview` forces `Fast`.

`TargetProfile` gains `blockCompression` (`true` for the built-in `desktop` target). Off, `Auto`
means today's uncompressed formats, so a host whose adapter has no BC support still cooks.

Both enter the settings hash, and the encoders' versions enter `kCookerVersion`, so a change
re-cooks.

### 5. Adapters and hosts

The runtime is unchanged, but every example adapter and host needs work:
- `supports_format` must say yes to the BC formats the device samples (Vulkan: format properties;
  GL: `GL_EXT_texture_compression_s3tc` / BPTC / RGTC; sokol: `sg_query_pixelformat`;
  NoGraphicsAPI: `supports_texture_format`).
- Uploads: GL needs `glCompressedTextureSubImage2D/3D` instead of `glTextureSubImage*`; Vulkan's
  and NoGraphicsAPI's copies already take block formats (row length in texels, a multiple of 4);
  sokol takes compressed data as image content.
- Shaders rebuild a BC5 normal's Z: `z = sqrt(saturate(1 - x² - y²))`. The host knows the format
  from `texture_info()`. A host that cannot change its shaders sets `encoding = BC7` for normals.
- The normal placeholder stays RGBA8; a shader that rebuilds Z from X and Y gets the same flat
  normal from it.

A host that loads a BC texture on an adapter without BC support gets K5004 at metadata time, as
for any unsupported format.

### 6. Later in this work: Zstd supercompression and RDO

- **Zstd** compresses each mip level inside the KTX2 file (`supercompressionScheme = 2`). It
  shrinks the store on disk and the bytes read at load, not GPU memory.
  - The reader must accept it.
  - The loader decompresses each level straight into the staging memory, or through scratch memory
    when the adapter pads rows.
  - The runtime gets zstd's decoder, the dependency `dependencies.md` already plans ("Zstd").
- **RDO** (bc7enc_rdo's `ert`) changes the encoded blocks so Zstd compresses them better, at a
  small quality cost (a `rdoLambda` setting, 0 = off). It only helps with Zstd on, so it lands with
  it.
- Settings: `supercompression` (`None`, `Zstd`), `zstdLevel`, `rdoLambda`.

## Alternatives considered

- **CMP_Core for everything:** one dependency instead of two, full BC7 modes. But it is dormant,
  has no RDO, and its SIMD dispatch has to be kept off everywhere. Worth measuring in the spike.
- **`bc7e.ispc` from the start:** the best BC7 per second, but a build-time compiler download for
  every cook build. Better as an option once the pipeline stands.
- **DirectXTex:** reference quality and maintained, but slow for BC6H/BC7, tied to DirectXMath, and
  officially Windows or GCC; too heavy for a cook library that also builds with clang on Linux.
- **ISPC Texture Compressor:** fast and good, but archived in 2024 and ISPC-only.
- **Basis Universal (UASTC / ETC1S, transcoded at load):** one file for every GPU family. It belongs
  with the mobile targets (v0.9, `TargetProfile` per platform), not with desktop BCn.
- **Writing our own encoders:** BC4 and BC5 are small, but BC6H and BC7 are large and subtle; no.

## Consequences

- Desktop textures shrink 4× (color, normal, ORM), 2× (masks) and 8× (HDR). Cook time grows:
  BC7 at `Normal` quality costs seconds for a 4K texture; `Fast` stays interactive for
  cook-on-miss.
- Every example gains compressed uploads and Z reconstruction; `kiln-headless` and the null adapter
  already accept every format.
- Goldens for cooked textures change once (a new `kCookerVersion`).
- Two new cook-side dependencies, entered in `dependencies.md` and `third_party/README.md`.

## Rollout

1. **KTX2 writer for block formats.** DFD for BC1–BC7 (Khronos Data Format models `BC1A`…`BC7`),
   round trip through the reader, `ktx validate` on the output. No encoder yet: tests write
   synthetic blocks.
2. **Spike (no commit of goldens).** Vendor bc7enc_rdo and CMP_Core on a branch; encode the texture
   corpus with each; record PSNR, speed and byte-exactness across MSVC, clang-cl, clang and gcc in
   this note. Confirms or changes section 2.
3. **BC1/3/4/5/7 in the cooker.** `encoding`, `quality`, `TargetProfile::blockCompression`, the
   usage table, parallel encoding, goldens, `kiln-info` output.
4. **BC6H** for `Hdr`.
5. **Adapters and examples:** `supports_format`, compressed uploads (GL), BC5 normal Z in every
   example shader, the reference frame re-checked.
6. **Zstd supercompression and RDO:** reader, loader, runtime decoder, settings.

## Open points for the owner

1. **Encoders:** bc7enc_rdo + CMP_Core (BC6H), as proposed? Or CMP_Core alone, or `bc7e.ispc` now?
2. **Default:** `blockCompression` on for the built-in `desktop` target? (Proposed: yes.)
3. **Normals:** BC5 with Z rebuilt in the shader (proposed), or BC7 so shaders need no change?
4. **UI textures:** BC7 like color (proposed), or uncompressed to keep text crisp?
5. **Goldens:** byte-exact as today (proposed), with the PSNR fallback only for an encoder that
   cannot be made deterministic?
6. **Zstd and RDO:** step 6 of this work (proposed), or a separate note later?
