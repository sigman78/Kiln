// src/cook/image.cpp — image pipeline. Integer-exact except normal renormalization,
// which uses IEEE double and std::sqrt (correctly rounded, so identical everywhere), and
// the f32 (HDR) path, which uses only correctly rounded f32 add, multiply and divide.
// The per-texel work lives in kernels.h; this file validates, allocates and dispatches.
#include "kiln/cook/image.h"

// Keep a*b+c from being fused into an FMA: fused and unfused results can differ in
// the last bit, which would break cross-platform byte-identical output. gcc in
// ISO mode (-std=c++NN) already defaults to -ffp-contract=off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#include "kernels.h"
#include "parallel.h"

#include <cstring>

namespace kiln::cook {

namespace {

bool valid_image(Image const& img) noexcept {
    return img.width > 0 && img.height > 0 && img.channels >= 1 && img.channels <= 4 &&
           (img.bitsPerChannel == 8 || img.bitsPerChannel == 16 || img.bitsPerChannel == 32) &&
           img.pixels.size() == img.byte_size();
}

Image make_image(u32 w, u32 h, u32 channels, u32 bits, Allocator const* alloc) noexcept {
    Image img;
    img.width          = w;
    img.height         = h;
    img.channels       = channels;
    img.bitsPerChannel = bits;
    img.pixels.init(alloc ? alloc : default_allocator(), Tag::Cook);
    img.pixels.resize(usize(img.byte_size()));
    return img;
}

/// Rows of `img` for an in-place kernel.
kernels::RowCtx in_place_rows(Image& img) noexcept {
    return kernels::RowCtx{
        .dst = img.pixels.data(), .dstRowBytes = usize(img.row_bytes()), .width = img.width};
}

/// Rows per parallel_for chunk: about 512 KiB of source bytes, and no more than
/// kMaxChunks chunks however large the image.
u32 row_grain(u32 rows, usize srcBytesPerRow) noexcept {
    constexpr usize kChunkBytes = usize(512) << 10;
    constexpr u32 kMaxChunks    = 256;
    u32 const byBytes           = u32(max(kChunkBytes / max(srcBytesPerRow, usize(1)), usize(1)));
    u32 const byCount           = rows / kMaxChunks + (rows % kMaxChunks != 0 ? 1u : 0u);
    return max(byBytes, byCount);
}

struct RowBands {
    kernels::RowFn fn;
    kernels::RowCtx ctx;

    static void run(void* user, u32 begin, u32 end) noexcept {
        auto const* self = static_cast<RowBands const*>(user);
        self->fn(self->ctx, begin, end);
    }
};

void in_place_bands(Image& img, kernels::RowFn fn, JobBudget const& budget) noexcept {
    RowBands bands = {.fn = fn, .ctx = in_place_rows(img)};
    parallel_for(budget.jobs, img.pixels.allocator(), img.height,
                 row_grain(img.height, bands.ctx.dstRowBytes), &RowBands::run, &bands, budget.maxThreads);
}

void prepare_band(void* user, u32 begin, u32 end) noexcept {
    kernels::prepare_rows(*static_cast<kernels::PrepareCtx const*>(user), begin, end);
}

struct DownsampleBands {
    kernels::DownsampleFn fn;
    kernels::DownsampleCtx ctx;

    static void run(void* user, u32 begin, u32 end) noexcept {
        auto const* self = static_cast<DownsampleBands const*>(user);
        self->fn(self->ctx, begin, end);
    }
};

// --- f32 (HDR) path: plain loops, kept apart from the integer kernels ---

/// Channel `i` of a row as f32: integers become v / max, no sRGB decode.
f32 load_f32(u8 const* row, usize i, u32 bits) noexcept {
    if (bits == 8) return f32(row[i]) / 255.0f;
    if (bits == 16) {
        u16 v;
        std::memcpy(&v, row + i * 2, 2);
        return f32(v) / 65535.0f;
    }
    f32 v;
    std::memcpy(&v, row + i * 4, 4);
    return v;
}

struct FloatPrepare {
    Image const* src = nullptr;
    Image* dst       = nullptr;
    bool grayAlpha   = false;

