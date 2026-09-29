// KTX2 reader: validates the header, level index, DFD and KVD.
#include "kiln/ktx2.h"

#include "formats_internal.h"

namespace kiln::ktx2 {

char const* supercompression_name(Supercompression s) noexcept {
    switch (s) {
    case Supercompression::None: return "None";
    case Supercompression::BasisLZ: return "BasisLZ";
    case Supercompression::Zstd: return "Zstd";
    case Supercompression::Zlib: return "Zlib";
    }
    return "Unknown";
}

namespace {

constexpr u64 kU64Max = ~u64(0);

/// checked_mul / checked_add (core.h) over u64, so u32 operands convert.
bool mul_ok(u64 a, u64 b, u64& out) noexcept { return checked_mul(a, b, out); }
bool add_ok(u64 a, u64 b, u64& out) noexcept { return checked_add(a, b, out); }

/// Bytes of one w x h x d image in `info`, whole blocks, overflow-checked.
bool image_bytes(FormatInfo const& info, u32 w, u32 h, u32 d, u64& out) noexcept {
    u64 bw = (u64(w) + info.blockWidth - 1) / info.blockWidth;
    u64 bh = (u64(h) + info.blockHeight - 1) / info.blockHeight;
    u64 n  = 0;
    return mul_ok(bw, bh, n) && mul_ok(n, d, n) && mul_ok(n, info.bytesPerBlock, out);
}

/// Walk KVD entries; `fn` returns true to stop. Returns false if the KVD is malformed
/// (and sets `err` to a static message).
template <class Fn> bool walk_kvd(Span<u8 const> kvd, Fn&& fn, char const** err = nullptr) noexcept {
    usize pos = 0;
    while (pos < kvd.size) {
        if (kvd.size - pos < 4) {
            if (err) *err = "entry length field truncated";
            return false;
        }
        u32 len = read_unaligned<u32>(kvd.data + pos);
        pos += 4;
        if (len > kvd.size - pos) {
            if (err) *err = "entry runs past kvdByteLength";
            return false;
        }
        u8 const* entry = kvd.data + pos;
        usize keyLen    = 0;
        while (keyLen < len && entry[keyLen] != 0)
            ++keyLen;
        if (keyLen == len) {
            if (err) *err = "key is not NUL-terminated";
            return false;
        }
        if (keyLen == 0) {
            if (err) *err = "empty key";
            return false;
        }
        StrView key{reinterpret_cast<char const*>(entry), keyLen};
        Span<u8 const> value{entry + keyLen + 1, len - keyLen - 1};
        if (fn(key, value)) return true;
        pos = align_up(pos + len, usize(4)); // may step past the end: the last entry's padding is optional
    }
    return true;
}

Status fail(DiagSink const* diag, StrView asset, Code code, u32 diagCode, char const* where, char const* fmt,
            u64 a = 0, u64 b = 0) noexcept {
    // All messages take up to two integers so callers don't need their own varargs.
    return diagf(diag, make_status(code), diagCode, Severity::Error, asset, where, fmt,
                 static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

} // namespace

u64 Ktx2View::metadata_size(Header const& h) noexcept {
    u64 end = sizeof(Header) + u64(max(h.levelCount, 1u)) * sizeof(LevelIndex);
    if (h.dfdByteLength != 0) end = max(end, u64(h.dfdByteOffset) + h.dfdByteLength);
    if (h.kvdByteLength != 0) end = max(end, u64(h.kvdByteOffset) + h.kvdByteLength);
    if (h.sgdByteLength != 0 && h.sgdByteOffset <= kU64Max - h.sgdByteLength)
        end = max(end, h.sgdByteOffset + h.sgdByteLength);
    return end;
}

Result<Ktx2View> Ktx2View::open(Span<u8 const> bytes, DiagSink const* diag, StrView asset) noexcept {
    Ktx2View v;
    v.bytes_ = bytes;

    // --- Header ------------------------------------------------------------------
    if (bytes.size >= sizeof(kIdentifier) && std::memcmp(bytes.data, kIdentifier, sizeof(kIdentifier)) != 0)
        return fail(diag, asset, Code::Corrupt, kDiagKtxIdentifier, "header",
                    "not a KTX2 file (bad identifier)");
    if (bytes.size < sizeof(Header))
        return fail(diag, asset, Code::Corrupt, kDiagKtxTruncated, "header",
                    "truncated: %llu bytes, header needs %llu", bytes.size, sizeof(Header));
    std::memcpy(&v.header_, bytes.data, sizeof(Header));
    Header const& h = v.header_;

    if (h.supercompressionScheme != u32(Supercompression::None))
        return fail(diag, asset, Code::Unsupported, kDiagKtxSupercompression, "header",
                    "supercompression scheme %llu is not supported", h.supercompressionScheme);
    if (h.sgdByteLength != 0)
        return fail(diag, asset, Code::Corrupt, kDiagKtxSupercompression, "header",
                    "supercompression global data (%llu bytes) without a supercompression scheme",
                    h.sgdByteLength);

    Format const format = static_cast<Format>(h.vkFormat);
    v.info_             = kiln::format_info(format);
    if (format == Format::Undefined || v.info_ == nullptr)
        return fail(diag, asset, Code::Unsupported, kDiagKtxFormat, "header",
                    "vkFormat %llu is not supported", h.vkFormat);
    FormatInfo const& info = *v.info_;

    u32 const expectedTypeSize = info.compressed ? 1u : u32(info.bitsPerChannel / 8);
    if (h.typeSize != expectedTypeSize)
        return fail(diag, asset, Code::Corrupt, kDiagKtxTypeSize, "header",
                    "typeSize %llu, format needs %llu", h.typeSize, expectedTypeSize);

    if (h.pixelWidth == 0)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDimensions, "header", "pixelWidth is 0");
    if (h.pixelHeight == 0)
        return fail(diag, asset, Code::Unsupported, kDiagKtxDimensions, "header",
                    "1D textures (pixelHeight 0) are not supported");
    if (h.pixelDepth != 0)
        return fail(diag, asset, Code::Unsupported, kDiagKtxDimensions, "header",
                    "3D textures (pixelDepth %llu) are not supported", h.pixelDepth);
    if (h.faceCount != 1 && h.faceCount != 6)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDimensions, "header",
                    "faceCount %llu (must be 1 or 6)", h.faceCount);
    if (h.faceCount == 6 && h.pixelWidth != h.pixelHeight)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDimensions, "header",
                    "cube map faces are not square (%llux%llu)", h.pixelWidth, h.pixelHeight);
    u32 const maxLevels = u32(std::bit_width(max(h.pixelWidth, h.pixelHeight)));
    if (h.levelCount > maxLevels)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDimensions, "header",
                    "levelCount %llu exceeds the full mip chain (%llu)", h.levelCount, maxLevels);

    v.desc_ = TextureDesc{
        .format  = format,
        .width   = h.pixelWidth,
        .height  = h.pixelHeight,
        .depth   = 1,
        .layers  = max(h.layerCount, 1u),
        .faces   = h.faceCount,
        .levels  = max(h.levelCount, 1u),
        .isArray = h.layerCount != 0,
        .isCube  = h.faceCount == 6,
    };
    u32 const levelCount = v.desc_.levels;

    // --- Level index -----------------------------------------------------------------
    u64 const indexEnd = sizeof(Header) + u64(levelCount) * sizeof(LevelIndex);
    if (bytes.size < indexEnd)
        return fail(diag, asset, Code::Corrupt, kDiagKtxTruncated, "levelIndex",
                    "truncated: %llu bytes, level index ends at %llu", bytes.size, indexEnd);
    std::memcpy(v.levels_, bytes.data + sizeof(Header), levelCount * sizeof(LevelIndex));

    // --- DFD -------------------------------------------------------------------------
    if (h.dfdByteLength < 4)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDfd, "dfd", "dfdByteLength %llu is too small",
                    h.dfdByteLength);
    if (h.dfdByteOffset < indexEnd || h.dfdByteOffset % 4 != 0)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDfd, "dfd",
                    "dfdByteOffset %llu is misaligned or overlaps the level index (ends at %llu)",
                    h.dfdByteOffset, indexEnd);
    u64 const dfdEnd = u64(h.dfdByteOffset) + h.dfdByteLength;
    if (dfdEnd > bytes.size)
        return fail(diag, asset, Code::Corrupt, kDiagKtxTruncated, "dfd",
                    "truncated: %llu bytes, DFD ends at %llu", bytes.size, dfdEnd);
    v.dfd_             = bytes.subspan(h.dfdByteOffset, h.dfdByteLength);
    u32 const dfdTotal = read_unaligned<u32>(v.dfd_.data);
    if (dfdTotal != h.dfdByteLength)
        return fail(diag, asset, Code::Corrupt, kDiagKtxDfd, "dfd",
                    "DFD totalSize %llu != dfdByteLength %llu", dfdTotal, h.dfdByteLength);
    if (v.dfd_.size >= 4 + kDfdBasicBlockHeaderSize) {
        u32 const word0     = read_unaligned<u32>(v.dfd_.data + 4);
        u32 const word1     = read_unaligned<u32>(v.dfd_.data + 8);
        u32 const blockSize = word1 >> 16;
        if (blockSize < kDfdBasicBlockHeaderSize || blockSize > v.dfd_.size - 4)
            return fail(diag, asset, Code::Corrupt, kDiagKtxDfd, "dfd",
                        "descriptorBlockSize %llu does not fit in the DFD (%llu bytes)", blockSize,
                        v.dfd_.size);
        // Only the Khronos basic block (vendor 0, type 0) carries a transfer function we understand.
        if (word0 == 0) {
            bool const dfdSrgb = v.dfd_transfer_function() == kDfdTransferSrgb;
            if (dfdSrgb != info.srgb)
                (void)diagf(diag, kOk, kDiagKtxDfd, Severity::Warning, asset, "dfd",
                            "DFD transferFunction %u disagrees with vkFormat %s; using vkFormat",
                            unsigned(v.dfd_transfer_function()), info.name);
        }
    }

    // --- KVD -------------------------------------------------------------------------
    if (h.kvdByteLength != 0) {
        if (h.kvdByteOffset < indexEnd || h.kvdByteOffset % 4 != 0)
            return fail(diag, asset, Code::Corrupt, kDiagKtxKvd, "kvd",
                        "kvdByteOffset %llu is misaligned or overlaps the level index (ends at %llu)",
                        h.kvdByteOffset, indexEnd);
        u64 const kvdEnd = u64(h.kvdByteOffset) + h.kvdByteLength;
        if (kvdEnd > bytes.size)
            return fail(diag, asset, Code::Corrupt, kDiagKtxTruncated, "kvd",
                        "truncated: %llu bytes, KVD ends at %llu", bytes.size, kvdEnd);
        if (h.kvdByteOffset < dfdEnd && h.dfdByteOffset < kvdEnd)
            return fail(diag, asset, Code::Corrupt, kDiagKtxKvd, "kvd",
                        "KVD at %llu overlaps the DFD (ends at %llu)", h.kvdByteOffset, dfdEnd);
        v.kvd_          = bytes.subspan(h.kvdByteOffset, h.kvdByteLength);
        char const* err = "";
        if (!walk_kvd(v.kvd_, [](StrView, Span<u8 const>) { return false; }, &err))
            return diagf(diag, make_status(Code::Corrupt), kDiagKtxKvd, Severity::Error, asset, "kvd",
                         "malformed key/value data: %s", err);
    }

    // --- Level ranges ------------------------------------------------------------------
    u64 const metaEnd = metadata_size(h);
    u32 const align   = fmt::ktx2_level_align(info.bytesPerBlock);
    for (u32 i = 0; i < levelCount; ++i) {
        LevelIndex const& li = v.levels_[i];
        if (li.byteLength != li.uncompressedByteLength)
            return fail(diag, asset, Code::Corrupt, kDiagKtxLevelIndex, "levelIndex",
                        "level byteLength %llu != uncompressedByteLength %llu", li.byteLength,
                        li.uncompressedByteLength);
        u64 expected = 0;
        if (!image_bytes(info, v.level_width(i), v.level_height(i), v.level_depth(i), expected) ||
            !mul_ok(expected, u64(v.desc_.faces) * v.desc_.layers, expected))
            return fail(diag, asset, Code::Corrupt, kDiagKtxDimensions, "levelIndex",
                        "level %llu size overflows (%llu faces x layers)", i,
                        u64(v.desc_.faces) * v.desc_.layers);
        if (li.byteLength != expected)
            return fail(diag, asset, Code::Corrupt, kDiagKtxLevelIndex, "levelIndex",
                        "level byteLength %llu, dimensions need %llu", li.byteLength, expected);
        if (li.byteOffset % align != 0)
            return fail(diag, asset, Code::Corrupt, kDiagKtxLevelIndex, "levelIndex",
                        "level byteOffset %llu is not aligned to %llu", li.byteOffset, align);
        u64 end = 0;
        if (li.byteOffset < metaEnd || !add_ok(li.byteOffset, li.byteLength, end))
            return fail(diag, asset, Code::Corrupt, kDiagKtxLevelIndex, "levelIndex",
                        "level byteOffset %llu overlaps the metadata (ends at %llu)", li.byteOffset, metaEnd);
        // KTX2 stores the smallest level first: level i must end at or before level i-1 starts.
        if (i > 0 && end > v.levels_[i - 1].byteOffset)
            return fail(diag, asset, Code::Corrupt, kDiagKtxLevelIndex, "levelIndex",
                        "level %llu is not stored before level %llu (levels must be smallest first)", i,
                        i - 1);
    }

    return v;
}

