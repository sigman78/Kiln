// kiln/ktx2.h — minimal zero-copy KTX2 reader (kiln_runtime): header, level index, DFD, KVD.
// Scope: 2D textures, optionally arrays and cubes; levels plain or Zstd-supercompressed.
#pragma once

#include "kiln/alloc.h"
#include "kiln/core.h"
#include "kiln/formats.h"
#include "kiln/result.h"

namespace kiln::ktx2 {

// On-disk structures (little-endian, KTX 2.0 spec §3).

inline constexpr u8 kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

struct Header {
    u8 identifier[12];
    u32 vkFormat;
    u32 typeSize;
    u32 pixelWidth;
    u32 pixelHeight;
    u32 pixelDepth;
    u32 layerCount;
    u32 faceCount;
    u32 levelCount;
    u32 supercompressionScheme;
    u32 dfdByteOffset;
    u32 dfdByteLength;
    u32 kvdByteOffset;
    u32 kvdByteLength;
    u64 sgdByteOffset;
    u64 sgdByteLength;
};
static_assert(sizeof(Header) == 80);
static_assert(std::is_trivially_copyable_v<Header>);

struct LevelIndex {
    u64 byteOffset;
    u64 byteLength;
    u64 uncompressedByteLength;
};
static_assert(sizeof(LevelIndex) == 24);
static_assert(std::is_trivially_copyable_v<LevelIndex>);

enum class Supercompression : u32 { None = 0, BasisLZ = 1, Zstd = 2, Zlib = 3 };

/// Human-readable name ("None", "BasisLZ", "Zstd", "Zlib", or "Unknown").
KILN_API char const* supercompression_name(Supercompression s);

/// KHR_DF constants used by the reader and writer.
inline constexpr u32 kDfdBasicBlockHeaderSize  = 24; ///< basic descriptor block without samples
inline constexpr u32 kDfdSampleSize            = 16;
inline constexpr u8 kDfdTransferLinear         = 1;
inline constexpr u8 kDfdTransferSrgb           = 2;
inline constexpr u8 kDfdFlagAlphaPremultiplied = 1;

/// Upper bound on levels: a u32 extent has at most 32 mip levels.
inline constexpr u32 kMaxLevels = 32;

/// Diagnostic codes (K4100-K4199 are reserved for KTX2, docs/design/error-model.md).
enum DiagCode : u32 {
    kDiagKtxIdentifier       = 4101, ///< file identifier mismatch
    kDiagKtxTruncated        = 4102, ///< span too short for header / level index / DFD / KVD
    kDiagKtxFormat           = 4103, ///< unknown or Undefined vkFormat
    kDiagKtxTypeSize         = 4104, ///< typeSize inconsistent with the format
    kDiagKtxDimensions       = 4105, ///< width/height/depth/faces/levels invalid or unsupported
    kDiagKtxLevelIndex       = 4106, ///< offset/length out of file, misaligned, size mismatch, order
    kDiagKtxSupercompression = 4107, ///< supercompression scheme not supported (only Zstd is)
    kDiagKtxDfd              = 4108, ///< malformed data format descriptor
    kDiagKtxKvd              = 4109, ///< malformed key/value data
    kDiagKtxLevelDecode      = 4110, ///< a Zstd level is not one frame of uncompressedByteLength bytes
};

/// Engine-facing description of the texture. All counts are >= 1.
struct TextureDesc {
    Format format = Format::Undefined;
    u32 width     = 0;
    u32 height    = 0;
    u32 depth     = 1;
    u32 layers    = 1;
    u32 faces     = 1;
    u32 levels    = 1;
    bool isArray  = false; ///< header layerCount != 0 (an array, possibly of one layer)
    bool isCube   = false; ///< faceCount == 6
};

/// A validated view of a KTX2 file (or its metadata prefix) in caller memory.
/// The bytes passed to open() must outlive the view. Copyable, no allocation.
class KILN_API Ktx2View {
public:
    Ktx2View() = default;

