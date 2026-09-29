# Block-compressed textures: BCn now, ASTC and ETC2 later

**Status:** Decided (owner, 2026-09-29): the three measurement axes, `BC` as the desktop default,
BC7 for UI, byte-exact goldens first, Zstd and RDO last, ASTC with the mobile targets, and the
plain C++ encoders `rgbcx` and `bc7enc` (from the spike's measurements), and for BC6H a C++ port
of the ISPC Texture Compressor's encoder (no ISPC toolchain, no prebuilt objects). Rollout steps
1–5 are implemented: the cooker writes BC1/3/4/5/6H/7, every example adapter uploads them, and
`desktop` cooks BC by default. Step 6 is half done: Zstd supercompression is on by default (owner,
2026-09-29); RDO is deferred.
**Decides:** Which block-compressed formats the cooker writes for each texture usage and target,
how the encoder libraries are chosen, the settings that control them, how block formats reach the
adapters, and the order of the work. Zstd supercompression and RDO are a later step of the same
work; ASTC and ETC2 follow with the mobile targets.

## Summary

The cooker learns to write **BC1, BC3, BC4, BC5, BC6H and BC7** into KTX2. Each texture usage has a
default format for the desktop target (color and ORM → BC7, normal → BC5, one-channel mask → BC4,
HDR → BC6H). The encoders are picked on the Pareto front of **dependency size** against **encode
speed and quality**, measured on the demo models' textures (section 2): bc7enc_rdo's `rgbcx` for
BC1–5, `bc7enc` for BC7, and a C++ port of the ISPC Texture Compressor's BC6H. A target names its
**format family** (None, BC, later ASTC and ETC2), so mobile targets slot in without a settings
change (section 7).

The runtime needs no new code for block formats: the reader, the upload layout and `texture_info`
already work in blocks. The example adapters and their shaders need small changes (compressed
uploads, the Z of a BC5 normal). Zstd supercompression and RDO come after, as a separate step,
because they bring the runtime's first third-party dependency.

## What exists

- `Format` has BC1–BC7 (values 131–146) with their `FormatInfo` rows: 4×4 blocks, 8 or 16 bytes.
- The KTX2 **reader** accepts block formats. The KTX2 **writer** writes BC1–BC7 with the same Data
  Format Descriptors as `ktx create` (step 1).
- The cooker encodes BC1/3/4/5/6H/7 (steps 3 and 4); the sections below say how.
- The reader rejected every supercompression scheme (before step 6; it now reads Zstd).
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

- **Normals, BC5 or BC7:** both cost 1 byte per texel. BC5 codes X and Y as two independent BC4
  channels, so each gets its own endpoints; BC7 shares its bits over three correlated channels, one
  of which (Z) is redundant. The spike confirmed it: BC5 had a lower angular error than BC7 at
  every encoder speed. BC7 stays available through `encoding` for hosts that cannot rebuild Z.
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

Chosen (owner, 2026-09-29), from the spike's measurements:
- **BC1/3/4/5: `rgbcx`**, with its smaller table (120 KB compiled). It gave the best quality at
  every speed of the candidates.
- **BC7: `bc7enc`** (47 KB, plain C++). `bc7e.ispc` gives about 1.5 dB more on color and 2.7 dB on
  ORM, at half the speed and with the ISPC compiler as a build tool; it stays an option for later,
  and switching changes every BC7 golden once.
- **BC6H: the ISPC Texture Compressor's encoder, ported to scalar C++** (owner, 2026-09-29: no
  ISPC toolchain and no committed per-platform objects). The port is byte-identical to the ISPC
  original (sse4 target) on every profile and runs at about a third of its speed: 17.5 MP/s at
  `veryfast`, 2.0 at `fast`, 0.77 at `basic`, against CMP_Core's 0.14 MP/s, with a lower error
  than CMP_Core at every profile.
