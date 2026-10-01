# `.mesh` payload compression

**Status:** Implemented on branch `mesh-compression` (2026-10-01); the default scheme awaits the
owner. Measured below.
**Decides:** which payload compression scheme the cooker uses by default (mesh-format-spec §5.9,
§10), and whether blobs need splitting yet (B16).

## What the branch adds

- Every scheme of spec §5.9 in the writer and the runtime: `Basic` (ByteShuffle + Zstd for vertex
  blobs, Zstd for indices), `Meshopt` (MeshoptVertex, MeshoptIndex) and `MeshoptZstd` (the same
  with outer Zstd). Mesh settings `compression` and `zstdLevel`, sidecar keys of the same names,
  `kiln-cook --mesh-compression` and `--mesh-zstd`. A blob that does not shrink stays `None`.
- `kiln_runtime` links meshoptimizer (the cooker's pinned v1.3) for its codecs; a static link takes
  only those objects. A host with its own `meshoptimizer` target keeps it (v1.3 or newer). A
  shipping configure therefore fetches meshoptimizer, the one exception to "no network". The codecs
  allocate nothing. The loader decodes into its own memory and copies to staging (Zstd reads its
  output back).
- `MeshoptIndex` may rotate a triangle's indices; the writer stores what decodes (spec §5.9).
- `kiln-info --bench` decodes a `.mesh` repeatedly and prints the ratio and decoded MB/s.

## Measurements (2026-10-01)

i7-9700K, MSVC release, one thread. The AMC Pacer test model (5.6 MB of decoded payload in the
default profile) and the six Khronos demo models, cooked per profile and scheme. Ratio is encoded /
decoded bytes over all assets; decode is the decoded-size-weighted MB/s of `kiln-info --bench` (best
of 5, payload in cache); write is the mesh's `cook.write` zone summed over the assets.

| Profile `default` (7.53 MB) | Ratio | Decode MB/s | Write ms |
|---|---:|---:|---:|
| None | 1.000 | 33 049 (a copy) | 3 |
| Basic, Zstd 3 | 0.639 | 837 | 41 |
| Basic, Zstd 19 | 0.540 | 487 | 978 |
| **Meshopt** | **0.503** | **2 357** | 47 |
| **MeshoptZstd, Zstd 3** | **0.434** | **975** | 64 |
| MeshoptZstd, Zstd 19 | 0.424 | 846 | 386 |

| Profile `float` (13.23 MB) | Ratio | Decode MB/s | Write ms |
|---|---:|---:|---:|
| None | 1.000 | 28 140 | 5 |
| Basic, Zstd 3 | 0.639 | 841 | 67 |
| Basic, Zstd 19 | 0.568 | 559 | 1 699 |
| Meshopt | 0.594 | 2 663 | 79 |
| MeshoptZstd, Zstd 3 | 0.538 | 1 401 | 110 |
| MeshoptZstd, Zstd 19 | 0.523 | 949 | 735 |

- `Meshopt` beats `Basic` at every level on ratio and on decode speed (about 3x faster than Basic
  at Zstd 3, with a smaller file). `Basic` is dominated. Its decode is not tuned (a plain byte
  transpose), but even a free transpose would not make up the ratio gap.
- `MeshoptZstd` at level 3 is 14% smaller than `Meshopt` (default profile) and still decodes at
  about 1 GB/s. Level 19 gains 1–3% for a 6x slower write.
- Every scheme decodes the Pacer (the largest asset) in under 7 ms on one thread.

## Proposal

1. Default `Meshopt`: half the bytes of `None`, decode at about 2.4 GB/s, a cook cost of a few ms.
2. `MeshoptZstd` (level 3) as the size option for shipping builds, chosen per target or project.
3. Keep `Basic` readable (the format has it) but stop offering it as a choice, or drop it from the
   settings.
4. B16: no blob splitting yet. A whole mesh decodes in milliseconds on one thread; split rules can
   wait for streaming (v0.8) or GPU decompression.

Not measured: load time end to end with the file read (compression saves read bytes, decode costs
CPU), the meshopt filters (`MeshoptOct`/`Quat`/`Exp`, only relevant for float data), and arm64.
