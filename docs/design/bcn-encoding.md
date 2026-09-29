# Block-compressed textures: BCn now, ASTC and ETC2 later

**Status:** Decided (owner, 2026-09-29): the three measurement axes, `BC` as the desktop default,
BC7 for UI, byte-exact goldens first, Zstd and RDO last, ASTC with the mobile targets. The encoders
and the normal-map format follow from the spike's numbers (rollout step 2). Nothing is implemented.
**Decides:** Which block-compressed formats the cooker writes for each texture usage and target,
how the encoder libraries are chosen, the settings that control them, how block formats reach the
adapters, and the order of the work. Zstd supercompression and RDO are a later step of the same
work; ASTC and ETC2 follow with the mobile targets.

## Summary

The cooker learns to write **BC1, BC3, BC4, BC5, BC6H and BC7** into KTX2. Each texture usage has a
default format for the desktop target (color and ORM → BC7, normal → BC5, one-channel mask → BC4,
HDR → BC6H). The encoders are picked on the Pareto front of **dependency size** against **encode
speed and quality**, measured on kiln's own corpus (section 2); the likely outcome is bc7enc_rdo
for BC1–5 and BC7 plus CMP_Core for BC6H. A target names its **format family** (None, BC, later
ASTC and ETC2), so mobile targets slot in without a settings change (section 7).

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
| `Normal` | RGBA8 | **BC5** (X, Y; the shader rebuilds Z); BC7 if the spike says so | 1/4 |
| `Orm` | RGBA8 | **BC7** (linear) | 1/4 |
| `Mask`, 1 channel, 8-bit | R8 | **BC4** | 1/2 |
| `Mask`, 2 channels | RG8 | **BC5** | 1/2 |
| `Height`, 16-bit | R16 | R16 (unchanged: BC4 has 8-bit precision) | — |
| `Lut` | RGBA8 / R8 | unchanged (exact values) | — |
| `Hdr` | RGBA16F | **BC6H** unsigned (`UFLOAT`) | 1/8 |

- **Normals, BC5 or BC7:** both cost 1 byte per texel. BC5 codes X and Y as two independent BC4
  channels, so each gets its own endpoints; BC7 shares its bits over three correlated channels, one
  of which (Z) is redundant. BC5 is the usual choice for that reason, at the cost of one line of
  shader code. The spike measures the angular error of both on kiln's normal maps and the default
  follows it; either way the other stays available through `encoding`.
- BC1 (4 bits per texel) and BC3 stay available through the `encoding` setting, for hosts that want
  size over quality. They are not a default: BC7 is the same size as BC3 and better for every
  usage above.
- BC6H `SFLOAT` is only for sources with negative values, which kiln's `.hdr` decoder never makes.
- A cube or array encodes each face or layer on its own; a mip level smaller than 4×4 is one block,
  its missing texels repeated from the edge.

### 2. Encoders: the Pareto front

Three axes decide, and each is measured, not assumed:
- **Dependency cost:** vendored source, compiled size inside `kiln_cook`, build requirements (a
  compiler such as ISPC, SIMD dispatch, OpenMP), license and maintenance.
- **Encode speed:** megapixels per second per format and quality level, single thread and with
  kiln's `parallel_for`.
- **Quality:** PSNR against the source (RGB for color, per channel for masks, angular error for
  normals, log-space error for BC6H) at each quality level.

Candidates, with the source each would vendor (measured 2026-09-29 from the repositories):

| Candidate | Formats | Vendored source | Build | License, state |
|---|---|---|---|---|
| bc7enc_rdo: `rgbcx` + `bc7enc` (+ `ert`) | BC1/3/4/5, BC7 (4 modes); RDO for all | ~250 KB code + a 110 KB or 450 KB table | plain C++ | MIT or public domain; active |
| bc7enc_rdo: `bc7e.ispc` | BC7 (all modes) | 160 KB | ISPC compiler | Apache-2.0; active |
| CMP_Core, BC6H only | BC6H | ~350 KB (kernel 165 KB + shared headers) | C++; keep SIMD dispatch off | MIT; dormant since 2024-01 |
| CMP_Core, all | BC1–BC7 | ~1 MB (incl. 270 KB BC7 table header) | as above | as above |
| DirectXTex BC codecs | BC1–BC7 | 230 KB + DirectXMath headers | Windows or GCC; OpenMP for speed | MIT; active |
| ISPC Texture Compressor | BC1/3/6H/7 | 260 KB | ISPC compiler | MIT; **archived** 2024 |
| astcenc (for section 7) | ASTC | 1.26 MB | C++, SIMD variants chosen at build | Apache-2.0; active |

Source bytes are only a proxy; the spike records the compiled size, which is what a cook build and
a tool download carry.

