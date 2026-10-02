// KTX2 writer for uncompressed and BC block-compressed textures, optionally Zstd-supercompressed.
#include "kiln/cook/ktx2_writer.h"
#include "kiln/log.h"

#include "formats_internal.h"

#include <algorithm>

// The custom-allocator API is stable only within one zstd version; the vendored one is pinned.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace kiln::ktx2 {

namespace {

/// Round up to a multiple of `a` (not necessarily a power of two, e.g. 12).
constexpr u64 round_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

// KHR_DF channel ids and qualifiers for the RGBSDA color model.
constexpr u32 kChannelIds[4] = {0, 1, 2, 15}; // R, G, B, A
// Sample qualifier bits (KHR_DF_SAMPLE_DATATYPE_*): bits 4..7 of the channelType byte.
constexpr u32 kQualifierLinear    = 0x10; // 0x20 is EXPONENT; libktx's validator checks this
constexpr u32 kQualifierSigned    = 0x40;
constexpr u32 kQualifierFloat     = 0x80;
constexpr u8 kColorModelRgbsda    = 1;
constexpr u8 kColorPrimariesBt709 = 1;

constexpr char kWriterKey[] = "KTXwriter"; // written with its NUL

/// One sample of a BC block: a channel id of the block's color model and a 64- or 128-bit range.
struct BlockSample {
    u8 channel   = 0;
    u16 bitStart = 0;
    u8 bitLength = 0; ///< minus 1, as stored
};
/// A BC format's KHR_DF color model (BC1A 128 ... BC7 134) and its samples in memory order, as
/// `ktx create` writes them (tests/corpus/ktx2/generated/bc*.ktx2).
struct BlockModel {
    u8 model   = 0;
    u8 samples = 0;
    BlockSample s[2];
};

BlockModel block_model(Format f) {
    constexpr u8 kAlpha = 15;
    switch (f) {
    case Format::BC1_RGB_UNORM:
    case Format::BC1_RGB_SRGB: return {128, 1, {{0, 0, 63}}}; // BC1A color
    case Format::BC1_RGBA_UNORM:
    case Format::BC1_RGBA_SRGB: return {128, 1, {{1, 0, 63}}}; // BC1A color with 1-bit alpha
    case Format::BC2_UNORM:
    case Format::BC2_SRGB:
        return {
            129, 2, {{kAlpha, 0, 63}, {0, 64, 63}}
        };
    case Format::BC3_UNORM:
    case Format::BC3_SRGB:
        return {
            130, 2, {{kAlpha, 0, 63}, {0, 64, 63}}
        };
    case Format::BC4_UNORM:
    case Format::BC4_SNORM: return {131, 1, {{0, 0, 63}}};
    case Format::BC5_UNORM:
    case Format::BC5_SNORM:
        return {
            132, 2, {{0, 0, 63}, {1, 64, 63}}
        };
    case Format::BC6H_UFLOAT:
    case Format::BC6H_SFLOAT: return {133, 1, {{0, 0, 127}}};
    case Format::BC7_UNORM:
    case Format::BC7_SRGB: return {134, 1, {{0, 0, 127}}};
    default: return {};
    }
}

u32 dfd_samples(FormatInfo const& info) {
    return info.compressed ? block_model(info.format).samples : info.channels;
}

u32 dfd_size(FormatInfo const& info) {
    return 4 + kDfdBasicBlockHeaderSize + kDfdSampleSize * dfd_samples(info);
}

/// Sample bounds and qualifiers by component kind (KDF 1.3, 5.19 / 5.20).
void sample_range(FormatInfo const& info, u32* lower, u32* upper, u32* qualifiers) {
    u32 const bits = info.compressed ? 32u : info.bitsPerChannel; // block samples span the full u32
    *lower = 0, *upper = 0, *qualifiers = 0;
    switch (info.kind) {
    case FormatKind::UNorm: *upper = u32((u64(1) << bits) - 1); break;
    case FormatKind::SNorm:
        *upper      = u32((u64(1) << (bits - 1)) - 1);
        *lower      = info.compressed ? 0x80000000u : ~*upper + 1u; // blocks: INT32_MIN, as libktx writes
        *qualifiers = kQualifierSigned;
        break;
    case FormatKind::SFloat:
        *lower      = 0xBF800000u; // -1.0f
        *upper      = 0x3F800000u; //  1.0f
        *qualifiers = kQualifierSigned | kQualifierFloat;
        break;
    case FormatKind::UFloat:
        *upper      = 0x3F800000u;
        *qualifiers = kQualifierFloat;
        break;
    }
}

/// Write the DFD for `info` at `out` (dfd_size(info) bytes, zeroed).
void write_dfd(u8* out, FormatInfo const& info, bool premultiplied) {
    u32 const total     = dfd_size(info);
    u32 const blockSize = total - 4;
    write_unaligned<u32>(out + 0, total);
    write_unaligned<u32>(out + 4, 0u);                     // vendorId 0 (Khronos) | descriptorType 0 (basic)
    write_unaligned<u32>(out + 8, 2u | (blockSize << 16)); // versionNumber 2 | descriptorBlockSize
    out[12]   = kColorModelRgbsda;
    out[13]   = kColorPrimariesBt709;
    out[14]   = info.srgb ? kDfdTransferSrgb : kDfdTransferLinear;
    out[15]   = premultiplied ? kDfdFlagAlphaPremultiplied : u8(0);
    out[20]   = info.bytesPerBlock; // bytesPlane0; planes 1..7 stay 0
    u32 lower = 0, upper = 0, qualifiers = 0;
    sample_range(info, &lower, &upper, &qualifiers);
    u8* sample = out + 4 + kDfdBasicBlockHeaderSize;

    if (info.compressed) {
        BlockModel const m = block_model(info.format);
        out[12]            = m.model;
        out[16]            = u8(info.blockWidth - 1); // texelBlockDimension0..1; 2..3 stay 0
        out[17]            = u8(info.blockHeight - 1);
        for (u32 i = 0; i < m.samples; ++i, sample += kDfdSampleSize) {
            BlockSample const& b = m.s[i];
            u32 channelType      = b.channel | qualifiers;
            if (b.channel == 15 && info.srgb) channelType |= kQualifierLinear; // BC2/BC3 alpha
            write_unaligned<u32>(sample + 0,
                                 u32(b.bitStart) | (u32(b.bitLength) << 16) | (channelType << 24));
            write_unaligned<u32>(sample + 8, lower);
            write_unaligned<u32>(sample + 12, upper);
        }
        return;
    }

    // out[16..19] texelBlockDimension: 0 == 1 texel in each dimension.
    u32 const bits = info.bitsPerChannel;
    for (u32 c = 0; c < info.channels; ++c, sample += kDfdSampleSize) {
        // Channels in memory order R, G, B, A; one- to three-channel formats have no alpha.
        u32 const id     = kChannelIds[c];
        bool const alpha = id == 15;
        u32 channelType  = id | qualifiers;
        if (alpha && info.srgb) channelType |= kQualifierLinear;
        write_unaligned<u32>(sample + 0, (c * bits) | ((bits - 1) << 16) | (channelType << 24));
        // sample + 4: samplePosition[4] = 0
        write_unaligned<u32>(sample + 8, lower);
        write_unaligned<u32>(sample + 12, upper);
    }
}

Status invalid(DiagSink const* diag, u32 code, char const* fmt, unsigned long long a = 0,
               unsigned long long b = 0, unsigned long long c = 0) {
    return diagf(diag, make_status(Code::InvalidArgument), code, Severity::Error, {}, "ktx2 write", fmt, a, b,
                 c);
}

} // namespace

