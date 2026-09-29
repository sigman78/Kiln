// src/cook/texture_cook.cpp — texture cooking: PNG/JPEG/WebP -> KTX2 (decode, convert per
// usage, mips, BC encode) or KTX2 pass-through.
#include "bc_encode.h"
#include "cook_internal.h"

#include "kiln/cook/image.h"
#include "kiln/cook/ktx2_writer.h"
#include "kiln/log.h"

#include <cstring>

namespace kiln::cook {

namespace {

constexpr u32 kNoLimit = 0xFFFFFFFFu;

Status fail(DiagSink const* diag, StrView asset, Status status, u32 code, char const* fmt,
            unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0) noexcept {
    return diagf(diag, status, code, Severity::Error, asset, "texture", fmt, a, b, c);
}

void note(DiagSink const* diag, StrView asset, Severity sev, u32 code, char const* fmt,
          unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0,
          unsigned long long d = 0) noexcept {
    (void)diagf(diag, kOk, code, sev, asset, "texture", fmt, a, b, c, d);
}

/// The CookShape a KTX2 texture has.
CookShape shape_of(ktx2::TextureDesc const& d) noexcept {
    return d.isCube ? CookShape::Cube : d.isArray ? CookShape::Array : CookShape::Tex2D;
}

Result<CookedTexture> pass_through(TextureSource const& src, CookShape shape, u32 cap, u32 maxLayers,
                                   Allocator const* alloc, DiagSink const* diag, StrView asset,
                                   u64 sourceHash) noexcept {
    Result<ktx2::Ktx2View> r = ktx2::Ktx2View::open(src.bytes, diag, asset);
    if (r.failed())
        return fail(diag, asset, r.status(), kDiagImagePassthroughBad,
                    "KTX2 source rejected by the reader (supercompressed, 3D or invalid)");
    ktx2::Ktx2View const& view = r.value();
    ktx2::TextureDesc const d  = view.desc();
    if (view.header().supercompressionScheme != 0)
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "supercompressed KTX2 cannot be passed through");
    if (d.depth > 1 || (d.isArray && d.isCube))
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "only 2D, cube and array KTX2 can be passed through (depth %llu, cube array %llu)",
                    d.depth, d.isArray && d.isCube ? 1u : 0u);
    if (shape != CookShape::Auto && shape != shape_of(d))
        return diagf(diag, make_status(Code::InvalidArgument), kDiagImagePassthroughBad, Severity::Error,
                     asset, "texture",
                     "the KTX2 source is %s, the settings ask for %s (pass-through cannot reshape)",
                     cook_shape_name(shape_of(d)), cook_shape_name(shape));
    if (d.layers > maxLayers)
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "%llu array layers exceed the target limit %llu", d.layers, maxLayers);
    if (!view.has_all_level_data())
        return fail(diag, asset, make_status(Code::Corrupt), kDiagImagePassthroughBad,
                    "KTX2 file is missing level data");
    if (max(d.width, d.height) > cap)
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "KTX2 extent %llux%llu exceeds the size cap %llu (pass-through cannot downscale)",
                    d.width, d.height, cap);

    CookedTexture out;
    out.file.init(alloc, Tag::Cook);
    out.file.append(src.bytes);
    out.desc        = d;
    out.passthrough = true;
    out.sourceHash  = sourceHash;
    return out;
}

struct Plan {
    Format format = Format::Undefined;
    u32 channels  = 0;
    u32 bits      = 0;
    bool rgba8    = false; ///< RGBA8 target; a gray+alpha source expands as (Y, Y, Y, A)
    MipOptions mips;
    bool normal = false;
};

