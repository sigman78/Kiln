// kiln/manifest.h — XXH3-128 hashes and the store manifest, format 0.1: for each target profile, a
// sorted map from asset names to the build keys of their artifacts (docs/design/store-manifest.md).
#pragma once

#include "kiln/assets.h"

namespace kiln {

/// XXH3-128 of `bytes`, seed 0. A checksum, not a signature.
KILN_API Hash128 xxh3_128(Span<u8 const> bytes) noexcept;

/// 32 lowercase hex digits and a NUL into `out`.
KILN_API void hash128_hex(Hash128 const& h, char (&out)[33]) noexcept;
/// 26 base32 characters (RFC 4648 alphabet in lower case, `a-z2-7`, no padding) and a NUL into
/// `out`: the bytes in order, 5 bits at a time from the top bit; the last character holds 3 bits.
KILN_API void hash128_base32(Hash128 const& h, char (&out)[27]) noexcept;

// ---------------------------------------------------------------------------
// Store layout, flat: <store>/manifest.dir (the runtime reads it) and each artifact at <store>/<26>,
// the build key in base32 (the key covers the kind, so names never clash; the manifest gives it);
// the cook also keeps <store>/manifest.in (input records) and <store>/manifest.lock (one writer).
// ---------------------------------------------------------------------------

inline constexpr char kManifestFile[] = "manifest.dir";

/// `<storeDir>/manifest.dir`. Returns what `format()` returns (>= cap - 1: truncated).
[[nodiscard]] KILN_API usize manifest_file_path(StrView storeDir, char* out, usize cap) noexcept;
/// `<storeDir>/<hash128_base32(key)>`, no extension, as manifest_file_path().
[[nodiscard]] KILN_API usize artifact_file_path(StrView storeDir, Hash128 const& key, char* out,
                                                usize cap) noexcept;
/// Null if `name` is a valid profile name (`[a-z0-9_-]`, 1 to 63 characters), else a reason.
KILN_API char const* check_profile_name(StrView name) noexcept;

// ---------------------------------------------------------------------------
// Manifest format 0.1 (little-endian; sections at 8-byte boundaries)
// ---------------------------------------------------------------------------

inline constexpr u32 kManifestMagic        = fourcc('K', 'M', 'A', 'N');
inline constexpr u16 kManifestMajor        = 0;
inline constexpr u16 kManifestMinor        = 1;
inline constexpr u32 kManifestHeaderBytes  = 128;
inline constexpr u32 kManifestProfileBytes = 40;
inline constexpr u32 kManifestEntryBytes   = 56;
inline constexpr u32 kManifestIndexBytes   = 16;
/// Byte offset of the checksum in the header; the checksum hashes the file with it zeroed.
inline constexpr u32 kManifestChecksumOffset = 80;

/// Diagnostic codes of a malformed manifest (K4201-K4209).
enum ManifestDiagCode : u32 {
    kDiagManifestMagic     = 4201, ///< not a manifest
    kDiagManifestVersion   = 4202, ///< another major or minor version
    kDiagManifestSizes     = 4203, ///< a size, count or offset is out of the file, misaligned or overlapping
    kDiagManifestReserved  = 4204, ///< flags, reserved or padding bytes are not zero
    kDiagManifestName      = 4205, ///< a name is outside the strings or invalid, or a kind is unknown
    kDiagManifestOrder     = 4206, ///< profiles or a profile's entries are out of order
    kDiagManifestDuplicate = 4207, ///< a profile name twice, or a name and kind twice in one profile
    kDiagManifestIndex     = 4208, ///< an index range is unsorted, has a wrong hash or misses an entry
    kDiagManifestChecksum  = 4209, ///< the checksum does not match
};

struct ManifestEntry {
    StrView name   = {}; ///< points into the manifest bytes
    AssetKind kind = AssetKind::Mesh;
    Hash128 key;      ///< the build key, which names the artifact
    Hash128 checksum; ///< xxh3_128 of the artifact's bytes
    u64 bytes = 0;    ///< the artifact's size
};

/// The entries of one target profile in a validated manifest. Copyable, no allocation; lookups
/// are a binary search. Valid while the manifest's bytes are.
class KILN_API ManifestProfile {
public:
    ManifestProfile() noexcept = default;

    StrView name() const noexcept { return name_; }
    u64 hash() const noexcept { return hash_; }                  ///< hash_target
    u64 block_formats() const noexcept { return blockFormats_; } ///< block_format_bit() set
    u64 size() const noexcept { return count_; }
    ManifestEntry entry(u64 i) const noexcept;
    /// The entry of `name` and `kind`, or false.
    [[nodiscard]] bool find(AssetKind kind, StrView name, ManifestEntry* out) const noexcept;

private:
    friend class ManifestView;
    Span<u8 const> bytes_;
    u64 entries_ = 0, index_ = 0, strings_ = 0; ///< section offsets
    u64 first_ = 0, count_ = 0;                 ///< this profile's entries
    StrView name_;
    u64 hash_ = 0, blockFormats_ = 0;
};

/// A validated manifest in caller memory. The bytes given to open() must outlive the view and its
/// profiles. Copyable, no allocation.
class KILN_API ManifestView {
public:
    ManifestView() noexcept = default;

    /// Checks everything: header, sizes, names, order, index and checksum. On failure, emits one
    /// K42xx diagnostic and returns Corrupt (VersionMismatch for K4202).
    static Result<ManifestView> open(Span<u8 const> bytes, DiagSink const* diag = nullptr,
                                     StrView where = {}) noexcept;

    u32 profile_count() const noexcept { return profiles_; }
    ManifestProfile profile(u32 i) const noexcept;
    /// The profile called `name`, or false.
    [[nodiscard]] bool find_profile(StrView name, ManifestProfile* out) const noexcept;
    Hash128 checksum() const noexcept;

private:
    Span<u8 const> bytes_;
    u32 profiles_ = 0;
};

} // namespace kiln