Result<Vec<u8>> write(WriteDesc const& desc, Allocator const* alloc, DiagSink const* diag) {
    // --- Validate --------------------------------------------------------------------
    FormatInfo const* info = format_info(desc.format);
    if (desc.format == Format::Undefined || info == nullptr)
        return invalid(diag, kDiagKtxFormat, "unknown format %llu", u32(desc.format));
    if (info->compressed && block_model(desc.format).samples == 0)
        return invalid(diag, kDiagKtxFormat,
                       "block-compressed format %llu is not supported by the writer (BC1-BC7 only)",
                       u32(desc.format));
    if (desc.width == 0 || desc.height == 0)
        return invalid(diag, kDiagKtxDimensions, "extent %llux%llu must be at least 1x1", desc.width,
                       desc.height);
    if (desc.faces != 1 && desc.faces != 6)
        return invalid(diag, kDiagKtxDimensions, "%llu faces, must be 1 or 6", desc.faces);
    if (desc.faces == 6 && desc.width != desc.height)
        return invalid(diag, kDiagKtxDimensions, "cube faces must be square, not %llux%llu", desc.width,
                       desc.height);
    if (desc.layers == 0 || (!desc.isArray && desc.layers != 1))
        return invalid(diag, kDiagKtxDimensions, "%llu layers given; more than 1 needs isArray", desc.layers);
    u32 const maxLevels = u32(std::bit_width(max(desc.width, desc.height)));
    if (desc.levels.size == 0 || desc.levels.size > maxLevels)
        return invalid(diag, kDiagKtxDimensions, "%llu levels given, must be 1..%llu", desc.levels.size,
                       maxLevels);
    if (desc.writerTag.find('\0') != StrView::kNpos)
        return invalid(diag, kDiagKtxKvd, "writerTag contains a NUL byte");
    if (desc.extraKeys.size > 15) return invalid(diag, kDiagKtxKvd, "too many extra key/value entries");
    for (usize i = 0; i < desc.extraKeys.size; ++i) {
        KeyValue const& kvp = desc.extraKeys[i];
        if (kvp.key.empty() || kvp.key.find('\0') != StrView::kNpos || kvp.key == StrView(kWriterKey))
            return invalid(diag, kDiagKtxKvd, "extra key %llu is empty, contains NUL or is KTXwriter",
                           u64(i));
        for (usize j = 0; j < i; ++j)
            if (desc.extraKeys[j].key == kvp.key)
                return invalid(diag, kDiagKtxKvd, "duplicate key/value key at index %llu", u64(i));
    }
    // KVD entries sorted by key bytes (KTX 2.0 3.11.1): KTXwriter plus extras.
    struct Entry {
        StrView key;
        StrView value;
    };
    Entry entries[16];
    u32 entryCount        = 0;
    entries[entryCount++] = {StrView(kWriterKey), desc.writerTag};
    for (usize i = 0; i < desc.extraKeys.size; ++i)
        entries[entryCount++] = {desc.extraKeys[i].key, desc.extraKeys[i].value};
    std::sort(entries, entries + entryCount,
              [](Entry const& a, Entry const& b) { return compare(a.key, b.key) < 0; });
    if (desc.zstdLevel > u32(ZSTD_maxCLevel()))
        return invalid(diag, kDiagKtxSupercompression, "zstdLevel %llu is above %llu", desc.zstdLevel,
                       u32(ZSTD_maxCLevel()));
    if (!(desc.zstdMinSaving >= 0.0f && desc.zstdMinSaving < 1.0f)) // also false for NaN
        return invalid(diag, kDiagKtxSupercompression, "zstdMinSaving must be in [0, 1)");
    u32 const levelCount = u32(desc.levels.size);
    for (u32 i = 0; i < levelCount; ++i) {
        u64 const expected =
            format_image_bytes(desc.format, max(desc.width >> i, 1u), max(desc.height >> i, 1u)) *
            desc.layers * desc.faces;
        if (desc.levels[i].size != expected)
            return invalid(diag, kDiagKtxLevelIndex, "level %llu has %llu bytes, expected %llu", i,
                           desc.levels[i].size, expected);
        if (expected != 0 && desc.levels[i].data == nullptr)
            return invalid(diag, kDiagKtxLevelIndex, "level %llu has no data", i);
    }

    // --- Supercompression ----------------------------------------------------------------
    // Fixed parameters, one thread: the same levels give the same frames on every machine.
    Allocator const* const a = alloc ? alloc : default_allocator();
    bool zstd                = desc.zstdLevel != 0;
    Span<u8 const> stored[kMaxLevels];
    Vec<u8> frames[kMaxLevels];
    for (u32 i = 0; i < levelCount; ++i)
        stored[i] = desc.levels[i];
    if (zstd) {
        fmt::ZstdMem mem{a, Tag::Cook};
        ZSTD_CCtx* cctx = ZSTD_createCCtx_advanced(ZSTD_customMem{&fmt::zstd_alloc, &fmt::zstd_free, &mem});
        if (!cctx)
            return diagf(diag, make_status(Code::OutOfMemory), kDiagKtxSupercompression, Severity::Error, {},
                         "ktx2 write", "out of memory for the Zstd context");
        (void)ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, int(desc.zstdLevel));
        (void)ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
        (void)ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
        usize failure = 0;
        for (u32 i = 0; i < levelCount && failure == 0; ++i) {
            frames[i].init(a, Tag::Cook);
            frames[i].resize(ZSTD_compressBound(desc.levels[i].size));
            usize const n = ZSTD_compress2(cctx, frames[i].data(), frames[i].size(), desc.levels[i].data,
                                           desc.levels[i].size);
            if (ZSTD_isError(n)) {
                failure = n;
                break;
            }
            frames[i].resize(n);
            stored[i] = frames[i].span();
        }
        ZSTD_freeCCtx(cctx);
        if (failure != 0)
            return diagf(diag, make_status(Code::Internal), kDiagKtxSupercompression, Severity::Error, {},
                         "ktx2 write", "Zstd compression failed: %s", ZSTD_getErrorName(failure));
    }

    // --- Layout ----------------------------------------------------------------------
    u32 const dfdOffset = u32(sizeof(Header) + levelCount * sizeof(LevelIndex));
    u32 const dfdLength = dfd_size(*info);
    u32 const kvdOffset = dfdOffset + dfdLength;
    u32 kvdLength       = 0;
    for (u32 i = 0; i < entryCount; ++i) {
        u32 const kvLength =
            u32(entries[i].key.size) + 1u + u32(entries[i].value.size) + 1u; // key NUL value NUL
        kvdLength += u32(align_up(u64(4) + kvLength, u64(4)));
    }

    // Writes the level index for Zstd frames or plain levels; returns the file size.
    auto const lay_out = [&](bool compressed, LevelIndex* index) {
        u64 const align = compressed ? 1u : fmt::ktx2_level_align(info->bytesPerBlock);
        u64 pos         = u64(kvdOffset) + kvdLength;
        for (u32 i = levelCount; i-- > 0;) { // smallest level first, level 0 last
            u64 const size                  = compressed ? stored[i].size : desc.levels[i].size;
            pos                             = round_up(pos, align);
            index[i].byteOffset             = pos;
            index[i].byteLength             = size;
            index[i].uncompressedByteLength = desc.levels[i].size;
            pos += size;
        }
        return pos;
    };
    LevelIndex index[kMaxLevels]{};
    u64 total = lay_out(zstd, index);
    if (zstd && desc.zstdMinSaving > 0.0f) {
        // Disk space is allocated in blocks, so a small file gains nothing: compare 4 KiB blocks.
        constexpr u64 kBlock  = 4096;
        u64 const plainBlocks = (lay_out(false, index) + kBlock - 1) / kBlock;
        u64 const zstdBlocks  = (total + kBlock - 1) / kBlock;
        if (f64(zstdBlocks) > f64(plainBlocks) * (1.0 - f64(desc.zstdMinSaving))) {
            zstd = false;
            for (u32 i = 0; i < levelCount; ++i)
                stored[i] = desc.levels[i];
        }
        total = lay_out(zstd, index);
    }

    Header h{};
    std::memcpy(h.identifier, kIdentifier, sizeof(kIdentifier));
    h.vkFormat    = u32(desc.format);
    h.typeSize    = info->compressed ? 1u : u32(info->bitsPerChannel / 8); // 1 for blocks (KTX 2.0 3.3)
    h.pixelWidth  = desc.width;
    h.pixelHeight = desc.height;
    h.pixelDepth  = 0;
    h.layerCount  = desc.isArray ? desc.layers : 0;
    h.faceCount   = desc.faces;
    h.levelCount  = levelCount;
    h.supercompressionScheme = u32(zstd ? Supercompression::Zstd : Supercompression::None);
    h.dfdByteOffset          = dfdOffset;
    h.dfdByteLength          = dfdLength;
    h.kvdByteOffset          = kvdOffset;
    h.kvdByteLength          = kvdLength;
    h.sgdByteOffset          = 0;
    h.sgdByteLength          = 0;

    // --- Emit (resize zero-fills, so every padding byte is 0) -----------------------------
    Vec<u8> out(a, Tag::Cook);
    out.resize(usize(total));
    u8* p = out.data();
    std::memcpy(p, &h, sizeof(h));
    std::memcpy(p + sizeof(Header), index, levelCount * sizeof(LevelIndex));
    write_dfd(p + dfdOffset, *info, desc.premultipliedAlpha);

    u8* kv = p + kvdOffset;
    for (u32 i = 0; i < entryCount; ++i) {
        Entry const& e     = entries[i];
        u32 const kvLength = u32(e.key.size) + 1u + u32(e.value.size) + 1u;
        write_unaligned<u32>(kv, kvLength);
        std::memcpy(kv + 4, e.key.data, e.key.size);
        if (e.value.size) std::memcpy(kv + 4 + e.key.size + 1, e.value.data, e.value.size);
        // the key/value NULs and the entry padding are already zero
        kv += align_up(u64(4) + kvLength, u64(4));
    }

    for (u32 i = 0; i < levelCount; ++i)
        if (stored[i].size) std::memcpy(p + index[i].byteOffset, stored[i].data, stored[i].size);

    return Result<Vec<u8>>(std::move(out));
}

} // namespace kiln::ktx2