/// The block format of an encoding, or Undefined to stay uncompressed. Auto follows the usage
/// table of bcn-encoding.md when the target has the BC family.
Format block_format(TextureEncoding e, BlockFamily family, TextureUsage usage, u32 srcChannels,
                    bool srgb) noexcept {
    if (e == TextureEncoding::Auto) {
        if (family != BlockFamily::BC) return Format::Undefined;
        switch (usage) {
        case TextureUsage::Auto:
        case TextureUsage::Color:
        case TextureUsage::Ui:
        case TextureUsage::Orm: e = TextureEncoding::BC7; break;
        case TextureUsage::Normal: e = TextureEncoding::BC5; break;
        case TextureUsage::Mask: e = srcChannels == 2 ? TextureEncoding::BC5 : TextureEncoding::BC4; break;
        case TextureUsage::Hdr:
        case TextureUsage::Lut:
        case TextureUsage::Height: return Format::Undefined;
        }
    }
    switch (e) {
    case TextureEncoding::BC1: return srgb ? Format::BC1_RGB_SRGB : Format::BC1_RGB_UNORM;
    case TextureEncoding::BC3: return srgb ? Format::BC3_SRGB : Format::BC3_UNORM;
    case TextureEncoding::BC4: return Format::BC4_UNORM;
    case TextureEncoding::BC5: return Format::BC5_UNORM;
    case TextureEncoding::BC7: return srgb ? Format::BC7_SRGB : Format::BC7_UNORM;
    case TextureEncoding::Auto:
    case TextureEncoding::Uncompressed:
    case TextureEncoding::BC6H: return Format::Undefined;
    }
    return Format::Undefined;
}

/// The 8-bit image a block format encodes from: 1 channel for BC4, 2 for a BC5 mask, else RGBA
/// (a BC5 normal keeps the normal plan and encodes its R and G).
Plan block_plan(Image const& img, TextureUsage usage, bool srgb, Format bc, DiagSink const* diag,
                StrView asset) noexcept {
    Plan p;
    p.format = bc;
    p.bits   = 8;
    p.normal = usage == TextureUsage::Normal;
    if (bc == Format::BC4_UNORM)
        p.channels = 1;
    else if (bc == Format::BC5_UNORM && !p.normal)
        p.channels = 2;
    else {
        p.channels  = 4;
        p.rgba8     = true;
        p.mips.srgb = srgb;
    }
    if (p.normal && img.channels < 3)
        note(diag, asset, Severity::Warning, kDiagImageChannelMismatch,
             "normal map has %llu channel(s); expanded to RGBA", img.channels);
    else if (!p.rgba8 && img.channels > p.channels)
        (void)diagf(diag, kOk, kDiagImageChannelMismatch, Severity::Warning, asset, "texture",
                    "%s keeps the first %u of %u channels", format_name(bc), p.channels, img.channels);
    return p;
}

Plan plan_for(Image const& img, TextureUsage usage, ColorSpace cs, DiagSink const* diag,
              StrView asset) noexcept {
    Plan p;
    switch (usage) {
    case TextureUsage::Normal:
        if (img.channels < 3)
            note(diag, asset, Severity::Warning, kDiagImageChannelMismatch,
                 "normal map has %llu channel(s); expanded to RGBA", img.channels);
        p.format = Format::R8G8B8A8_UNORM;
        p.normal = true;
        break;
    case TextureUsage::Orm: p.format = Format::R8G8B8A8_UNORM; break;
    case TextureUsage::Mask:
    case TextureUsage::Height:
    case TextureUsage::Lut:
        if (img.channels == 1) {
            p.channels = 1;
            p.bits     = img.bitsPerChannel;
            p.format   = img.bitsPerChannel == 16 ? Format::R16_UNORM : Format::R8_UNORM;
            return p;
        }
        if (img.channels == 2) {
            p.channels = 2;
            p.bits     = 8;
            p.format   = Format::R8G8_UNORM;
            return p;
        }
        if (usage == TextureUsage::Lut) {
            p.format = Format::R8G8B8A8_UNORM;
            break;
        }
        (void)diagf(diag, kOk, kDiagImageChannelMismatch, Severity::Warning, asset, "texture",
                    "%s texture has %u channels; keeping only the first (R)",
                    usage == TextureUsage::Mask ? "Mask" : "Height", img.channels);
        p.channels = 1;
        p.bits     = img.bitsPerChannel;
        p.format   = img.bitsPerChannel == 16 ? Format::R16_UNORM : Format::R8_UNORM;
        return p;
    case TextureUsage::Auto:
    case TextureUsage::Color:
    case TextureUsage::Ui:
    case TextureUsage::Hdr: // v0.5: no float formats, HDR is cooked like color
        p.format    = cs == ColorSpace::Srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
        p.mips.srgb = cs == ColorSpace::Srgb;
        break;
    }
    p.channels = 4;
    p.bits     = 8;
    p.rgba8    = true;
    return p;
}

