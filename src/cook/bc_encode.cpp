// src/cook/bc_encode.cpp — BC1/3/4/5 with rgbcx and BC7 with bc7enc (third_party/bc7enc_rdo).
// Design: docs/design/bcn-encoding.md.
#include "bc_encode.h"

#include "parallel.h"

#include "kiln/log.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "bc7enc.h"
#include "rgbcx.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace kiln::cook {

namespace {

enum class Codec : u8 { BC1, BC3, BC4, BC5, BC7 };

/// The encoders' global tables, filled once. rgbcx::init must not run while another thread encodes.
struct Encoders {
    bc7enc_compress_block_params bc7[3]; ///< by EncodeQuality

    Encoders() noexcept {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
        u32 const uber[3] = {0, 2, 4};
        for (u32 q = 0; q < 3; ++q) {
            bc7enc_compress_block_params_init(&bc7[q]);
            bc7enc_compress_block_params_init_linear_weights(&bc7[q]);
            bc7[q].m_uber_level     = uber[q];
            bc7[q].m_max_partitions = BC7ENC_MAX_PARTITIONS;
        }
    }
};

Encoders const& encoders() noexcept {
    static Encoders const e;
    return e;
}

Codec codec_of(Format f) noexcept {
    switch (f) {
    case Format::BC1_RGB_UNORM:
    case Format::BC1_RGB_SRGB: return Codec::BC1;
    case Format::BC3_UNORM:
    case Format::BC3_SRGB: return Codec::BC3;
    case Format::BC4_UNORM: return Codec::BC4;
    case Format::BC5_UNORM: return Codec::BC5;
    case Format::BC7_UNORM:
    case Format::BC7_SRGB: return Codec::BC7;
    default: KILN_PANIC("bc_encode: format %u is not a supported BC format", u32(f));
    }
}

/// rgbcx's BC1 and BC3 levels (0-18) per quality; the spike's measured points.
constexpr u32 kRgbcxLevel[3] = {0, 10, 18};

struct Job {
    Image const* img;
    u8* out;
    Encoders const* enc;
    Codec codec;
    u32 quality;
    u32 blockBytes;
    u32 blocksX;
};

/// The 16 texels of block (bx, by) as RGBA8, edges repeated. Missing channels are 0, alpha 255.
void gather(Image const& img, u32 bx, u32 by, u8 px[64]) noexcept {
    u32 const c = img.channels;
    for (u32 y = 0; y < 4; ++y) {
        u32 const sy  = min(by * 4 + y, img.height - 1);
        u8 const* row = img.pixels.data() + usize(sy) * img.width * c;
        for (u32 x = 0; x < 4; ++x) {
            u8 const* p = row + usize(min(bx * 4 + x, img.width - 1)) * c;
            u8* d       = px + (y * 4 + x) * 4;
            d[0]        = p[0];
            d[1]        = c > 1 ? p[1] : 0;
            d[2]        = c > 2 ? p[2] : 0;
            d[3]        = c > 3 ? p[3] : 255;
        }
    }
}

void encode_rows(void* user, u32 begin, u32 end) noexcept {
    Job const& j = *static_cast<Job const*>(user);
    u8 px[64];
    for (u32 by = begin; by < end; ++by)
        for (u32 bx = 0; bx < j.blocksX; ++bx) {
            gather(*j.img, bx, by, px);
            u8* const dst = j.out + (usize(by) * j.blocksX + bx) * j.blockBytes;
            bool const hq = j.quality != u32(EncodeQuality::Fast);
            switch (j.codec) {
            case Codec::BC1: rgbcx::encode_bc1(kRgbcxLevel[j.quality], dst, px, true, false); break;
            case Codec::BC3:
                if (j.quality == u32(EncodeQuality::High))
                    rgbcx::encode_bc3_hq(kRgbcxLevel[j.quality], dst, px);
                else
                    rgbcx::encode_bc3(kRgbcxLevel[j.quality], dst, px);
                break;
            case Codec::BC4:
                if (hq)
                    rgbcx::encode_bc4_hq(dst, px, 4);
                else
                    rgbcx::encode_bc4(dst, px, 4);
                break;
            case Codec::BC5:
                if (hq)
                    rgbcx::encode_bc5_hq(dst, px, 0, 1, 4);
                else
                    rgbcx::encode_bc5(dst, px, 0, 1, 4);
                break;
            case Codec::BC7: bc7enc_compress_block(dst, px, &j.enc->bc7[j.quality]); break;
            }
        }
}

} // namespace

void bc_encode(Image const& img, Format format, EncodeQuality quality, Vec<u8>& out,
               JobBudget const& budget) noexcept {
    KILN_VERIFY(img.bitsPerChannel == 8 && img.width > 0 && img.height > 0);
    Codec const codec = codec_of(format);
    KILN_VERIFY(img.channels == 4 || (img.channels <= 2 && (codec == Codec::BC4 || codec == Codec::BC5)));
    u32 const blocksX = (img.width + 3) / 4, blocksY = (img.height + 3) / 4;
    u32 const bytes = codec == Codec::BC1 || codec == Codec::BC4 ? 8u : 16u;
    usize const at  = out.size();
    out.resize(at + usize(blocksX) * blocksY * bytes);
    Job job{.img        = &img,
            .out        = out.data() + at,
            .enc        = &encoders(),
            .codec      = codec,
            .quality    = u32(quality),
            .blockBytes = bytes,
            .blocksX    = blocksX};
    // About 1024 blocks per chunk: enough work per task for the slowest (BC7) and fastest (BC4) codecs.
    parallel_for(budget.jobs, out.allocator(), blocksY, max(1u, 1024u / blocksX), &encode_rows, &job,
                 budget.maxThreads);
}

} // namespace kiln::cook