    /// Same channel rules as the integer converter; alpha added as 1.0.
    static void run(void* user, u32 begin, u32 end) noexcept {
        auto const& k = *static_cast<FloatPrepare const*>(user);
        u32 const sc = k.src->channels, dc = k.dst->channels, bits = k.src->bitsPerChannel;
        for (u32 y = begin; y < end; ++y) {
            u8 const* sr = k.src->pixels.data() + usize(y) * usize(k.src->row_bytes());
            u8* dr       = k.dst->pixels.data() + usize(y) * usize(k.dst->row_bytes());
            for (u32 x = 0; x < k.src->width; ++x) {
                f32 in[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (u32 c = 0; c < sc; ++c)
                    in[c] = load_f32(sr, usize(x) * sc + c, bits);
                f32 out[4] = {in[0], in[1], in[2], in[3]};
                if (sc == 1) out[1] = out[2] = in[0];
                if (sc == 2) {
                    out[1] = k.grayAlpha ? in[0] : in[1];
                    out[2] = k.grayAlpha ? in[0] : 0.0f;
                    out[3] = k.grayAlpha ? in[1] : 1.0f;
                }
                if (dc <= sc)
                    for (u32 c = 0; c < dc; ++c)
                        out[c] = in[c];
                std::memcpy(dr + usize(x) * dc * 4, out, usize(dc) * 4);
            }
        }
    }
};

struct FloatDownsample {
    Image const* src = nullptr;
    Image* dst       = nullptr;
    usize colStep = 0, rowStep = 0;

