// kiln/catalog.h — 128-bit content hashes (XXH3-128) and the store catalog, format 0.1: a sorted
// map from asset names to the build keys of their artifacts (docs/design/store-catalog.md).
#pragma once

#include "kiln/assets.h"

namespace kiln {

/// A 128-bit hash in XXH3's canonical byte order: the hex of the bytes is what `xxhsum -H2` prints.
struct Hash128 {
    u8 bytes[16] = {};

    [[nodiscard]] friend bool operator==(Hash128 const& a, Hash128 const& b) noexcept {
        return std::memcmp(a.bytes, b.bytes, sizeof a.bytes) == 0;
    }
    [[nodiscard]] bool is_zero() const noexcept { return *this == Hash128{}; }
};

/// XXH3-128 of `bytes`, seed 0. A checksum, not a signature.
[[nodiscard]] KILN_API Hash128 xxh3_128(Span<u8 const> bytes) noexcept;

/// 32 lowercase hex digits and a NUL into `out`.
KILN_API void hash128_hex(Hash128 const& h, char (&out)[33]) noexcept;

// ---------------------------------------------------------------------------
// Catalog format 0.1 (little-endian; sections at 8-byte boundaries)
// ---------------------------------------------------------------------------

inline constexpr u32 kCatalogMagic       = fourcc('K', 'C', 'A', 'T');
inline constexpr u16 kCatalogMajor       = 0;
inline constexpr u16 kCatalogMinor       = 1;
inline constexpr u32 kCatalogHeaderBytes = 128;
inline constexpr u32 kCatalogEntryBytes  = 56;
inline constexpr u32 kCatalogIndexBytes  = 16;
/// Byte offset of the checksum in the header; the checksum hashes the file with it zeroed.
inline constexpr u32 kCatalogChecksumOffset = 88;

/// Diagnostic codes of a malformed catalog (K4201-K4209).
enum CatalogDiagCode : u32 {
    kDiagCatalogMagic     = 4201, ///< not a catalog
    kDiagCatalogVersion   = 4202, ///< another major or minor version
    kDiagCatalogSizes     = 4203, ///< a size or offset is out of the file, misaligned or overlapping
    kDiagCatalogReserved  = 4204, ///< flags or reserved bytes are not zero
    kDiagCatalogName      = 4205, ///< a name is outside the strings or invalid, or a kind is unknown
    kDiagCatalogOrder     = 4206, ///< entries are not sorted by name, then kind
    kDiagCatalogDuplicate = 4207, ///< two entries have the same name and kind
    kDiagCatalogIndex     = 4208, ///< the index is unsorted, has a wrong hash or misses an entry
    kDiagCatalogChecksum  = 4209, ///< the checksum does not match
};

struct CatalogEntry {
    StrView name   = {}; ///< points into the catalog bytes
    AssetKind kind = AssetKind::Mesh;
    Hash128 key;      ///< the build key, which names the artifact
    Hash128 checksum; ///< xxh3_128 of the artifact's bytes
    u64 bytes = 0;    ///< the artifact's size
};

/// The target profile a catalog was cooked for.
struct CatalogProfile {
    StrView name     = {};
    u64 hash         = 0; ///< hash_target
    u64 blockFormats = 0; ///< block_format_bit() set
};

/// A validated catalog in caller memory. The bytes given to open() must outlive the view.
/// Copyable, no allocation; lookups are a binary search.
class KILN_API CatalogView {
public:
    CatalogView() noexcept = default;

    /// Checks everything: header, sizes, names, order, index and checksum. On failure, emits one
    /// K42xx diagnostic and returns Corrupt (VersionMismatch for K4202).
    [[nodiscard]] static Result<CatalogView> open(Span<u8 const> bytes, DiagSink const* diag = nullptr,
                                                  StrView where = {}) noexcept;

    [[nodiscard]] u64 size() const noexcept { return count_; }
    [[nodiscard]] CatalogEntry entry(u64 i) const noexcept;
    [[nodiscard]] CatalogProfile profile() const noexcept;
    /// The entry of `name` and `kind`, or false.
    [[nodiscard]] bool find(AssetKind kind, StrView name, CatalogEntry* out) const noexcept;

private:
    Span<u8 const> bytes_;
    u64 count_   = 0;
    u64 entries_ = 0; ///< section offsets
    u64 index_   = 0;
    u64 strings_ = 0;
};

/// Null if `name` is a valid profile name (`[a-z0-9_-]`, 1 to 63 characters), else a reason.
[[nodiscard]] KILN_API char const* check_profile_name(StrView name) noexcept;

/// `<storeDir>/catalogs/<profile>.kcat`. Returns what `format()` returns (>= cap - 1: truncated).
[[nodiscard]] KILN_API usize catalog_file_path(StrView storeDir, StrView profile, char* out,
                                               usize cap) noexcept;
/// `<storeDir>/artifacts/<2 hex digits>/<32 hex digits>.mesh|.ktx2`, as catalog_file_path().
[[nodiscard]] KILN_API usize artifact_file_path(StrView storeDir, AssetKind kind, Hash128 const& key,
                                                char* out, usize cap) noexcept;

} // namespace kiln
