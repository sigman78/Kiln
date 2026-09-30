// Store manifest reader, format 0.1 (docs/design/store-catalog.md). Every field is decoded from
// the bytes and every offset checked against the file size before use.
#include "formats_internal.h"

#include "kiln/manifest.h"

namespace kiln {

namespace {

template <class T> T rd(Span<u8 const> b, u64 off) noexcept { return read_unaligned<T>(b.data + off); }

Hash128 rd_hash(Span<u8 const> b, u64 off) noexcept {
    Hash128 h;
    std::memcpy(h.bytes, b.data + off, sizeof h.bytes);
    return h;
}

/// -1, 0, 1 as `a` sorts before, with or after `b`: bytes, then length.
int compare_names(StrView a, StrView b) noexcept {
    usize const n = min(a.size, b.size);
    if (int const c = n ? std::memcmp(a.data, b.data, n) : 0; c != 0) return c < 0 ? -1 : 1;
    return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}

bool all_zero(Span<u8 const> b, u64 off, u64 n) noexcept {
    for (u64 i = 0; i < n; ++i)
        if (b.data[off + i] != 0) return false;
    return true;
}

u64 align8(u64 v) noexcept { return (v + 7) & ~u64(7); }

/// A string of the string section, or false when it does not lie inside it.
bool string_at(Span<u8 const> b, u64 strings, u64 stringBytes, u32 off, u32 len, StrView* out) noexcept {
    if (u64(off) > stringBytes || u64(len) > stringBytes - off) return false;
    *out = StrView(reinterpret_cast<char const*>(b.data + strings + off), len);
    return true;
}

// Header fields.
constexpr u64 kTotal = 16, kProfileCount = 24, kEntryCount = 32, kProfiles = 40, kEntries = 48, kIndex = 56,
              kStrings = 64, kStringBytes = 72;

} // namespace

// A macro keeps the format string a literal for -Wformat-security.
#define KILN_MANIFEST_FAIL(code, diagCode, ...)                                                              \
    return diagf(diag, make_status(code), diagCode, Severity::Error, where, "manifest", __VA_ARGS__)

Result<ManifestView> ManifestView::open(Span<u8 const> b, DiagSink const* diag, StrView where) noexcept {
    if (b.size < kManifestHeaderBytes || rd<u32>(b, 0) != kManifestMagic)
        KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestMagic, "not a store manifest");
    u16 const major = rd<u16>(b, 4), minor = rd<u16>(b, 6);
    if (major != kManifestMajor || minor != kManifestMinor)
        KILN_MANIFEST_FAIL(Code::VersionMismatch, kDiagManifestVersion,
                           "version %u.%u, this reader reads %u.%u", unsigned(major), unsigned(minor),
                           unsigned(kManifestMajor), unsigned(kManifestMinor));
    if (rd<u32>(b, 8) != kManifestHeaderBytes || rd<u64>(b, kTotal) != b.size || (b.size & 7u) != 0)
        KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestSizes,
                           "header or file size does not match (%llu bytes)",
                           static_cast<unsigned long long>(b.size));

    // Sections follow each other in order: profiles, entries, index, strings, then padding to 8
    // bytes. Counts are bounded by the file first, so no product or sum below wraps.
    u64 const room  = b.size - kManifestHeaderBytes;
    u64 const pc    = rd<u32>(b, kProfileCount);
    u64 const ec    = rd<u64>(b, kEntryCount);
    u64 const pOff  = rd<u64>(b, kProfiles);
    u64 const eOff  = rd<u64>(b, kEntries);
    u64 const iOff  = rd<u64>(b, kIndex);
    u64 const sOff  = rd<u64>(b, kStrings);
    u64 const sSize = rd<u64>(b, kStringBytes);
    bool const fits = pc <= room / kManifestProfileBytes &&
                      ec <= room / (kManifestEntryBytes + kManifestIndexBytes) &&
                      pOff == kManifestHeaderBytes && eOff == pOff + pc * kManifestProfileBytes &&
                      iOff == eOff + ec * kManifestEntryBytes && sOff == iOff + ec * kManifestIndexBytes &&
                      sOff <= b.size && sSize <= b.size - sOff && align8(sOff + sSize) == b.size;
    if (!fits) KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestSizes, "sections do not fit the file");
    if (rd<u32>(b, 12) != 0 || rd<u32>(b, 28) != 0 || !all_zero(b, 96, 32) ||
        !all_zero(b, sOff + sSize, b.size - sOff - sSize))
        KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestReserved,
                           "flags, reserved or padding bytes are not zero");

    // Profiles: sorted and unique by name; their entry ranges tile the entries in order.
    StrView prevProfile;
    u64 next = 0;
    for (u64 p = 0; p < pc; ++p) {
        u64 const at = pOff + p * kManifestProfileBytes;
        StrView name;
        if (!string_at(b, sOff, sSize, rd<u32>(b, at), rd<u32>(b, at + 4), &name) || check_profile_name(name))
            KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestName, "profile %llu: bad name",
                               static_cast<unsigned long long>(p));
        if (p > 0) {
            int const c = compare_names(prevProfile, name);
            if (c > 0)
                KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestOrder, "profile %llu is out of order",
                                   static_cast<unsigned long long>(p));
            if (c == 0)
                KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestDuplicate, "profile '%.*s' is given twice",
                                   KILN_SV(name));
        }
        prevProfile     = name;
        u64 const first = rd<u64>(b, at + 24), count = rd<u64>(b, at + 32);
        if (first != next || count > ec - next)
            KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestSizes, "profile '%.*s': its entries do not fit",
                               KILN_SV(name));
        next += count;
    }
    if (next != ec) KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestSizes, "entries outside every profile");

    ManifestView v;
    v.bytes_    = b;
    v.profiles_ = u32(pc);

    for (u32 p = 0; p < u32(pc); ++p) {
        ManifestProfile const prof = v.profile(p);
        StrView prev;
        u16 prevKind = 0;
        for (u64 i = prof.first_; i < prof.first_ + prof.count_; ++i) {
            u64 const at = eOff + i * kManifestEntryBytes;
            StrView name;
            u16 const kind = rd<u16>(b, at + 8);
            if (!string_at(b, sOff, sSize, rd<u32>(b, at), rd<u32>(b, at + 4), &name) ||
                check_asset_name(name) || (kind != 1 && kind != 2))
                KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestName, "entry %llu: bad name or kind",
                                   static_cast<unsigned long long>(i));
            if (rd<u16>(b, at + 10) != 0 || rd<u32>(b, at + 12) != 0)
                KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestReserved,
                                   "entry %llu: flags or reserved bytes are not zero",
                                   static_cast<unsigned long long>(i));
            if (i > prof.first_) {
                int const c = compare_names(prev, name);
                if (c > 0 || (c == 0 && prevKind > kind))
                    KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestOrder, "entry %llu is out of order",
                                       static_cast<unsigned long long>(i));
                if (c == 0 && prevKind == kind)
                    KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestDuplicate, "entry %llu repeats '%.*s'",
                                       static_cast<unsigned long long>(i), KILN_SV(name));
            }
            prev     = name;
            prevKind = kind;
        }
        // A record's hash follows from its entry, so strictly rising (hash, entry) pairs give
        // distinct entries: `count` of them inside the range is every entry once.
        for (u64 i = prof.first_; i < prof.first_ + prof.count_; ++i) {
            u64 const at = iOff + i * kManifestIndexBytes;
            u64 const h = rd<u64>(b, at), e = rd<u64>(b, at + 8);
            bool ok = e >= prof.first_ && e < prof.first_ + prof.count_ &&
                      h == hash_name(prof.entry(e - prof.first_).name);
            if (ok && i > prof.first_) {
                u64 const ph = rd<u64>(b, at - kManifestIndexBytes),
                          pe = rd<u64>(b, at - kManifestIndexBytes + 8);
                ok           = ph < h || (ph == h && pe < e);
            }
            if (!ok)
                KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestIndex, "index record %llu is wrong",
                                   static_cast<unsigned long long>(i));
        }
    }
    if (!(rd_hash(b, kManifestChecksumOffset) == fmt::xxh3_128_zeroed(b, kManifestChecksumOffset, 16)))
        KILN_MANIFEST_FAIL(Code::Corrupt, kDiagManifestChecksum, "checksum does not match");
    return v;
}

