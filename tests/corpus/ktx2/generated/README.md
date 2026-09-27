# Generated KTX2 variants

Small files made with the reference `ktx` CLI (KTX-Software **v4.4.2**) to cover cases the
Khronos set in `../khronos/` lacks. `generate.ps1` holds the exact commands; run it with
PowerShell 7 (`pwsh -File tests/corpus/ktx2/generated/generate.ps1`). Output is deterministic
(`--testrun` fixes the KTXwriter string), and every file passes `ktx validate`.

Pixel data comes from level 2 / level 3 of `../khronos/r8g8b8a8_srgb_mip.ktx2`
(Apache-2.0, The Khronos Group Inc.; see `../khronos/README.md` and
`../khronos/LICENSE.Apache-2.0`), except `rgba16f.ktx2`, whose 4x4 gradient the script
computes itself.

| File | Bytes | Format | Size | Faces / levels | Covers | kiln v0.5 |
|---|---:|---|---|---|---|---|
| `cube_rgba8_srgb_mip.ktx2` | 2376 | R8G8B8A8_SRGB | 8x8 | 6 / 4 | cubemap with generated mips | ok |
| `r8_unorm.ktx2` | 280 | R8_UNORM | 8x8 | 1 / 1 | single channel, 1-byte texels | ok |
| `r8g8_unorm.ktx2` | 360 | R8G8_UNORM | 8x8 | 1 / 1 | two channels | ok |
| `rgba16f.ktx2` | 392 | R16G16B16A16_SFLOAT | 4x4 | 1 / 1 | half float, typeSize 2 | ok |
| `rgba8_unorm_mip.ktx2` | 1724 | R8G8B8A8_UNORM | 16x16 | 1 / 5 | linear RGBA8, generated mips | ok |
| `rgba8_unorm_npot_mip.ktx2` | 480 | R8G8B8A8_UNORM | 7x5 | 1 / 3 | non-power-of-two mip chain | ok |
| `rgba8_srgb_kvd.ktx2` | 564 | R8G8B8A8_SRGB | 8x8 | 1 / 1 | KTXswizzle + KTXorientation metadata | ok |
| `1d_rgba8_srgb.ktx2` | 520 | R8G8B8A8_SRGB | 64 (1D) | 1 / 1 | 1D texture (pixelHeight 0) | unsupported, K4105 |
| `rgba8_srgb_mip_zstd.ktx2` | 485 | R8G8B8A8_SRGB | 16x16 | 1 / 5 | Zstd supercompression of an uncompressed format | unsupported, K4107 |
| `rgba8_srgb_mip_zlib.ktx2` | 493 | R8G8B8A8_SRGB | 16x16 | 1 / 5 | Zlib supercompression | unsupported, K4107 |

Not generated: R32G32B32A32_SFLOAT (covered by the half-float file; `ktx create` refuses PNG
input for SFLOAT formats, so float data must come from `--raw`).