bool is_pow2(u32 v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

Result<CookedTexture> cook_decoded(TextureSource const& src, TextureCookSettings const& settings,
                                   TargetProfile const& target, u32 cap, Allocator const* alloc,
                                   DiagSink const* diag, JobBudget const& budget, StrView asset,
                                   u64 sourceHash) noexcept {
    detail::Stopwatch const swTotal;
    detail::Stopwatch const swDecode;
    KILN_TRY_ASSIGN(Image decoded, decode_image(src.bytes, alloc, diag, asset));
    u64 const decodeUs = swDecode.elapsed_us();

    TextureUsage const usage = settings.usage == TextureUsage::Auto ? TextureUsage::Color : settings.usage;
    ColorSpace const cs =
        settings.colorSpace == ColorSpace::Auto ? color_space_for(usage) : settings.colorSpace;
    // HDR (hdr-textures.md): usage Hdr cooks f32 texels to RGBA16F. A float source has no
    // integer plan; an integer source marked Hdr keeps its 0..1 range.
    bool const hdr         = usage == TextureUsage::Hdr;
    bool const floatSource = decoded.bitsPerChannel == 32;
    if (floatSource && !hdr)
        return diagf(diag, make_status(Code::Unsupported), kDiagImageUnsupported, Severity::Error, asset,
                     "texture", "an HDR source needs usage hdr, not %s", texture_usage_name(usage));
    if (hdr && !floatSource)
        (void)diagf(diag, kOk, kDiagImageNoHdrRange, Severity::Warning, asset, "texture",
                    "usage hdr with a %u-bit source: values stay in 0..1", decoded.bitsPerChannel);
    // Only Color and Ui have sRGB block formats, as in the uncompressed plans.
    bool const srgb = cs == ColorSpace::Srgb && (usage == TextureUsage::Color || usage == TextureUsage::Ui);
    Format const bc =
        hdr ? Format::Undefined
            : block_format(settings.encoding, target.blockFamily, usage, decoded.channels, srgb);
    Plan plan;
    if (hdr) {
        plan.format   = Format::R16G16B16A16_SFLOAT;
        plan.channels = 4;
        plan.bits     = 32;
    } else if (bc != Format::Undefined) {
        plan = block_plan(decoded, usage, srgb, bc, diag, asset);
    } else {
        plan = plan_for(decoded, usage, cs, diag, asset);
    }
    if ((usage == TextureUsage::Normal || usage == TextureUsage::Height) && is_lossy_image(src.bytes))
        (void)diagf(
            diag, kOk, kDiagImageLossySource, Severity::Warning, asset, "texture",
            "%s texture comes from a lossy source (JPEG or lossy WebP); artifacts show as shading noise",
            usage == TextureUsage::Normal ? "Normal" : "Height");

    detail::Stopwatch const swPrepare;
    Result<Image> converted =
        prepare_image(decoded, plan.channels, plan.bits,
                      PrepareOptions{.grayAlpha   = (plan.rgba8 || hdr) && decoded.channels == 2,
                                     .flipGreen   = plan.normal && settings.flipGreen,
                                     .renormalize = plan.normal && settings.normalRenormalize},
                      alloc, budget);
    if (converted.failed())
        return fail(diag, asset, converted.status(), kDiagImageUnsupported, "image conversion failed");
    Image img = std::move(converted).value();
    decoded.pixels.release();

    if (plan.normal) {
        if (settings.normalRenormalize) plan.mips.renormalize = true;
    }
    u64 const prepareUs = swPrepare.elapsed_us();

    // A Cube or Array source is a vertical strip, slice 0 at the top (texture-shapes.md). Each
    // slice is one contiguous block of the row-major image.
    CookShape const shape = settings.shape == CookShape::Auto ? CookShape::Tex2D : settings.shape;
    u32 const stripW = img.width, stripH = img.height;
    u32 slices = 1, sliceH = stripH;
    if (shape == CookShape::Cube) {
        if (stripH != stripW * 6)
            return fail(diag, asset, make_status(Code::InvalidArgument), kDiagImageSliceLayout,
                        "a cube strip is 6 square faces (W x 6W), not %llux%llu", stripW, stripH);
        slices = 6;
        sliceH = stripW;
    } else if (shape == CookShape::Array) {
        bool const square = settings.slices == 0;
        slices            = square ? stripH / stripW : settings.slices;
        if (slices == 0 || stripH % slices != 0 || (square && stripH % stripW != 0))
            return fail(diag, asset, make_status(Code::InvalidArgument), kDiagImageSliceLayout,
                        "an array strip of %llux%llu does not divide into %llu slices", stripW, stripH,
                        square ? 0u : slices);
        if (slices > target.maxArrayLayers)
            return fail(diag, asset, make_status(Code::InvalidArgument), kDiagImageSliceLayout,
                        "%llu array layers exceed the target limit %llu", slices, target.maxArrayLayers);
        sliceH = stripH / slices;
    }

    // Size cap per slice: drop leading levels until the top fits.
    u32 const srcW = stripW, srcH = sliceH;
    u32 drop = 0;
    while (max(srcW >> drop, 1u) > cap || max(srcH >> drop, 1u) > cap)
        ++drop;
    u32 const topW = max(srcW >> drop, 1u);
    u32 const topH = max(srcH >> drop, 1u);
    if (drop > 0)
        note(diag, asset, Severity::Info, kDiagImageDownscaled,
             "%llux%llu exceeds the size cap %llu; dropped %llu top level(s)", srcW, srcH, cap, drop);

    u32 const fullLevels = u32(std::bit_width(max(srcW, srcH)));
    u32 const buildCount = settings.genMips ? fullLevels : drop + 1;
    detail::Stopwatch const swMips;
    Vec<Vec<Image>> chains(alloc, Tag::Cook);
    chains.reserve(slices);
    if (slices == 1) {
        KILN_TRY_ASSIGN(Vec<Image> chain,
                        build_mip_chain(std::move(img), plan.mips, buildCount, alloc, budget));
        chains.push_back(std::move(chain));
    } else {
        usize const sliceBytes = usize(img.row_bytes()) * sliceH;
        for (u32 i = 0; i < slices; ++i) {
            Image slice{.width          = srcW,
                        .height         = srcH,
                        .channels       = img.channels,
                        .bitsPerChannel = img.bitsPerChannel,
                        .pixels         = Vec<u8>(alloc, Tag::Cook)};
            slice.pixels.append(Span<u8 const>(img.pixels.data() + i * sliceBytes, sliceBytes));
            KILN_TRY_ASSIGN(Vec<Image> chain,
                            build_mip_chain(std::move(slice), plan.mips, buildCount, alloc, budget));
            chains.push_back(std::move(chain));
        }
        img.pixels.release();
    }
    u64 const mipsUs     = swMips.elapsed_us();
    u32 const levelCount = u32(chains[0].size()) - drop;
    KILN_VERIFY(levelCount >= 1 && levelCount <= ktx2::kMaxLevels);
    KILN_VERIFY(chains[0][drop].width == topW && chains[0][drop].height == topH);

    if (levelCount > 1 && (!is_pow2(topW) || !is_pow2(topH)))
        note(diag, asset, Severity::Info, kDiagImageNpotMips,
             "non-power-of-two %llux%llu with mips: levels use floor halving", topW, topH);

    // A KTX2 level holds every slice of that level, slice 0 first.
    bool const blocks = bc != Format::Undefined;
    Span<u8 const> levels[ktx2::kMaxLevels];
    Vec<u8> levelBytes[ktx2::kMaxLevels];
    detail::Stopwatch const swEncode;
    for (u32 i = 0; i < levelCount; ++i) {
        if (slices == 1 && !hdr && !blocks) {
            levels[i] = chains[0][drop + i].pixels.span();
            continue;
        }
        levelBytes[i].init(alloc, Tag::Cook);
        usize const sliceBytes = chains[0][drop + i].pixels.size();
        if (blocks) {
            for (Vec<Image> const& chain : chains)
                bc_encode(chain[drop + i], bc, settings.quality, levelBytes[i], budget);
        } else if (!hdr) {
            levelBytes[i].reserve(sliceBytes * slices);
            for (Vec<Image> const& chain : chains)
                levelBytes[i].append(chain[drop + i].pixels.span());
        } else {
            // f32 to half, once per level after the mip chain.
            levelBytes[i].resize(sliceBytes / 2 * slices);
            u8* out = levelBytes[i].data();
            for (Vec<Image> const& chain : chains) {
                u8 const* px = chain[drop + i].pixels.data();
                for (usize at = 0; at < sliceBytes; at += 4, out += 2) {
                    f32 v;
                    std::memcpy(&v, px + at, 4);
                    u16 const h = float_to_half(v);
                    std::memcpy(out, &h, 2);
                }
            }
        }
        levels[i] = levelBytes[i].span();
    }
    u64 const encodeUs = blocks ? swEncode.elapsed_us() : 0;

    // Content identity for invalidation (named store layout, open-questions R4).
    char sourceHex[17], cookHex[17];
    format(sourceHex, sizeof sourceHex, "%016llx", static_cast<unsigned long long>(sourceHash));
    u64 const cookHash =
        hash_combine(hash_combine(hash_settings(settings), hash_target(target)), u64(kCookerVersion));
    format(cookHex, sizeof cookHex, "%016llx", static_cast<unsigned long long>(cookHash));
    ktx2::KeyValue const extra[] = {
        {"kiln.cookHash",   StrView(cookHex)  },
        {"kiln.sourceHash", StrView(sourceHex)},
    };
    ktx2::WriteDesc const wd = {
        .format             = plan.format,
        .width              = topW,
        .height             = topH,
        .layers             = shape == CookShape::Array ? slices : 1,
        .faces              = shape == CookShape::Cube ? 6u : 1u,
        .isArray            = shape == CookShape::Array,
        .levels             = {levels, levelCount},
        .writerTag          = "kiln-cook",
        .premultipliedAlpha = false,
        .extraKeys          = extra,
    };
    detail::Stopwatch const swWrite;
    KILN_TRY_ASSIGN(Vec<u8> file, ktx2::write(wd, alloc, diag));
    u64 const writeUs = swWrite.elapsed_us();

    CookedTexture out;
    out.file            = std::move(file);
    out.desc            = ktx2::TextureDesc{.format  = plan.format,
                                            .width   = topW,
                                            .height  = topH,
                                            .layers  = shape == CookShape::Array ? slices : 1,
                                            .faces   = shape == CookShape::Cube ? 6u : 1u,
                                            .levels  = levelCount,
                                            .isArray = shape == CookShape::Array,
                                            .isCube  = shape == CookShape::Cube};
    out.passthrough     = false;
    out.sourceHash      = sourceHash;
    out.stats.decodeUs  = decodeUs;
    out.stats.prepareUs = prepareUs;
    out.stats.mipsUs    = mipsUs;
    out.stats.encodeUs  = encodeUs;
    out.stats.writeUs   = writeUs;
    out.stats.totalUs   = swTotal.elapsed_us();
    return out;
}

} // namespace

