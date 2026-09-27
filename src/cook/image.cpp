// src/cook/image.cpp — image pipeline. Integer-exact except normal renormalization,
// which uses IEEE double and std::sqrt (correctly rounded, so identical everywhere).
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

namespace kiln::cook {

namespace {

bool valid_image(Image const& img) noexcept {
    return img.width > 0 && img.height > 0 && img.channels >= 1 && img.channels <= 4 &&
           (img.bitsPerChannel == 8 || img.bitsPerChannel == 16) && img.pixels.size() == img.byte_size();
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

} // namespace

u16 srgb8_to_linear16(u8 v) noexcept { return kernels::kSrgbToLinear16[v]; }

u8 linear16_to_srgb8(u16 v) noexcept { return kernels::kLinear16ToSrgb8.v[v]; }

Result<Image> prepare_image(Image const& src, u32 channels, u32 bitsPerChannel, PrepareOptions const& opt,
                            Allocator const* alloc) noexcept {
    if (!valid_image(src) || channels < 1 || channels > 4 || (bitsPerChannel != 8 && bitsPerChannel != 16))
        return make_status(Code::InvalidArgument);

    Image dst                     = make_image(src.width, src.height, channels, bitsPerChannel, alloc);
    kernels::PrepareCtx const ctx = {
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
    kernels::prepare_rows(ctx, 0, src.height);
    return dst;
}

Result<Image> convert_image(Image const& src, u32 channels, u32 bitsPerChannel,
                            Allocator const* alloc) noexcept {
    return prepare_image(src, channels, bitsPerChannel, PrepareOptions{}, alloc);
}

Result<Image> downsample_2x(Image const& src, MipOptions const& opt, Allocator const* alloc) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const w = max(src.width / 2, 1u);
    u32 const h = max(src.height / 2, 1u);
    Image dst   = make_image(w, h, src.channels, src.bitsPerChannel, alloc);
    // sRGB averaging exists only for 8-bit data (there is no 16-bit sRGB format) and
    // covers the color channels, never alpha (channel 3). Normals are linear.
    bool const renorm                  = opt.renormalize && src.channels >= 3;
    bool const srgb                    = opt.srgb && src.bitsPerChannel == 8 && !renorm;
    kernels::DownsampleMode const mode = renorm ? kernels::DownsampleMode::Renorm
                                         : srgb ? kernels::DownsampleMode::Srgb
                                                : kernels::DownsampleMode::Linear;
    kernels::DownsampleFn const fn     = kernels::downsample_kernel(src.bitsPerChannel, src.channels, mode);
    KILN_VERIFY(fn != nullptr);
    usize const srcRow               = usize(src.row_bytes());
    kernels::DownsampleCtx const ctx = {
        .src         = src.pixels.data(),
        .dst         = dst.pixels.data(),
        .srcRowBytes = srcRow,
        .dstRowBytes = usize(dst.row_bytes()),
        .dstWidth    = w,
        .colStep     = src.width > 1 ? usize(src.bytes_per_pixel()) : 0,
        .rowStep     = src.height > 1 ? srcRow : 0,
    };
    fn(ctx, 0, h);
    return dst;
}

void flip_green(Image& img) noexcept {
    if (!valid_image(img)) return;
    kernels::RowFn const fn = kernels::flip_green_kernel(img.bitsPerChannel, img.channels);
    if (fn) fn(in_place_rows(img), 0, img.height);
}

void renormalize(Image& img) noexcept {
    if (!valid_image(img)) return;
    kernels::RowFn const fn = kernels::renormalize_kernel(img.bitsPerChannel, img.channels);
    if (fn) fn(in_place_rows(img), 0, img.height);
}

Result<Vec<Image>> build_mip_chain(Image&& src, MipOptions const& opt, u32 maxLevels,
                                   Allocator const* alloc) noexcept {
    if (!valid_image(src)) return make_status(Code::InvalidArgument);
    u32 const full  = u32(std::bit_width(max(src.width, src.height)));
    u32 const count = maxLevels == 0 ? full : min(maxLevels, full);
    Vec<Image> chain(alloc ? alloc : default_allocator(), Tag::Cook);
    chain.reserve(count);
    chain.push_back(std::move(src));
    for (u32 i = 1; i < count; ++i) {
        Result<Image> next = downsample_2x(chain[i - 1], opt, alloc);
        if (next.failed()) return next.status();
        chain.push_back(std::move(next).value());
    }
    return chain;
}

} // namespace kiln::cook
