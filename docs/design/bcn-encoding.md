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
  RGB) is K3002, and so is one the target's profile does not have (`target-profiles.md`).
- `quality`: `Fast`, `Normal` (default), `High`. `CookSession::fastPreview` forces `Fast`. The
  encoder settings per level:

  | Format | Fast | Normal | High |
  |---|---|---|---|
  | BC1 (`rgbcx` level) | 0 | 10 | 18 |
  | BC3 | level 0 | level 10 | level 18, `encode_bc3_hq` |
  | BC4, BC5 | `encode_bc4` / `bc5` | `_hq` | `_hq` |
  | BC7 (`bc7enc` uber level, linear weights, 64 partitions) | 0 | 2 | 4 |
  | BC6H (the ISPC Texture Compressor's profiles) | `veryfast` | `fast` | `basic` |

The target decides which block formats exist: a **profile** (`target-profiles.md`) is the set of
block formats it samples, and the usage table picks the first format a usage prefers from that set.
The default profile, `compat`, has BC3, BC5, BC6H and BC7 (every example backend samples them), so
its one-channel masks are BC5; `desktop` adds BC4 and BC1; `uncompressed` has none. (Before
profiles, `TargetProfile::blockFamily` was `None` or `BC`, with `BC` the default.)

`encoding = Auto` stays `Auto` after resolution: a mask's BC4 or BC5 depends on the source's
channel count, which only the cook knows. `encoding` and `quality` enter the settings hash only
when they differ from their defaults; the profile's formats are in `hash_target`. An encoder update
that changes its output bumps `kCookerVersion`, so a change re-cooks.

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

Some APIs sample only part of the BC family (the table above). The default profile, `compat`, is
the set they all sample, so every example cooks the same files into one store; a host that wants
BC4 masks selects `desktop` (`target-profiles.md`). An explicit encoding outside the profile is an
error, never a substitution; and `create()` checks the store's profile against the adapter once.

### 6. Zstd supercompression (done 2026-09-29), then RDO (deferred)

Each mip level is one Zstd frame (KTX2 scheme 2), on by default, and only where it saves 10% of
the file. The measurements, the decision and the alternatives (LZ4, Oodle, Basis, a filter
scheme, RDO) are in `zstd-supercompression.md`. RDO (bc7enc_rdo's `ert`, an `rdoLambda` setting)
is deferred: it costs 4–8 dB for about 2x smaller BC files, which suits a shipping build, not fast
iteration.

### 7. ASTC and ETC2 (later, with the mobile targets)

Desktop GPUs sample BC; phones, Apple GPUs on iOS and handheld consoles sample ASTC (and older
Android ETC2). So ASTC belongs to per-platform targets (`ROADMAP.md` v0.9, mobile
target profiles), and this work leaves room for it:
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
8. **Next (v0.7): faster BC encoding** (owner, 2026-09-30). Encoding is about 90% of a cook on
   miss: WaterBottle's four 2048x2048 textures take 0.25-0.35 s each to encode in a release build
   and about 1 s each in a debug build. The benchmark below measured the candidates: Basis `bc7f` /
   `bc6hf` for fast presets, scalar `bc7e` for an offline quality mode; SIMD paths in `rgbcx` and
   `bc7enc` remain the alternative. Constraint: the output stays identical across threads and
   compilers (section 3).

## Benchmark follow-up: Basis direct BC encoders (2026-09-30)

These are measurements and candidates for the next encoder decision; the production encoders
and quality presets have not changed.

### Existing usage and candidates

- Kiln vendors `rgbcx` (BC1/3/4/5) and `bc7enc` (BC7) from `bc7enc_rdo` revision
  `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`, under MIT, and maintains the scalar BC6H C++
  port of ISPCTextureCompressor revision `79ddbc90334fc31edd438e68ccb0fe99b4e15aab`, also MIT.
- The individual encoders are scalar, but the cooker already parallelizes block rows through
  its job system, at roughly 1,024 blocks per task. A host without a job system runs inline.
  Adding SIMD and adding parallelism are therefore separate questions.
- Basis Universal v2_50, tested at `9bebe16726b3a61c8c213eeee3b7cffb462ef34e`, provides direct
  `bc7f` and `bc6hf` encoders in its transcoder, plus a standalone `basisu_bc7e_scalar` encoder.
  These accept pixels and produce GPU BC blocks; they do not require a `.basis` intermediate
  or a runtime transcoder. Initialize shared tables before submitting parallel work.
- Basis is [Apache-2.0](https://github.com/BinomialLLC/basis_universal/blob/9bebe16726b3a61c8c213eeee3b7cffb462ef34e/LICENSE):
  commercial use is permitted without royalties or releasing kiln's source; retain applicable
  license/notices and mark modified redistributed files. Its direct encoders here are scalar
  C++; much of `bc7f`'s speed comes from avoiding expensive searches, rather than explicit SIMD.
- Keep `rgbcx` for BC1–5 pending separate measurements. Its configurable search levels and HQ
  alpha/channel helpers offer more quality control than Basis's simpler BC1–5 helpers. This
  experiment benchmarks BC7 and BC6H only.

### Method

Windows, MSVC 19.51 Release `/O2 /fp:precise`, no LTO, i7-9700K (8 cores / 8 threads).
The BC7 corpus has 17 opaque color/emissive images, 5 alpha images (all from one model), and
9 ORM images; normals are excluded. BC6H uses three natural HDR environments and one synthetic
HDR cube. Inputs are center-cropped to at most 1024×1024, rounded to whole blocks, with no
resizing or mip generation. HDR inputs are clamped nonnegative and rounded to half before timing.

Each image has three timed repetitions at one and eight threads, with alternating execution
order and a warmup per configuration. Throughput uses summed per-image median times. The
benchmark includes block gathering, excludes initialization, allocations, decoding, quality
measurement, I/O and Zstd, and uses a persistent worker pool with kiln's approximate task grain.
BC7 uses linear channel weights; an independent `bcdec` decoder measures pixel-weighted pooled
RGB PSNR (higher is better). Alpha quality is measured separately in the raw results. HDR quality
is log-RMSE on `log2(1 + RGB)` (lower is better).

### BC7: opaque color, side by side

MP/s is millions of input pixels per second. All rows produce the same 16-byte BC7 block size.

| Encoder / preset | 1 thread MP/s | 8 threads MP/s | RGB PSNR (dB) |
|---|---:|---:|---:|
| Ours Fast (`bc7enc` uber 0) | 6.12 | 44.35 | 54.24 |
| Ours Normal (uber 2) | 4.23 | 29.90 | 54.75 |
| Ours High (uber 4) | 3.98 | 28.86 | 54.79 |
| Basis `bc7f` fast | 137.55 | 883.12 | 51.49 |
| Basis `bc7f` default | 100.53 | 669.06 | 52.34 |
| Basis `bc7f` partially analytical | 82.89 | 549.67 | 52.37 |
| Basis `bc7f` extended search (`DefaultNonAnalytical`) | 12.59 | 90.87 | 54.04 |
| Basis scalar `bc7e` Fast (level 2) | 1.66 | 11.85 | 54.76 |
| Basis scalar `bc7e` Basic (level 3) | 0.63 | 4.57 | 55.49 |
| Basis scalar `bc7e` Slow (level 4) | 0.77 | 5.56 | 55.47 |
| Basis scalar `bc7e` Very Slow (level 5) | 0.21 | 1.54 | 56.04 |
| Basis scalar `bc7e` Slowest (level 6) | 0.15 | 1.06 | 56.00 |

At one thread, Basis default is about 24× faster than our Normal, at a 2.40 dB loss.
Extended search is about 2× faster than our Fast, at a 0.20 dB loss. Scalar Very Slow gains
1.25 dB over our High but takes about 19× as long. Preset names do not imply monotonically
better quality or slower execution: Slowest slightly loses to Very Slow on this color corpus.

The result depends on texture usage. Against our High, `bc7f` extended search improves ORM
from 49.79 to 51.32 dB while increasing eight-thread throughput from 18.33 to 54.01 MP/s
(2.9×). On alpha textures, RGB PSNR rises from 49.93 to 52.57 dB and throughput from 28.59
to 125.30 MP/s (4.4×); the separate mean alpha PSNR rises from 54.84 to 58.64 dB. That alpha
sample is small and comes from a single model. Scalar Very Slow reaches 53.26 dB on ORM,
3.47 dB above our High, but runs at only 0.96 MP/s on eight threads.

### BC6H: HDR

| Encoder / preset | 1 thread MP/s | 8 threads MP/s | Log-RMSE |
|---|---:|---:|---:|
| Ours Fast (`veryfast`) | 18.10 | 121.69 | 0.00889 |
| Ours Normal (`fast`) | 1.98 | 13.82 | 0.00797 |
| Ours High (`basic`) | 0.77 | 5.58 | 0.00750 |
| Basis `bc6hf` default | 32.65 | 220.84 | 0.00891 |
| Basis custom search4 | 25.11 | 170.36 | 0.00887 |
| Basis custom search32 | 7.66 | 46.34 | 0.00881 |

The custom settings try 4 differential endpoint modes / 4 two-subset patterns, or 9 modes /
32 patterns with brute-force weight setting 4, respectively. Default uses 2 modes / 1 pattern;
all retain HQ least-squares refinement. More search only slightly improves this corpus:
Basis is attractive near our Fast quality, while our Normal and High retain lower HDR error.

### Dependency cost, SIMD experiment and conclusions

Minimal linked executable size deltas on this compiler were 47.5 KiB for our BC7, 19 KiB for
our BC6H, 335 KiB for Basis `bc7f`, 260 KiB for `bc6hf`, 370.5 KiB for both together, and
89.5 KiB for standalone scalar `bc7e`. The Basis transcoder probes use normal initialization
with KTX2 Zstd support disabled; these are measured integration costs, not theoretical minima
after extracting functions or trimming features. All would be cook-side dependencies.

Also tested etcpak's AVX2 BC7 at `3d716e1550023dc5ff8b602636af8b76584e0b66`. Its linear
RGB distance sums an unwanted alpha lane, causing nondeterministic output with uninitialized
palette alpha and double-counting alpha in the RGBA path. A standalone check with identical
RGB and alpha 0/255 returned RGB distance 65,025 instead of zero. Masking the fourth weight
fixed that check and thread invariance, but gave little speed benefit over our encoder for
these linear settings. Only the benchmark copy was patched; production code is unchanged.

Basis fast/extended search merits consideration for fast cooking, and standalone scalar
`bc7e` for an optional expensive offline quality mode. The results do not justify replacing
every existing preset. Current and Basis configurations, plus patched etcpak, produced identical
bytes between one and eight threads on this machine. Cross-compiler / cross-architecture
goldens and visual side-by-side decoded crops have not been checked. This measures encoder
throughput and numerical error, not whole-cook time, RDO, Zstd size or visual acceptability.

Local reproducibility artifacts are under ignored `build/bc-benchmark/`: `src/`, `CMakeLists.txt`,
`build.cmd`, `analyze.py`, `results/comparison.csv` (60 configurations), per-image CSVs and logs
for `crop1024`, `fixed1024` and `hq1024`, `results/input-manifest.json` (input hashes), and
`results/etcpak-linear-rgb-fix.patch`. These local artifacts are not committed; the measurements
above preserve the findings in the repository.

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
