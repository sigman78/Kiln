# Zstd supercompression of KTX2 textures

**Status:** Decided (owner, 2026-09-29): standard KTX2 Zstd (scheme 2), on by default; a
kiln-specific filter scheme and RDO are deferred. Implemented the same day (`bcn-encoding.md`
step 6); the rule "only when it pays" followed on request. Choices made while implementing are
open-questions R10.
**Decides:** How cooked textures are compressed on disk, the codec and why, which files stay
plain, and what the runtime does to load them.

## Summary

Uncompressed textures in the store were large, and BC encoding is too slow for fast iteration.
Zstd compresses each mip level of a KTX2 file losslessly. The file shrinks, and so do the bytes a
load reads; GPU memory stays the same. On the example assets, the uncompressed store shrinks from
437 MB to 68 MB and the BC store from 109 MB to 38 MB, for a few percent more cooking. Decoding
runs at about 1 GB/s per core, straight into the adapter's upload memory.

KTX2 defines Zstd as scheme 2, so the files stay standard: libktx and other tools read them.

## What exists

- The KTX2 reader accepts scheme 2; `Ktx2View::decode_level` gives a level's texels (K4110 when
  a level is not one frame of its size). BasisLZ and Zlib stay K4107.
- The loader reads each frame into scratch memory and decodes it into the staging memory, or into
  a second scratch buffer when the adapter pads rows.
- The writer compresses each level into one frame (`WriteDesc::zstdLevel`) with fixed
  parameters: content size on, checksum off, one thread. The goldens pin the frames.
- Settings: `supercompression` (`Zstd` by default, or `None`) and `zstdLevel` (0 = 3; 1 under
  `fastPreview`); `kiln-cook --zstd <level>`, 0 = off (`settings.md`).
- zstd 1.5.7 is vendored in `third_party/zstd/`: the decoder in `kiln_runtime`, the encoder in
  `kiln_cook` (`dependencies.md`, "Zstd").

## Measurements

A throwaway harness outside the repository compressed every mip level of the example stores
(39 textures, mostly 2048², cooked once uncompressed and once as BC), each level on its own, as
KTX2 stores them. One thread, i7-9700K, MSVC. Every level decoded back to the same bytes.

Seven textures are almost flat (black emissive maps, constant fills, test sky cubes) and
compress 50 to 2500 times. They distort totals, so the tables show the other 32 ("natural").

**Uncompressed store (346 MB of natural textures):**

| Codec | MB | Ratio | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|---:|
| zstd 1 | 75.6 | 4.6 | 419 | 1033 |
| zstd 3 | 69.9 | 5.0 | 310 | 979 |
| zstd 9 | 60.6 | 5.7 | 68 | 1215 |
| zstd 19 | 50.1 | 6.9 | 4 | 1164 |
| LZ4 | 113.9 | 3.0 | 797 | 3651 |
| LZ4 HC 9 | 76.1 | 4.5 | 19 | 4141 |

Per texture, zstd 1 saves from 1.6 to 18.8 times (median 4.3). These textures have much empty
space and a constant alpha channel; dense photographs compress less.

**BC store (86 MB of natural textures):** zstd 3 gives 2.2 times (median 2.1, lowest 1.08),
at 1.1 GB/s decode. Splitting BC5 blocks into endpoint and index streams gains 10% on normal maps
only.

**Pixel filters before zstd 3** (byte planes, then a prediction from neighbours):

| Filter | Color | Normal | ORM |
|---|---:|---:|---:|
| none | 4.95 | 4.87 | 5.02 |
| planes + left delta | 4.92 | 6.42 | 9.01 |
| planes + median edge detector | 5.12 | 6.38 | 8.91 |
| planes + median + subtract green | 6.86 | – | 4.13 |

The best filter per role saves a further 31%. A tuned scalar inverse runs at 3.8 GB/s (left
delta) and 0.66 GB/s (median).

