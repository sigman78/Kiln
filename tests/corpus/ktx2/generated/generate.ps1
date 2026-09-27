# Regenerates tests/corpus/ktx2/generated/*.ktx2 with the reference KTX-Software CLI.
#
# Tool: ktx v4.4.2 (KTX-Software 4.4.2), on PATH. Run with PowerShell 7+ (pwsh; needs
# [System.Half]) from any directory:
#
#   pwsh -File tests/corpus/ktx2/generated/generate.ps1
#
# Inputs:
#   - src8.png / src16.png / src8.raw: level 3 (8x8) and level 2 (16x16) of
#     ../khronos/r8g8b8a8_srgb_mip.ktx2 (Apache-2.0, see ../khronos/README.md), pulled
#     out with `ktx extract`.
#   - f16.raw: a synthetic 4x4 RGBA half-float gradient built below (no source image).
# --testrun makes ktx write a fixed KTXwriter string so the output is reproducible.
# Every output is checked with `ktx validate`. After regenerating, update
# ../manifest.txt if anything changed (`ktx info` gives the ground truth).
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$khr  = Join-Path $here '..\khronos\r8g8b8a8_srgb_mip.ktx2'
$tmp  = Join-Path ([System.IO.Path]::GetTempPath()) ('kiln-ktx2-gen-' + [System.Guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null

function Invoke-Ktx {
    & ktx @args
    if ($LASTEXITCODE -ne 0) { throw "ktx $($args -join ' ') failed ($LASTEXITCODE)" }
}

& ktx --version
Push-Location $tmp
try {
    Invoke-Ktx extract --level 3 $khr src8.png          # 8x8, sRGB
    Invoke-Ktx extract --level 2 $khr src16.png         # 16x16, sRGB
    Invoke-Ktx extract --raw --level 3 $khr src8.raw    # 256 bytes of RGBA8

    # 4x4 RGBA16F gradient: r = x/3, g = y/3, b = 0.5, a = 1.
    $f16 = [System.Collections.Generic.List[byte]]::new()
    for ($y = 0; $y -lt 4; ++$y) {
        for ($x = 0; $x -lt 4; ++$x) {
            foreach ($v in @(($x / 3.0), ($y / 3.0), 0.5, 1.0)) {
                $f16.AddRange([System.BitConverter]::GetBytes([System.Half]$v))
            }
        }
    }
    [System.IO.File]::WriteAllBytes((Join-Path $tmp 'f16.raw'), $f16.ToArray())

    $out = $here
    $six = @('src8.png') * 6

    # Cubemap: 6 identical faces, full mip chain (8x8 -> 1x1, 4 levels).
    Invoke-Ktx create --testrun --format R8G8B8A8_SRGB --cubemap --generate-mipmap @six "$out\cube_rgba8_srgb_mip.ktx2"
    # 1D: the 256 raw bytes read as a 64x1 RGBA8 row.
    Invoke-Ktx create --testrun --format R8G8B8A8_SRGB --1d --raw --width 64 --height 1 src8.raw "$out\1d_rgba8_srgb.ktx2"
    # Single / dual channel: ktx drops the extra channels; --assign-tf linear keeps it
    # from converting sRGB -> linear.
    Invoke-Ktx create --testrun --format R8_UNORM --assign-tf linear src8.png "$out\r8_unorm.ktx2"
    Invoke-Ktx create --testrun --format R8G8_UNORM --assign-tf linear src8.png "$out\r8g8_unorm.ktx2"
    # Half float from raw data (PNG input is refused for SFLOAT formats).
    Invoke-Ktx create --testrun --format R16G16B16A16_SFLOAT --raw --width 4 --height 4 f16.raw "$out\rgba16f.ktx2"
    # Linear RGBA8 with generated mips: POT 16x16 and NPOT 7x5 (resampled by --width/--height).
    Invoke-Ktx create --testrun --format R8G8B8A8_UNORM --assign-tf linear --generate-mipmap src16.png "$out\rgba8_unorm_mip.ktx2"
    Invoke-Ktx create --testrun --format R8G8B8A8_UNORM --assign-tf linear --width 7 --height 5 --generate-mipmap src16.png "$out\rgba8_unorm_npot_mip.ktx2"
    # Extra key/value data: KTXswizzle and KTXorientation.
    Invoke-Ktx create --testrun --format R8G8B8A8_SRGB --swizzle rgb1 --assign-texcoord-origin bottom-left src8.png "$out\rgba8_srgb_kvd.ktx2"
    # Supercompressed uncompressed formats (unsupported by kiln v0.5).
    Invoke-Ktx create --testrun --format R8G8B8A8_SRGB --generate-mipmap --zstd 5 src16.png "$out\rgba8_srgb_mip_zstd.ktx2"
    Invoke-Ktx create --testrun --format R8G8B8A8_SRGB --generate-mipmap --zlib 5 src16.png "$out\rgba8_srgb_mip_zlib.ktx2"
} finally {
    Pop-Location
    Remove-Item -Recurse -Force $tmp
}

$failed = 0
foreach ($f in Get-ChildItem (Join-Path $here '*.ktx2')) {
    & ktx validate $f.FullName
    if ($LASTEXITCODE -ne 0) { Write-Host "INVALID: $($f.Name)"; ++$failed }
    else { Write-Host "valid: $($f.Name) ($($f.Length) bytes)" }
}
if ($failed) { exit 1 }