#undef KILN_MANIFEST_FAIL

ManifestProfile ManifestView::profile(u32 i) const noexcept {
    KILN_ASSERT(i < profiles_);
    u64 const pOff = rd<u64>(bytes_, kProfiles), at = pOff + u64(i) * kManifestProfileBytes;
    ManifestProfile p;
    p.bytes_        = bytes_;
    p.entries_      = rd<u64>(bytes_, kEntries);
    p.index_        = rd<u64>(bytes_, kIndex);
    p.strings_      = rd<u64>(bytes_, kStrings);
    p.name_         = StrView(reinterpret_cast<char const*>(bytes_.data + p.strings_ + rd<u32>(bytes_, at)),
                              rd<u32>(bytes_, at + 4));
    p.hash_         = rd<u64>(bytes_, at + 8);
    p.blockFormats_ = rd<u64>(bytes_, at + 16);
    p.first_        = rd<u64>(bytes_, at + 24);
    p.count_        = rd<u64>(bytes_, at + 32);
    return p;
}

bool ManifestView::find_profile(StrView name, ManifestProfile* out) const noexcept {
    u32 lo = 0, hi = profiles_;
    while (lo < hi) {
        u32 const mid           = lo + (hi - lo) / 2;
        ManifestProfile const p = profile(mid);
        int const c             = compare_names(p.name(), name);
        if (c == 0) {
            *out = p;
            return true;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

Hash128 ManifestView::checksum() const noexcept {
    return bytes_.empty() ? Hash128{} : rd_hash(bytes_, kManifestChecksumOffset);
}

ManifestEntry ManifestProfile::entry(u64 i) const noexcept {
    KILN_ASSERT(i < count_);
    u64 const at = entries_ + (first_ + i) * kManifestEntryBytes;
    ManifestEntry e;
    e.name     = StrView(reinterpret_cast<char const*>(bytes_.data + strings_ + rd<u32>(bytes_, at)),
                         rd<u32>(bytes_, at + 4));
    e.kind     = rd<u16>(bytes_, at + 8) == 1 ? AssetKind::Mesh : AssetKind::Texture;
    e.key      = rd_hash(bytes_, at + 16);
    e.checksum = rd_hash(bytes_, at + 32);
    e.bytes    = rd<u64>(bytes_, at + 48);
    return e;
}

bool ManifestProfile::find(AssetKind kind, StrView name, ManifestEntry* out) const noexcept {
    u64 const h = hash_name(name);
    u64 lo = first_, hi = first_ + count_;
    while (lo < hi) {
        u64 const mid = lo + (hi - lo) / 2;
        if (rd<u64>(bytes_, index_ + mid * kManifestIndexBytes) < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < first_ + count_ && rd<u64>(bytes_, index_ + lo * kManifestIndexBytes) == h; ++lo) {
        ManifestEntry const e = entry(rd<u64>(bytes_, index_ + lo * kManifestIndexBytes + 8) - first_);
        if (e.kind == kind && e.name == name) {
            *out = e;
            return true;
        }
    }
    return false;
}

char const* check_profile_name(StrView name) noexcept {
    if (name.empty() || name.size > 63) return "1 to 63 characters";
    for (char const c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return "only a-z, 0-9, '_' and '-'";
    return nullptr;
}

usize manifest_file_path(StrView storeDir, char* out, usize cap) noexcept {
    return format(out, cap, "%.*s/%s", KILN_SV(storeDir), kManifestFile);
}

usize artifact_file_path(StrView storeDir, Hash128 const& key, char* out, usize cap) noexcept {
    char b32[27];
    hash128_base32(key, b32);
    return format(out, cap, "%.*s/%s", KILN_SV(storeDir), b32);
}

} // namespace kiln