The expected result, to be confirmed by the spike:
- **BC1/3/4/5:** `rgbcx`. Small, fast, near the best quality for these formats; CMP_Core and
  DirectXTex would be the alternatives if its table size or quality disappoints.
- **BC7:** `bc7enc` (CPU) dominates on dependency cost; `bc7e.ispc` and CMP_Core may dominate on
  quality per second. If the gap is small, `bc7enc` wins; `bc7e.ispc` can come later as an opt-in
  CMake option with a hash-checked ISPC download (like Slang).
- **BC6H:** CMP_Core's kernel against DirectXTex's `BC6HBC7.cpp`, the only maintained plain-C++
  choices. The smaller one that is deterministic and fast enough wins.
- Whatever is chosen goes into `third_party/` (vendored, pinned) and links into `kiln_cook` only.
  The shipping build gains nothing.

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

`TargetProfile` gains `blockFamily`: `None`, `BC` (the built-in `desktop` target), and later `ASTC`
and `ETC2` (section 7). With `None`, `Auto` means today's uncompressed formats, so a host whose
adapter has no BC support still cooks. `encoding` names families too once ASTC exists (for example
`ASTC_6x6`); an explicit encoding outside the target's family is clamped with K3003.

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

### 7. ASTC and ETC2 (later, with the mobile targets)

Desktop GPUs sample BC; phones, Apple GPUs on iOS and handheld consoles sample ASTC (and older
Android ETC2). So ASTC belongs to per-platform targets (HANDOFF v0.9, "target profiles and
cross-cooking"), and this work leaves room for it:
- **Encoder: astcenc** (Arm's reference). Apache-2.0, maintained, and its normal builds are
  documented to give bit-identical output across compilers and CPU architectures, which fits
  kiln's byte-exact goldens. It is the largest candidate (1.26 MB of source), a cost the spike
  measures in compiled size too.
- **Block size is the quality knob:** 4×4 (8 bits per texel) for normals and UI, 6×6 (3.6 bpt) for
  color and ORM, 8×8 (2 bpt) for large or distant textures. `Format` already has all 28 LDR
  formats (14 block sizes, UNORM and sRGB).
- **HDR** on those devices needs ASTC's HDR profile (not in `Format` yet) or RGBA16F.
- **ETC2 / EAC** (`Format` has them) only matter for GLES3-era Android without ASTC; an encoder
  (etcpak is small and fast) is added only if such a target is needed.
- **Basis Universal** (UASTC / ETC1S, transcoded at load) is the alternative for "one file for
  every GPU". It changes the runtime (a transcoder instead of direct upload), so it is a separate
  decision with the targets.

## Alternatives considered

- **CMP_Core for everything:** one dependency instead of two, full BC7 modes. But it is dormant,
  has no RDO, and its SIMD dispatch has to be kept off everywhere. Worth measuring in the spike.
- **`bc7e.ispc` from the start:** the best BC7 per second, but a build-time compiler download for
  every cook build. Better as an option once the pipeline stands.
- **DirectXTex:** reference quality and maintained, but slow for BC6H/BC7, tied to DirectXMath, and
  officially Windows or GCC; too heavy for a cook library that also builds with clang on Linux.
- **ISPC Texture Compressor:** fast and good, but archived in 2024 and ISPC-only.
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
2. **Spike: the Pareto measurement.** Build every BCn candidate of section 2 in a throwaway
   harness (not in `kiln_cook`); encode the texture corpus at each quality level; record compiled
   size, speed, quality and byte-exactness across MSVC, clang-cl, clang and gcc in this note. The
   owner picks from that table.
3. **BC1/3/4/5/7 in the cooker.** `encoding`, `quality`, `TargetProfile::blockFamily`, the
   usage table, parallel encoding, goldens, `kiln-info` output.
4. **BC6H** for `Hdr`.
5. **Adapters and examples:** `supports_format`, compressed uploads (GL), BC5 normal Z in every
   example shader, the reference frame re-checked.
6. **Zstd supercompression and RDO:** reader, loader, runtime decoder, settings.
7. **Later, with the mobile targets (v0.9):** astcenc, ASTC block sizes per usage, the `ASTC`
   family in `TargetProfile`; ETC2 only if a target needs it.

## Owner decisions (2026-09-29)

1. **Axes:** dependency cost against speed and quality, as proposed; the encoders are picked from
   the spike's table.
2. **Default:** `blockFamily = BC` for the built-in `desktop` target.
3. **Normals:** measured in the spike (BC5 against BC7); BC5 unless the numbers say otherwise.
4. **UI textures:** BC7, like color.
5. **Goldens:** the simple thing first (byte-exact); the PSNR fallback only if an encoder needs it.
6. **Zstd and RDO:** last, as step 6.
7. **ASTC:** with the mobile targets (v0.9).
