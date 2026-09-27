// src/cook/texture_cook.cpp — texture cooking: PNG -> KTX2 (decode, convert per usage,
// mips) or KTX2 pass-through.
#include "cook_internal.h"

#include "kiln/cook/image.h"
#include "kiln/cook/ktx2_writer.h"
#include "kiln/log.h"

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

Result<CookedTexture> pass_through(TextureSource const& src, u32 cap, Allocator const* alloc,
                                   DiagSink const* diag, StrView asset, u64 sourceHash) noexcept {
    Result<ktx2::Ktx2View> r = ktx2::Ktx2View::open(src.bytes, diag, asset);
    if (r.failed())
        return fail(diag, asset, r.status(), kDiagImagePassthroughBad,
                    "KTX2 source rejected by the reader (supercompressed, 3D or invalid)");
    ktx2::Ktx2View const& view = r.value();
    ktx2::TextureDesc const d  = view.desc();
    if (view.header().supercompressionScheme != 0)
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "supercompressed KTX2 cannot be passed through");
    if (d.depth > 1 || d.isArray || d.isCube)
        return fail(diag, asset, make_status(Code::Unsupported), kDiagImagePassthroughBad,
                    "only plain 2D KTX2 can be passed through (depth %llu, array %llu, cube %llu)", d.depth,
                    d.isArray ? 1u : 0u, d.isCube ? 1u : 0u);
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

Result<CookedTexture> cook_png(TextureSource const& src, TextureCookSettings const& settings,
                               TargetProfile const& target, u32 cap, Allocator const* alloc,
                               DiagSink const* diag, JobBudget const& budget, StrView asset,
                               u64 sourceHash) noexcept {
    detail::Stopwatch const swTotal;
    detail::Stopwatch const swDecode;
    KILN_TRY_ASSIGN(Image decoded, decode_png(src.bytes, alloc, diag, asset));
    u64 const decodeUs = swDecode.elapsed_us();

    TextureUsage const usage = settings.usage == TextureUsage::Auto ? TextureUsage::Color : settings.usage;
    ColorSpace const cs =
        settings.colorSpace == ColorSpace::Auto ? color_space_for(usage) : settings.colorSpace;
    Plan plan = plan_for(decoded, usage, cs, diag, asset);

    detail::Stopwatch const swPrepare;
    Result<Image> converted =
        prepare_image(decoded, plan.channels, plan.bits,
                      PrepareOptions{.grayAlpha   = plan.rgba8 && decoded.channels == 2,
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

    // Size cap: drop leading levels until the top fits.
    u32 const srcW = img.width, srcH = img.height;
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
    KILN_TRY_ASSIGN(Vec<Image> chain, build_mip_chain(std::move(img), plan.mips, buildCount, alloc, budget));
    u64 const mipsUs     = swMips.elapsed_us();
    u32 const levelCount = u32(chain.size()) - drop;
    KILN_VERIFY(levelCount >= 1 && levelCount <= ktx2::kMaxLevels);
    KILN_VERIFY(chain[drop].width == topW && chain[drop].height == topH);

    if (levelCount > 1 && (!is_pow2(topW) || !is_pow2(topH)))
        note(diag, asset, Severity::Info, kDiagImageNpotMips,
             "non-power-of-two %llux%llu with mips: levels use floor halving", topW, topH);

    Span<u8 const> levels[ktx2::kMaxLevels];
    for (u32 i = 0; i < levelCount; ++i)
        levels[i] = chain[drop + i].pixels.span();

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
        .levels             = {levels, levelCount},
        .writerTag          = "kiln-cook",
        .premultipliedAlpha = false,
        .extraKeys          = extra,
    };
    detail::Stopwatch const swWrite;
    KILN_TRY_ASSIGN(Vec<u8> file, ktx2::write(wd, alloc, diag));
    u64 const writeUs = swWrite.elapsed_us();

    CookedTexture out;
    out.file = std::move(file);
    out.desc = ktx2::TextureDesc{.format = plan.format, .width = topW, .height = topH, .levels = levelCount};
    out.passthrough     = false;
    out.sourceHash      = sourceHash;
    out.stats.decodeUs  = decodeUs;
    out.stats.prepareUs = prepareUs;
    out.stats.mipsUs    = mipsUs;
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

    if (is_ktx2(src.bytes)) return pass_through(src, cap, alloc, diag, asset, sourceHash);
    if (is_png(src.bytes))
        return cook_png(src, settings, target, cap, alloc, diag, JobBudget{env.jobs, env.maxThreads}, asset,
                        sourceHash);
    return fail(diag, asset, make_status(Code::Unsupported), kDiagImageUnknownFormat,
                "source is neither PNG nor KTX2 (%llu bytes)", src.bytes.size);
}

} // namespace kiln::cook
