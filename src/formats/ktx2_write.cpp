// KTX2 writer for raw (uncompressed, non-supercompressed) 2D textures.
#include "kiln/cook/ktx2_writer.h"

namespace kiln::ktx2 {

namespace {

constexpr u32 gcd(u32 a, u32 b) noexcept {
    while (b != 0) {
        u32 t = a % b;
        a     = b;
        b     = t;
    }
    return a;
}
constexpr u32 lcm(u32 a, u32 b) noexcept { return a / gcd(a, b) * b; }

/// Round up to a multiple of `a` (not necessarily a power of two, e.g. 12).
constexpr u64 round_up(u64 v, u64 a) noexcept { return (v + a - 1) / a * a; }

// KHR_DF channel ids and qualifiers for the RGBSDA color model.
constexpr u32 kChannelIds[4] = {0, 1, 2, 15}; // R, G, B, A
// Sample qualifier bits (KHR_DF_SAMPLE_DATATYPE_*): bits 4..7 of the channelType byte.
constexpr u32 kQualifierLinear    = 0x10; // 0x20 is EXPONENT; libktx's validator checks this
constexpr u32 kQualifierSigned    = 0x40;
constexpr u32 kQualifierFloat     = 0x80;
constexpr u8 kColorModelRgbsda    = 1;
constexpr u8 kColorPrimariesBt709 = 1;

constexpr char kWriterKey[] = "KTXwriter"; // written with its NUL

u32 dfd_size(FormatInfo const& info) noexcept {
    return 4 + kDfdBasicBlockHeaderSize + kDfdSampleSize * info.channels;
}

/// Write the DFD for an uncompressed format at `out` (dfd_size(info) bytes, zeroed).
void write_dfd(u8* out, FormatInfo const& info, bool premultiplied) noexcept {
    u32 const total     = dfd_size(info);
    u32 const blockSize = total - 4;
    write_unaligned<u32>(out + 0, total);
    write_unaligned<u32>(out + 4, 0u);                     // vendorId 0 (Khronos) | descriptorType 0 (basic)
    write_unaligned<u32>(out + 8, 2u | (blockSize << 16)); // versionNumber 2 | descriptorBlockSize
    out[12] = kColorModelRgbsda;
    out[13] = kColorPrimariesBt709;
    out[14] = info.srgb ? kDfdTransferSrgb : kDfdTransferLinear;
    out[15] = premultiplied ? kDfdFlagAlphaPremultiplied : u8(0);
    // out[16..19] texelBlockDimension: 0 == 1 texel in each dimension.
    out[20]        = info.bytesPerBlock; // bytesPlane0; planes 1..7 stay 0
    u32 const bits = info.bitsPerChannel;

    u32 lower = 0, upper = 0, qualifiers = 0;
    switch (info.kind) {
    case FormatKind::UNorm: upper = u32((u64(1) << bits) - 1); break;
    case FormatKind::SNorm:
        upper      = u32((u64(1) << (bits - 1)) - 1);
        lower      = ~upper + 1u; // two's complement of -upper
        qualifiers = kQualifierSigned;
        break;
    case FormatKind::SFloat:
        lower      = 0xBF800000u; // -1.0f
        upper      = 0x3F800000u; //  1.0f
        qualifiers = kQualifierSigned | kQualifierFloat;
        break;
    case FormatKind::UFloat: // compressed only (BC6H); rejected before we get here
        upper      = 0x3F800000u;
        qualifiers = kQualifierFloat;
        break;
    }

    u8* sample = out + 4 + kDfdBasicBlockHeaderSize;
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
               unsigned long long b = 0, unsigned long long c = 0) noexcept {
    return diagf(diag, make_status(Code::InvalidArgument), code, Severity::Error, {}, "ktx2 write", fmt, a, b,
                 c);
}

} // namespace

Result<Vec<u8>> write(WriteDesc const& desc, Allocator const* alloc, DiagSink const* diag) noexcept {
    // --- Validate --------------------------------------------------------------------
    FormatInfo const* info = format_info(desc.format);
    if (desc.format == Format::Undefined || info == nullptr)
        return invalid(diag, kDiagKtxFormat, "unknown format %llu", u32(desc.format));
    if (info->compressed)
        return invalid(diag, kDiagKtxFormat,
                       "block-compressed format %llu is not supported by the v0.5 writer", u32(desc.format));
    if (desc.width == 0 || desc.height == 0)
        return invalid(diag, kDiagKtxDimensions, "extent %llux%llu must be at least 1x1", desc.width,
                       desc.height);
    u32 const maxLevels = u32(std::bit_width(max(desc.width, desc.height)));
    if (desc.levels.size == 0 || desc.levels.size > maxLevels)
        return invalid(diag, kDiagKtxDimensions, "%llu levels given, must be 1..%llu", desc.levels.size,
                       maxLevels);
    if (desc.writerTag.find('\0') != StrView::kNpos)
        return invalid(diag, kDiagKtxKvd, "writerTag contains a NUL byte");
    u32 const levelCount = u32(desc.levels.size);
    for (u32 i = 0; i < levelCount; ++i) {
        u64 const expected =
            format_image_bytes(desc.format, max(desc.width >> i, 1u), max(desc.height >> i, 1u));
        if (desc.levels[i].size != expected)
            return invalid(diag, kDiagKtxLevelIndex, "level %llu has %llu bytes, expected %llu", i,
                           desc.levels[i].size, expected);
        if (expected != 0 && desc.levels[i].data == nullptr)
            return invalid(diag, kDiagKtxLevelIndex, "level %llu has no data", i);
    }

    // --- Layout ----------------------------------------------------------------------
    u32 const dfdOffset = u32(sizeof(Header) + levelCount * sizeof(LevelIndex));
    u32 const dfdLength = dfd_size(*info);
    u32 const kvdOffset = dfdOffset + dfdLength;
    u32 const keyBytes  = u32(sizeof(kWriterKey));       // includes NUL
    u32 const valueSize = u32(desc.writerTag.size) + 1u; // includes NUL
    u32 const kvLength  = keyBytes + valueSize;
    u32 const kvdLength = u32(align_up(u64(4) + kvLength, u64(4)));

    u64 const align = lcm(info->bytesPerBlock, 4u);
    LevelIndex index[kMaxLevels]{};
    u64 pos = u64(kvdOffset) + kvdLength;
    for (u32 i = levelCount; i-- > 0;) { // smallest level first, level 0 last
        pos                             = round_up(pos, align);
        index[i].byteOffset             = pos;
        index[i].byteLength             = desc.levels[i].size;
        index[i].uncompressedByteLength = desc.levels[i].size;
        pos += desc.levels[i].size;
    }
    u64 const total = pos;

    Header h{};
    std::memcpy(h.identifier, kIdentifier, sizeof(kIdentifier));
    h.vkFormat               = u32(desc.format);
    h.typeSize               = u32(info->bitsPerChannel / 8);
    h.pixelWidth             = desc.width;
    h.pixelHeight            = desc.height;
    h.pixelDepth             = 0;
    h.layerCount             = 0;
    h.faceCount              = 1;
    h.levelCount             = levelCount;
    h.supercompressionScheme = u32(Supercompression::None);
    h.dfdByteOffset          = dfdOffset;
    h.dfdByteLength          = dfdLength;
    h.kvdByteOffset          = kvdOffset;
    h.kvdByteLength          = kvdLength;
    h.sgdByteOffset          = 0;
    h.sgdByteLength          = 0;

    // --- Emit (resize zero-fills, so every padding byte is 0) -----------------------------
    Vec<u8> out(alloc ? alloc : default_allocator(), Tag::Cook);
    out.resize(usize(total));
    u8* p = out.data();
    std::memcpy(p, &h, sizeof(h));
    std::memcpy(p + sizeof(Header), index, levelCount * sizeof(LevelIndex));
    write_dfd(p + dfdOffset, *info, desc.premultipliedAlpha);

    u8* kv = p + kvdOffset;
    write_unaligned<u32>(kv, kvLength);
    std::memcpy(kv + 4, kWriterKey, keyBytes);
    if (desc.writerTag.size) std::memcpy(kv + 4 + keyBytes, desc.writerTag.data, desc.writerTag.size);
    // the value's NUL and the entry padding are already zero

    for (u32 i = 0; i < levelCount; ++i)
        if (desc.levels[i].size)
            std::memcpy(p + index[i].byteOffset, desc.levels[i].data, desc.levels[i].size);

    return Result<Vec<u8>>(std::move(out));
}

} // namespace kiln::ktx2