Result<CookedTexture> cook_texture(TextureSource const& src, TextureCookSettings const& settings,
                                   TargetProfile const& target, CookEnv const& env) noexcept {
    Allocator const* const alloc = env.alloc ? env.alloc : default_allocator();
    DiagSink const* const diag   = env.diag;
    StrView const asset          = src.assetPath.empty() ? src.sourcePath : src.assetPath;
    u64 const sourceHash         = src.sourceHash ? src.sourceHash : xxh64(src.bytes);
    u32 const settingsCap        = settings.maxSize ? settings.maxSize : kNoLimit;
    u32 const targetCap          = target.maxTextureSize ? target.maxTextureSize : kNoLimit;
    u32 const cap                = min(settingsCap, targetCap);

    if (is_ktx2(src.bytes))
        return pass_through(src, settings.shape, cap, target.maxArrayLayers, alloc, diag, asset, sourceHash);
    if (is_png(src.bytes) || is_jpeg(src.bytes) || is_webp(src.bytes) || is_hdr(src.bytes))
        return cook_decoded(src, settings, target, cap, alloc, diag, JobBudget{env.jobs, env.maxThreads},
                            asset, sourceHash);
    return fail(diag, asset, make_status(Code::Unsupported), kDiagImageUnknownFormat,
                "source is not PNG, JPEG, WebP or KTX2 (%llu bytes)", src.bytes.size);
}

} // namespace kiln::cook
