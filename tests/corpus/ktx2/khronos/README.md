# Khronos KTX2 test files

Unmodified copies of files from the [KTX-Software](https://github.com/KhronosGroup/KTX-Software)
repository, used as real-world reader input for `tests/test_ktx2_corpus.cpp` and
`tests/test_ktx2_corpus_rt.cpp`. Expected values live in `../manifest.txt`.

- Source: `https://github.com/KhronosGroup/KTX-Software`, commit
  `dd3b8b0a788c9e61ca835bec3c11961d5ffe2d6f`, directory `tests/resources/ktx2/` (Git LFS; raw
  download URL `https://media.githubusercontent.com/media/KhronosGroup/KTX-Software/<commit>/tests/resources/ktx2/<file>`).
- License: Apache-2.0 (text in `LICENSE.Apache-2.0`). That repository's `REUSE.toml` annotates
  `**/**.ktx2` with `SPDX-FileCopyrightText = "2015-2022 The Khronos Group Inc."` and
  `SPDX-License-Identifier = "Apache-2.0"`; none of the files below has a more specific
  annotation there.
- Ground truth: `ktx info` / `ktx validate` from KTX-Software v4.4.2. All 12 files validate.

| File | Bytes | Source path | vkFormat | Size | Layers / faces / levels | Supercompression | KVD keys | Covers |
|---|---:|---|---|---|---|---|---|---|
| `r8g8b8a8_srgb.ktx2` | 65800 | `tests/resources/ktx2/r8g8b8a8_srgb.ktx2` | R8G8B8A8_SRGB | 128x128 | 0 / 1 / 1 | none | KTXwriter | baseline 2D RGBA8 sRGB, single level |
| `r8g8b8a8_srgb_mip.ktx2` | 22252 | `tests/resources/ktx2/r8g8b8a8_srgb_mip.ktx2` | R8G8B8A8_SRGB | 64x64 | 0 / 1 / 7 | none | KTXwriter | full mip chain, level order and alignment; source of the generated set |
| `r8g8b8_srgb_mip.ktx2` | 16788 | `tests/resources/ktx2/r8g8b8_srgb_mip.ktx2` | R8G8B8_SRGB | 64x64 | 0 / 1 / 7 | none | KTXwriter | 3-byte texels (level alignment lcm(3,4) = 12, 3-byte 1x1 level) |
| `orient_down_metadata.ktx2` | 65824 | `tests/resources/ktx2/orient_down_metadata.ktx2` | R8G8B8A8_SRGB | 128x128 | 0 / 1 / 1 | none | KTXorientation, KTXwriter | extra key/value data |
| `r8g8b8a8_srgb_array_7_mip.ktx2` | 9908 | `tests/resources/ktx2/r8g8b8a8_srgb_array_7_mip.ktx2` | R8G8B8A8_SRGB | 16x16 | 7 / 1 / 5 | none | KTXwriter | array texture with mips |
| `r8g8b8a8_srgb_3d_7.ktx2` | 7432 | `tests/resources/ktx2/r8g8b8a8_srgb_3d_7.ktx2` | R8G8B8A8_SRGB | 16x16x7 | 0 / 1 / 1 | none | KTXwriter | 3D texture: rejected as unsupported (K4105) |
| `r8g8b8a8_srgb_mip_etc2.ktx2` | 5872 | `tests/resources/ktx2/r8g8b8a8_srgb_mip_etc2.ktx2` | ETC2_R8G8B8A8_SRGB | 64x64 | 0 / 1 / 7 | none | KTXwriter | ETC2 block format, levels smaller than a block |
| `r8g8b8a8_srgb_mip_astc.ktx2` | 5856 | `tests/resources/ktx2/r8g8b8a8_srgb_mip_astc.ktx2` | ASTC_4x4_SRGB | 64x64 | 0 / 1 / 7 | none | KTXwriter | ASTC block format |
| `bc3_unorm_array_7.ktx2` | 459024 | `tests/resources/ktx2/bc3_unorm_array_7.ktx2` | BC3_UNORM | 256x256 | 7 / 1 / 1 | none | KTXorientation, KTXwriter | BC block format, array |
| `r8g8b8a8_srgb_mip_blze.ktx2` | 666 | `tests/resources/ktx2/r8g8b8a8_srgb_mip_blze.ktx2` | UNDEFINED (ETC1S) | 64x64 | 0 / 1 / 7 | BasisLZ (1) | KTXwriter | BasisLZ: unsupported (K4107) |
| `alpha_simple_blze.ktx2` | 375 | `tests/resources/ktx2/alpha_simple_blze.ktx2` | UNDEFINED (ETC1S) | 8x8 | 0 / 1 / 1 | BasisLZ (1) | KTXwriter | BasisLZ with global data: unsupported (K4107) |
| `ktx_document_uastc_rdo_4_zstd_5.ktx2` | 64160 | `tests/resources/ktx2/ktx_document_uastc_rdo_4_zstd_5.ktx2` | UNDEFINED (UASTC) | 1024x1024 | 0 / 1 / 11 | Zstd (2) | KTXwriter, KTXwriterScParams | UASTC + Zstd: unsupported (K4107) |

"Layers" is the header `layerCount` (0 means not an array).