    /// Validates `bytes`, which must hold at least the metadata prefix (header, level
    /// index, DFD, KVD); level data in the span is available through level_data().
    /// On failure, emits one K41xx diagnostic to `diag` and returns its Status.
    static Result<Ktx2View> open(Span<u8 const> bytes, DiagSink const* diag = nullptr,
                                 StrView assetName = {});

    /// Size of the metadata prefix described by `header` (end of the level index, DFD
    /// and KVD, whichever is last). Lets a loader read the header, then the prefix.
    static u64 metadata_size(Header const& header);

    Header const& header() const { return header_; }
    TextureDesc desc() const { return desc_; }
    FormatInfo const& info() const { return *info_; }

    /// The level index; entry i describes mip level i (0 = largest). Size == desc().levels.
    Span<LevelIndex const> levels() const { return {levels_, desc_.levels}; }

    /// header().levelCount == 0: the file stores one level and asks the loader to
    /// generate the rest of the mip chain.
    [[nodiscard]] bool wants_generated_mips() const { return header_.levelCount == 0; }

    u32 level_width(u32 level) const { return max(desc_.width >> level, 1u); }
    u32 level_height(u32 level) const { return max(desc_.height >> level, 1u); }
    u32 level_depth(u32 level) const { return max(desc_.depth >> level, 1u); }

    /// Bytes of one face/layer image at `level` (all depth slices), tightly packed.
    u64 level_image_bytes(u32 level) const;

    /// True when every level is one Zstd frame (supercompressionScheme 2). level_data() then
    /// holds the frames; decode_level() gives the texels.
    [[nodiscard]] bool supercompressed() const { return header_.supercompressionScheme != 0; }

    /// Stored bytes of level `level` (all layers and faces) if the span given to open()
    /// contained them, else empty. Compressed when supercompressed().
    Span<u8 const> level_data(u32 level) const;

    /// Writes the texels of level `level` into `out`, which must hold exactly
    /// levels()[level].uncompressedByteLength bytes. Copies them when the file is not
    /// supercompressed. The Zstd decoder takes its memory from `alloc` (Tag::Io; nullptr is
    /// default_allocator()). InvalidArgument: wrong `out` size, or the level is not in the span.
    /// Corrupt (K4110): the stored bytes do not decode to the level.
    Status decode_level(u32 level, Span<u8> out, Allocator const* alloc = nullptr,
                        DiagSink const* diag = nullptr, StrView assetName = {}) const;

    /// True when the span given to open() contained the data of every level.
    [[nodiscard]] bool has_all_level_data() const;

    /// The DFD bytes (starting with totalSize).
    Span<u8 const> dfd() const { return dfd_; }
    /// Basic descriptor block fields.
    u8 dfd_color_model() const { return dfd_byte(8); }
    u8 dfd_transfer_function() const { return dfd_byte(10); }
    u8 dfd_flags() const { return dfd_byte(11); }
    u32 dfd_sample_count() const;

    /// The KVD bytes (validated entries).
    Span<u8 const> kvd() const { return kvd_; }
    [[nodiscard]] bool has_kvd() const { return !kvd_.empty(); }

    /// Look up `key` in the KVD. Returns the value bytes, including any trailing NUL
    /// the writer stored, or an empty span if the key is absent.
    Span<u8 const> find_key(StrView key) const;

    /// Iterate over KVD entries: fn(user, key, value) for each, in file order.
    void for_each_key(void (*fn)(void* user, StrView key, Span<u8 const> value), void* user) const;

private:
    /// Byte `off` of the basic descriptor block (0 if absent).
    u8 dfd_byte(usize off) const { return dfd_.size > 4 + off ? dfd_.data[4 + off] : u8(0); }

    Span<u8 const> bytes_;
    Span<u8 const> dfd_;
    Span<u8 const> kvd_;
    FormatInfo const* info_ = nullptr;
    Header header_{};
    TextureDesc desc_{};
    LevelIndex levels_[kMaxLevels]{};
};

} // namespace kiln::ktx2