    static void run(void* user, u32 begin, u32 end) noexcept {
        auto const& k     = *static_cast<FloatDownsample const*>(user);
        u32 const ch      = k.src->channels;
        usize const srow  = usize(k.src->row_bytes());
        usize const drow  = usize(k.dst->row_bytes());
        usize const pixel = usize(ch) * 4;
        for (u32 y = begin; y < end; ++y) {
            for (u32 x = 0; x < k.dst->width; ++x) {
                u8 const* p = k.src->pixels.data() + usize(2 * y) * srow + usize(2 * x) * pixel;
                for (u32 c = 0; c < ch; ++c) {
                    f32 a, b, d, e;
                    std::memcpy(&a, p + c * 4, 4);
                    std::memcpy(&b, p + k.colStep + c * 4, 4);
                    std::memcpy(&d, p + k.rowStep + c * 4, 4);
                    std::memcpy(&e, p + k.rowStep + k.colStep + c * 4, 4);
                    f32 const v = ((a + b) + (d + e)) * 0.25f;
                    std::memcpy(k.dst->pixels.data() + usize(y) * drow + usize(x) * pixel + c * 4, &v, 4);
                }
            }
        }
    }
};

} // namespace

u16 srgb8_to_linear16(u8 v) noexcept { return kernels::kSrgbToLinear16[v]; }

u8 linear16_to_srgb8(u16 v) noexcept { return kernels::kLinear16ToSrgb8.v[v]; }

Result<Image> prepare_image(Image const& src, u32 channels, u32 bitsPerChannel, PrepareOptions const& opt,
                            Allocator const* alloc, JobBudget const& budget) noexcept {
    if (!valid_image(src) || channels < 1 || channels > 4 ||
        (bitsPerChannel != 8 && bitsPerChannel != 16 && bitsPerChannel != 32))
        return make_status(Code::InvalidArgument);
    if (bitsPerChannel == 32 || src.bitsPerChannel == 32) {
        if (bitsPerChannel != 32 || opt.flipGreen || opt.renormalize)
            return make_status(Code::InvalidArgument);
        Image dst = make_image(src.width, src.height, channels, 32, alloc);
        FloatPrepare k{.src = &src, .dst = &dst, .grayAlpha = opt.grayAlpha};
        parallel_for(budget.jobs, alloc, src.height, row_grain(src.height, usize(dst.row_bytes())),
                     &FloatPrepare::run, &k, budget.maxThreads);
        return dst;
    }

    Image dst               = make_image(src.width, src.height, channels, bitsPerChannel, alloc);
    kernels::PrepareCtx ctx = {
        .rows =
            {
                   .src         = src.pixels.data(),
                   .dst         = dst.pixels.data(),
                   .srcRowBytes = usize(src.row_bytes()),
                   .dstRowBytes = usize(dst.row_bytes()),
                   .width       = src.width,
                   },
        .convert     = kernels::convert_kernel(src.bitsPerChannel, src.channels, bitsPerChannel, channels,
                                               opt.grayAlpha),
        .flipGreen   = opt.flipGreen ? kernels::flip_green_kernel(bitsPerChannel, channels) : nullptr,
        .renormalize = opt.renormalize ? kernels::renormalize_kernel(bitsPerChannel, channels) : nullptr,
    };
    KILN_VERIFY(ctx.convert != nullptr);
    parallel_for(budget.jobs, alloc, src.height, row_grain(src.height, ctx.rows.srcRowBytes), &prepare_band,
                 &ctx, budget.maxThreads);
    return dst;
}

Result<Image> convert_image(Image const& src, u32 channels, u32 bitsPerChannel, Allocator const* alloc,
                            JobBudget const& budget) noexcept {
    return prepare_image(src, channels, bitsPerChannel, PrepareOptions{}, alloc, budget);
}

Result<Image> downsample_2x(Image const& src, MipOptions const& opt, Allocator const* alloc,
                            JobBudget const& budget) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const w = max(src.width / 2, 1u);
    u32 const h = max(src.height / 2, 1u);
    Image dst   = make_image(w, h, src.channels, src.bitsPerChannel, alloc);
    if (src.bitsPerChannel == 32) { // linear by definition: srgb and renormalize do not apply
        usize const srcRow = usize(src.row_bytes());
        FloatDownsample k{.src     = &src,
                          .dst     = &dst,
                          .colStep = src.width > 1 ? usize(src.bytes_per_pixel()) : 0,
                          .rowStep = src.height > 1 ? srcRow : 0};
        parallel_for(budget.jobs, alloc, h, row_grain(h, 2 * srcRow), &FloatDownsample::run, &k,
                     budget.maxThreads);
        return dst;
    }
    // sRGB averaging exists only for 8-bit data (there is no 16-bit sRGB format) and
    // covers the color channels, never alpha (channel 3). Normals are linear.
    bool const renorm                  = opt.renormalize && src.channels >= 3;
    bool const srgb                    = opt.srgb && src.bitsPerChannel == 8 && !renorm;
    kernels::DownsampleMode const mode = renorm ? kernels::DownsampleMode::Renorm
                                         : srgb ? kernels::DownsampleMode::Srgb
                                                : kernels::DownsampleMode::Linear;
    kernels::DownsampleFn const fn     = kernels::downsample_kernel(src.bitsPerChannel, src.channels, mode);
    KILN_VERIFY(fn != nullptr);
    usize const srcRow    = usize(src.row_bytes());
    DownsampleBands bands = {
        .fn = fn,
        .ctx =
            {
                  .src         = src.pixels.data(),
                  .dst         = dst.pixels.data(),
                  .srcRowBytes = srcRow,
                  .dstRowBytes = usize(dst.row_bytes()),
                  .dstWidth    = w,
                  .colStep     = src.width > 1 ? usize(src.bytes_per_pixel()) : 0,
                  .rowStep     = src.height > 1 ? srcRow : 0,
                  },
    };
    // An output row reads two source rows.
    parallel_for(budget.jobs, alloc, h, row_grain(h, 2 * srcRow), &DownsampleBands::run, &bands,
                 budget.maxThreads);
    return dst;
}

void flip_green(Image& img, JobBudget const& budget) noexcept {
    if (!valid_image(img)) return;
    kernels::RowFn const fn = kernels::flip_green_kernel(img.bitsPerChannel, img.channels);
    if (fn) in_place_bands(img, fn, budget);
}

void renormalize(Image& img, JobBudget const& budget) noexcept {
    if (!valid_image(img)) return;
    kernels::RowFn const fn = kernels::renormalize_kernel(img.bitsPerChannel, img.channels);
    if (fn) in_place_bands(img, fn, budget);
}

namespace {

u32 alpha_at(Image const& img, usize i) noexcept {
    if (img.bitsPerChannel == 8) return img.pixels[i * 4 + 3];
    u16 v;
    std::memcpy(&v, img.pixels.data() + (i * 4 + 3) * 2, 2);
    return v;
}

void set_alpha(Image& img, usize i, u32 v) noexcept {
    if (img.bitsPerChannel == 8) {
        img.pixels[i * 4 + 3] = u8(v);
        return;
    }
    u16 const w = u16(v);
    std::memcpy(img.pixels.data() + (i * 4 + 3) * 2, &w, 2);
}

} // namespace

void preserve_alpha_coverage(Span<Image> chain, f32 cutoff, Allocator const* alloc) noexcept {
    if (chain.size < 2 || !(cutoff > 0.0f && cutoff <= 1.0f)) return;
    Image const& top = chain[0];
    if (top.channels != 4 || (top.bitsPerChannel != 8 && top.bitsPerChannel != 16)) return;
    u32 const maxv = top.bitsPerChannel == 8 ? 255u : 65535u;
    u32 const ref  = max(1u, u32(cutoff * f32(maxv) + 0.5f)); // passes: alpha >= ref
    usize const n0 = usize(top.width) * top.height;
    u64 pass0      = 0;
    for (usize i = 0; i < n0; ++i)
        pass0 += alpha_at(top, i) >= ref;

    Vec<u32> hist(alloc ? alloc : default_allocator(), Tag::Cook);
    for (usize l = 1; l < chain.size; ++l) {
        Image& img    = chain[l];
        usize const n = usize(img.width) * img.height;
        hist.clear();
        hist.resize(usize(maxv) + 1, 0u);
        for (usize i = 0; i < n; ++i)
            ++hist[alpha_at(img, i)];
        // The threshold t whose pass count is closest to pass0 * n / n0 (compared without division:
        // |count * n0 - pass0 * n|); on a tie the higher t, which changes alpha least.
        u64 const want = pass0 * n;
        u64 count = 0, bestErr = ~u64(0);
        u32 best = ref;
        for (u32 t = maxv; t >= 1; --t) {
            count += hist[t];
            u64 const have = count * n0;
            u64 const err  = have > want ? have - want : want - have;
            if (err < bestErr) {
                bestErr = err;
                best    = t;
            }
        }
        if (best == ref) continue;
        // Scale so `best` lands on `ref`: texels at or above it pass, the rest do not.
        for (usize i = 0; i < n; ++i) {
            u64 const a = (u64(alpha_at(img, i)) * ref + best / 2) / best;
            set_alpha(img, i, u32(min<u64>(a, maxv)));
        }
    }
}

Result<Vec<Image>> build_mip_chain(Image&& src, MipOptions const& opt, u32 maxLevels, Allocator const* alloc,
                                   JobBudget const& budget) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const full  = u32(std::bit_width(max(src.width, src.height)));
    u32 const count = maxLevels == 0 ? full : min(maxLevels, full);
    Vec<Image> chain(alloc ? alloc : default_allocator(), Tag::Cook);
    chain.reserve(count);
    chain.push_back(std::move(src));
    for (u32 i = 1; i < count; ++i) {
        Result<Image> next = downsample_2x(chain[i - 1], opt, alloc, i == 1 ? budget : JobBudget{});
        if (next.failed()) return next.status();
        chain.push_back(std::move(next).value());
    }
    return chain;
}

} // namespace kiln::cook