**RDO** (bc7enc_rdo's reference encoder, level 0 of 11 textures, zstd 9): the BC files get about
twice smaller, but lose 4 to 8 dB PSNR, and encoding is about 5 times slower than BC7 itself.
bc7enc's entropy-reduction mode alone gives 11% on color for 1 dB, at no speed cost.

**Fixed cost:** creating a decoder context and decoding a 4 KiB frame takes about 0.2 µs.

## Decision

1. **Standard KTX2 Zstd (scheme 2)**, one frame per level, so a loader can still read levels
   one by one and other tools can read the files.
2. **On by default** at level 3; `fastPreview` uses level 1.
3. **Only when it pays:** the cook keeps a texture's Zstd levels only when they shrink the file by
   at least 10% (`kZstdMinSaving`), counted in 4 KiB disk blocks.
   - A file under 4 KiB takes one block either way, so it stays plain.
   - A texture that barely compresses (a noisy BC5 normal map saved 7%) stays plain: decoding
     would cost CPU for little space.
   - KTX2 names one scheme per file, so the choice is per file, not per level.
   - Decoding has no fixed cost worth a size rule of its own (0.2 µs).
   - On the example assets the rule keeps 2 of 39 BC textures plain (0.1 MB more) and none of the
     uncompressed ones.
4. **zstd vendored**, decoder-only in the runtime, built without assembly and without threads,
   its memory from the kiln `Allocator`.

## Alternatives considered

- **LZ4:** decodes 3.5 times faster, but files are 1.5 times bigger than zstd 1, and it is not a
  KTX2 scheme. zstd already decodes faster than a disk reads, per core.
- **Oodle** (Kraken, Mermaid, Selkie): the best ratio for its decode speed, but not free outside
  Unreal Engine.
- **Basis Universal** (v2.50): not lossless compression of an existing texture but its own
  codecs. The cooker re-encodes the texels, and every load transcodes them to BC or ASTC.
  Measured after the decision on level 0 of 11 of the textures above (4 color, 3 ORM, 4 normal),
  transcoded to BC7 (BC5 for normals), against kiln's BC7/BC5 + Zstd 3 (3.4–3.8 bits per texel,
  54.5 dB color, 66 dB ORM, 55 dB normal):

  | Mode | Bits/texel (color) | PSNR color / ORM / normal | Encode | Transcode |
  |---|---:|---|---:|---:|
  | ETC1S (BasisLZ), quality 100 | 0.52 | 38 / 39 / 43 dB | 0.9 MP/s | 165 MP/s |
  | UASTC + Zstd | 3.81 | 52 / 57 / 49 dB | 0.3 MP/s | 94 MP/s |
  | UASTC, RDO quality 90, + Zstd | 2.83 | 47 / 54 / 45 dB | 0.1 MP/s | 90 MP/s |
  | XUBC7 lossless, `bc7e_scalar` 4 | 2.78 | 55 / 69 / – dB | 0.4 MP/s | 76 MP/s |

  - ETC1S is 7–9 times smaller but 12–27 dB worse: for web and mobile distribution only.
  - UASTC without RDO is bigger and worse than BC7 + Zstd and 10 times slower to encode; with
    RDO it trades 7–11 dB for 25–35%. Its value is one file for ASTC and BC GPUs.
  - XUBC7 (supercompressed BC7) reaches kiln's quality with 19% fewer bytes on color and 11% on
    ORM. But it encodes about 9 times slower than bc7enc (3.5 MP/s), transcodes about 15 times
    slower than Zstd decodes (60 ms against 4 ms for a 2048² texture), needs a 1.7 MB transcoder in
    the runtime, and has no BC5 path for normal maps.
  - None of this helps fast iteration. Basis belongs with the mobile and web targets
    (`bcn-encoding.md` step 7); XUBC7 is a candidate for a shipping build's size budget.
- **A kiln filter scheme** (the pixel filters above, with a vendor scheme id from `0x10000`):
  31% smaller uncompressed stores, but only kiln could read the files. Deferred until disk
  space still hurts with Zstd alone.
- **RDO:** too slow and too lossy for iteration; a candidate for shipping builds
  (`rdoLambda`, off by default).

## Consequences

- `kiln_runtime` has its first third-party code, zstd's decoder. It is not optional: a runtime
  without it could not load a default store.
- `Ktx2View::level_data()` of a supercompressed file holds frames; tools that need texels call
  `decode_level()`.
- The cooker's output depends on the zstd version: a zstd update changes the goldens and needs a
  `kCookerVersion` bump.
- A host that links its own zstd statically may clash with kiln's copy (open-questions R10).

## Open points

- Whether the 10% threshold should become a setting (a host with slow disks may want less).
- The filter scheme and RDO, as above.