- They are vendored in `third_party/bc7enc_rdo/` and link into `kiln_cook` only. The shipping build
  gains nothing.

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
- `quality`: `Fast`, `Normal` (default), `High`. `CookSession::fastPreview` forces `Fast`. The
  encoder settings per level:

  | Format | Fast | Normal | High |
  |---|---|---|---|
  | BC1 (`rgbcx` level) | 0 | 10 | 18 |
  | BC3 | level 0 | level 10 | level 18, `encode_bc3_hq` |
  | BC4, BC5 | `encode_bc4` / `bc5` | `_hq` | `_hq` |
  | BC7 (`bc7enc` uber level, linear weights, 64 partitions) | 0 | 2 | 4 |
  | BC6H (the ISPC Texture Compressor's profiles) | `veryfast` | `fast` | `basic` |

`TargetProfile` gains `blockFamily`: `None`, `BC` (the default, and so the built-in `desktop`
target), and later `ASTC` and `ETC2` (section 7). With `None`, `Auto` means uncompressed formats, so
a host whose adapter has no BC support still cooks (`kiln-cook --block none`). `encoding` names families too once ASTC exists (for example
`ASTC_6x6`); an explicit encoding outside the target's family is clamped with K3003.

`encoding = Auto` stays `Auto` after resolution: a mask's BC4 or BC5 depends on the source's
channel count, which only the cook knows. `encoding`, `quality` and `blockFamily` enter the hashes
only when they differ from their defaults, so uncompressed cooks keep their store keys. An encoder
update that changes its output bumps `kCookerVersion`, so a change re-cooks.

### 5. Adapters and hosts

The runtime is unchanged. The example adapters (step 5, done 2026-09-29):

| Adapter | BC formats | How |
|---|---|---|
| Vulkan (`kiln-viewer`, `kiln-vk-basic`) | all the device samples | unchanged: format properties, and copies already count in blocks |
| GL (`kiln-gl`, `kiln-gl-bindless`) | BC4, BC5, BC6H, BC7 (core RGTC / BPTC); BC1, BC3 with `GL_EXT_texture_compression_s3tc` and `GL_EXT_texture_sRGB` | `glCompressedTextureSubImage2D/3D` |
| sokol (`kiln-sokol`) | BC3, BC4, BC5, BC6H, BC7 | image content; no BC1: sokol has only BC1 RGBA, where BC1 RGB's black texels would be transparent |
| NoGraphicsAPI (`kiln-nga`) | BC3, BC5, BC6H, BC7 | NoGraphicsAPI has no BC1 or BC4 |

- Every example shader rebuilds a normal's Z from X and Y (`z = sqrt(max(1 - x² - y², 0))`), so a
  BC5 normal map and an RGBA8 one both work; a host does not need to know which it got. A host
  that cannot change its shaders sets `encoding = BC7` for normals.
- The normal placeholder stays RGBA8; rebuilding Z from its X and Y gives the same flat normal.
- The reference scene (WaterBottle under the HDR test sky) rendered with BC textures matches the
  uncompressed frame at 51–53 dB PSNR on GL, GL bindless, sokol (D3D11) and Vulkan; its GPU
  texture memory falls from 89.9 MB to 22.6 MB.

A host that loads a BC texture on an adapter without BC support gets K5004 at metadata time, as
for any unsupported format.

### 6. Zstd supercompression (done 2026-09-29), then RDO (deferred)

- **Zstd** compresses each mip level inside the KTX2 file (`supercompressionScheme = 2`). It
  shrinks the store on disk and the bytes read at load, not GPU memory.
  - The reader accepts it (and still rejects BasisLZ and Zlib, K4107). A Zstd level has no
    alignment; its `uncompressedByteLength` must match the dimensions, and it must be exactly one
    frame of that size (`Ktx2View::decode_level`, K4110).
  - The loader reads each frame into scratch memory and decodes it straight into the staging
    memory, or into a second scratch buffer when the adapter pads rows. One decoder per upload
    job, its memory from the context `Allocator`.
  - zstd 1.5.7 is vendored (`third_party/zstd`): a decoder-only build in `kiln_runtime`, the
    encoder in `kiln_cook` (`dependencies.md`, "Zstd").
  - **On by default** (owner, 2026-09-29): `TextureCookSettings::supercompression = Zstd`,
    `zstdLevel` 0 = level 3; `fastPreview` uses level 1; `kiln-cook --zstd <level>`, 0 = off.
    Settings with `None` keep the hash, and so the store keys, of the files cooked before Zstd.
  - The writer fixes zstd's parameters (level, content size on, checksum off, one thread), so the
    goldens pin the frames on every compiler.
  - A KTX2 source with Zstd levels passes through as it is.
  - Measured on the example assets (39 textures, 8 threads): the uncompressed store shrinks from
    437 MB to 68 MB for 0.9 s more cooking, the BC store from 109 MB to 38 MB for 0.7 s more.
    Decoding runs at about 1 GB/s per core.
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
  has no RDO, and its SIMD dispatch has to be kept off everywhere. The spike measured it slower
  and worse than the chosen encoders, BC6H included.
- **`bc7e.ispc` from the start:** the best BC7 per second, but a build-time compiler download for
  every cook build. Better as an option once the pipeline stands.
- **DirectXTex:** reference quality and maintained, but slow for BC6H/BC7, tied to DirectXMath, and
  officially Windows or GCC; too heavy for a cook library that also builds with clang on Linux.
- **ISPC Texture Compressor as ISPC:** fast and good, but archived in 2024, and it needs the ISPC
  compiler or prebuilt objects per platform. Its BC6H encoder is used as a C++ port instead.
- **Writing our own encoders from scratch:** BC4 and BC5 are small, but BC6H and BC7 are large and
  subtle; no. Porting a proven encoder, checked byte for byte against the original, is different.

## Consequences

- Desktop textures shrink 4× (color, normal, ORM), 2× (masks) and 8× (HDR). Cook time grows:
  BC7 with `bc7enc` at `Normal` runs at about 3.5 MP/s per thread, so a 4K texture with mips
  (22 MP) takes about 6 s on one thread and under 1 s on 8.
- Every example gained compressed uploads and Z reconstruction; `kiln-headless` and the null
  adapter already accepted every format.
- The switch of the default target to BC changed its target hash, so every store key and the
  `cookHash` of every cooked file changed once (the mesh goldens' header hashes too). A store in
  the named layout is not re-cooked by that (open-questions R9): delete it once.
- Two cook-side dependencies (bc7enc_rdo, and the BC6H port kiln maintains) and one test-only
  decoder (bcdec), entered in `dependencies.md` and `third_party/README.md`.

## Rollout

1. **KTX2 writer for block formats** (done 2026-09-29). DFD for BC1–BC7 (Khronos Data Format models `BC1A`…`BC7`),
   round trip through the reader, `ktx validate` on the output. No encoder yet: tests write
   synthetic blocks.
2. **Spike: the Pareto measurement** (done 2026-09-29). Every BCn candidate of section 2 in a
   throwaway harness outside the repository; the owner picked from its table (section 2).
3. **BC1/3/4/5/7 in the cooker** (done 2026-09-29). `encoding`, `quality`,
   `TargetProfile::blockFamily`, the usage table, parallel encoding, goldens, `kiln-cook --block` and
   `--quality`.
4. **BC6H** for `Hdr` (done 2026-09-29): the C++ port of the ISPC Texture Compressor's encoder.
5. **Adapters and examples:** `supports_format`, compressed uploads (GL), BC5 normal Z in every
   example shader, the reference frame re-checked; then `desktop` switches to `blockFamily = BC`
   (done 2026-09-29).
6. **Zstd supercompression** (done 2026-09-29): reader, loader, runtime decoder, settings, on by
   default. **RDO** is deferred: it costs 4–8 dB for about 2x smaller BC files, which suits a
   shipping build, not fast iteration.
7. **Later, with the mobile targets (v0.9):** astcenc, ASTC block sizes per usage, the `ASTC`
   family in `TargetProfile`; ETC2 only if a target needs it.

## Owner decisions (2026-09-29)

1. **Axes:** dependency cost against speed and quality, as proposed; the encoders are picked from
   the spike's table.
2. **Default:** `blockFamily = BC` for the built-in `desktop` target.
3. **Normals:** measured in the spike (BC5 against BC7); BC5 unless the numbers say otherwise.
   The numbers favored BC5.
4. **UI textures:** BC7, like color.
5. **Goldens:** the simple thing first (byte-exact); the PSNR fallback only if an encoder needs it.
6. **Zstd and RDO:** last, as step 6. Zstd, on by default: decided 2026-09-29, with RDO deferred.
7. **ASTC:** with the mobile targets (v0.9).
8. **Encoders:** plain C++, `rgbcx` and `bc7enc` (option A of the spike). BC6H: no ISPC toolchain
   and no committed ISPC objects; a C++ port of the ISPC Texture Compressor's encoder.