u64 Ktx2View::level_image_bytes(u32 level) const noexcept {
    if (info_ == nullptr || level >= desc_.levels) return 0;
    u64 n = 0;
    return image_bytes(*info_, level_width(level), level_height(level), level_depth(level), n) ? n : 0;
}

Span<u8 const> Ktx2View::level_data(u32 level) const noexcept {
    if (level >= desc_.levels) return {};
    LevelIndex const& li = levels_[level];
    if (li.byteOffset > bytes_.size || li.byteLength > bytes_.size - li.byteOffset) return {};
    return bytes_.subspan(usize(li.byteOffset), usize(li.byteLength));
}

bool Ktx2View::has_all_level_data() const noexcept {
    for (u32 i = 0; i < desc_.levels; ++i)
        if (level_data(i).empty()) return false;
    return desc_.levels > 0;
}

u32 Ktx2View::dfd_sample_count() const noexcept {
    if (dfd_.size < 4 + kDfdBasicBlockHeaderSize) return 0;
    u32 const blockSize = read_unaligned<u32>(dfd_.data + 8) >> 16;
    return blockSize < kDfdBasicBlockHeaderSize ? 0 : (blockSize - kDfdBasicBlockHeaderSize) / kDfdSampleSize;
}

Span<u8 const> Ktx2View::find_key(StrView key) const noexcept {
    Span<u8 const> found;
    (void)walk_kvd(kvd_, [&](StrView k, Span<u8 const> value) {
        if (k != key) return false;
        found = value;
        return true;
    });
    return found;
}

void Ktx2View::for_each_key(void (*fn)(void* user, StrView key, Span<u8 const> value),
                            void* user) const noexcept {
    (void)walk_kvd(kvd_, [&](StrView k, Span<u8 const> value) {
        fn(user, k, value);
        return false;
    });
}

} // namespace kiln::ktx2
