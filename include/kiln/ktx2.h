// kiln/ktx2.h — minimal KTX2 reader (kiln_runtime).
//
// Parses and validates the KTX2 container: header, level index, data format
// descriptor (DFD) and key/value data (KVD). The reader never allocates and never
// copies pixel data; a Ktx2View is a small value that points into caller memory.
//
// open() accepts either a whole file or just its "metadata prefix" (header + level
// index + DFD + KVD), so a runtime can read the small prefix first and schedule
// the level reads afterwards. v0.5 scope: 2D textures (optionally arrays and
// cubes), no supercompression. See docs/HANDOFF.md §4.2.
#pragma once

#include "kiln/core.h"
#include "kiln/formats.h"
#include "kiln/result.h"

namespace kiln::ktx2 {

// ---------------------------------------------------------------------------
// On-disk structures (little-endian, KTX 2.0 spec §3)
// ---------------------------------------------------------------------------

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
[[nodiscard]] KILN_API char const* supercompression_name(Supercompression s) noexcept;

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
    kDiagKtxSupercompression = 4107, ///< supercompression scheme not supported
    kDiagKtxDfd              = 4108, ///< malformed data format descriptor
    kDiagKtxKvd              = 4109, ///< malformed key/value data
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

// ---------------------------------------------------------------------------
// Ktx2View
// ---------------------------------------------------------------------------

/// A validated view of a KTX2 file (or its metadata prefix) in caller memory.
/// The bytes passed to open() must outlive the view. Copyable, no allocation.
class KILN_API Ktx2View {
public:
    Ktx2View() noexcept = default;

    /// Validate `bytes` and return a view. `bytes` must contain at least the metadata
    /// prefix (header, level index, DFD and KVD); level data that the span contains
    /// is available through level_data(). On failure, one diagnostic with a K41xx code
    /// is emitted to `diag` and its Status is returned.
    [[nodiscard]] static Result<Ktx2View> open(Span<u8 const> bytes, DiagSink const* diag = nullptr,
                                               StrView assetName = {}) noexcept;

    /// Size of the metadata prefix described by `header` (end of the level index, DFD
    /// and KVD, whichever is last). Lets a loader read the header, then the prefix.
    [[nodiscard]] static u64 metadata_size(Header const& header) noexcept;

    [[nodiscard]] Header const& header() const noexcept { return header_; }
    [[nodiscard]] TextureDesc desc() const noexcept { return desc_; }
    [[nodiscard]] FormatInfo const& info() const noexcept { return *info_; }

    /// The level index; entry i describes mip level i (0 = largest). Size == desc().levels.
    [[nodiscard]] Span<LevelIndex const> levels() const noexcept { return {levels_, desc_.levels}; }

    /// header().levelCount == 0: the file stores one level and asks the loader to
    /// generate the rest of the mip chain.
    [[nodiscard]] bool wants_generated_mips() const noexcept { return header_.levelCount == 0; }

    [[nodiscard]] u32 level_width(u32 level) const noexcept { return max(desc_.width >> level, 1u); }
    [[nodiscard]] u32 level_height(u32 level) const noexcept { return max(desc_.height >> level, 1u); }
    [[nodiscard]] u32 level_depth(u32 level) const noexcept { return max(desc_.depth >> level, 1u); }

    /// Bytes of one face/layer image at `level` (all depth slices), tightly packed.
    [[nodiscard]] u64 level_image_bytes(u32 level) const noexcept;

    /// Raw bytes of level `level` (all layers and faces) if the span given to open()
    /// contained them, else empty.
    [[nodiscard]] Span<u8 const> level_data(u32 level) const noexcept;

    /// True when the span given to open() contained the data of every level.
    [[nodiscard]] bool has_all_level_data() const noexcept;

    /// The DFD bytes (starting with totalSize).
    [[nodiscard]] Span<u8 const> dfd() const noexcept { return dfd_; }
    /// Basic descriptor block fields.
    [[nodiscard]] u8 dfd_color_model() const noexcept { return dfd_byte(8); }
    [[nodiscard]] u8 dfd_transfer_function() const noexcept { return dfd_byte(10); }
    [[nodiscard]] u8 dfd_flags() const noexcept { return dfd_byte(11); }
    [[nodiscard]] u32 dfd_sample_count() const noexcept;

    /// The KVD bytes (validated entries).
    [[nodiscard]] Span<u8 const> kvd() const noexcept { return kvd_; }
    [[nodiscard]] bool has_kvd() const noexcept { return !kvd_.empty(); }

    /// Look up `key` in the KVD. Returns the value bytes, including any trailing NUL
    /// the writer stored, or an empty span if the key is absent.
    [[nodiscard]] Span<u8 const> find_key(StrView key) const noexcept;

    /// Iterate over KVD entries: fn(user, key, value) for each, in file order.
    void for_each_key(void (*fn)(void* user, StrView key, Span<u8 const> value), void* user) const noexcept;

private:
    /// Byte `off` of the basic descriptor block (0 if absent).
    [[nodiscard]] u8 dfd_byte(usize off) const noexcept {
        return dfd_.size > 4 + off ? dfd_.data[4 + off] : u8(0);
    }

    Span<u8 const> bytes_;
    Span<u8 const> dfd_;
    Span<u8 const> kvd_;
    FormatInfo const* info_ = nullptr;
    Header header_{};
    TextureDesc desc_{};
    LevelIndex levels_[kMaxLevels]{};
};

} // namespace kiln::ktx2
