// Store catalog reader, format 0.1 (docs/design/store-catalog.md). Every field is decoded from
// the bytes and every offset checked against the file size before use.
#include "formats_internal.h"

#include "kiln/catalog.h"

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

bool is_aligned(u64 v) noexcept { return (v & 7u) == 0; }

/// A string of the string section, or false when it does not lie inside it.
bool string_at(Span<u8 const> b, u64 strings, u64 stringBytes, u32 off, u32 len, StrView* out) noexcept {
    if (u64(off) > stringBytes || u64(len) > stringBytes - off) return false;
    *out = StrView(reinterpret_cast<char const*>(b.data + strings + off), len);
    return true;
}

} // namespace

// A macro keeps the format string a literal for -Wformat-security.
#define KILN_CATALOG_FAIL(code, diagCode, ...)                                                               \
    return diagf(diag, make_status(code), diagCode, Severity::Error, where, "catalog", __VA_ARGS__)

Result<CatalogView> CatalogView::open(Span<u8 const> b, DiagSink const* diag, StrView where) noexcept {
    if (b.size < kCatalogHeaderBytes || rd<u32>(b, 0) != kCatalogMagic)
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogMagic, "not a store catalog");
    u16 const major = rd<u16>(b, 4), minor = rd<u16>(b, 6);
    if (major != kCatalogMajor || minor != kCatalogMinor)
        KILN_CATALOG_FAIL(Code::VersionMismatch, kDiagCatalogVersion,
                          "version %u.%u, this reader reads %u.%u", unsigned(major), unsigned(minor),
                          unsigned(kCatalogMajor), unsigned(kCatalogMinor));

    u64 const total = rd<u64>(b, 16), count = rd<u64>(b, 24);
    u64 const entries = rd<u64>(b, 32), index = rd<u64>(b, 40);
    u64 const strings = rd<u64>(b, 48), stringBytes = rd<u64>(b, 56);
    if (rd<u32>(b, 8) != kCatalogHeaderBytes || total != b.size)
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogSizes, "header or file size does not match (%llu bytes)",
                          static_cast<unsigned long long>(b.size));
    // Sections follow each other in order: entries, index, strings, then at most 7 bytes of padding.
    u64 const maxCount = b.size / (kCatalogEntryBytes + kCatalogIndexBytes);
    if (count > maxCount || entries != kCatalogHeaderBytes || index != entries + count * kCatalogEntryBytes ||
        strings != index + count * kCatalogIndexBytes || stringBytes > b.size - strings ||
        b.size - strings - stringBytes > 7 || !is_aligned(b.size) || !is_aligned(index) ||
        !is_aligned(strings))
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogSizes, "sections do not fit the file");
    if (rd<u32>(b, 12) != 0 || !all_zero(b, 104, 24) ||
        !all_zero(b, strings + stringBytes, b.size - strings - stringBytes))
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogReserved,
                          "flags, reserved or padding bytes are not zero");

    StrView profile;
    if (!string_at(b, strings, stringBytes, rd<u32>(b, 80), rd<u32>(b, 84), &profile) ||
        check_profile_name(profile))
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogName, "bad profile name");

    CatalogView v;
    v.bytes_   = b;
    v.count_   = count;
    v.entries_ = entries;
    v.index_   = index;
    v.strings_ = strings;

    StrView prev;
    u16 prevKind = 0;
    for (u64 i = 0; i < count; ++i) {
        u64 const at = entries + i * kCatalogEntryBytes;
        StrView name;
        u16 const kind = rd<u16>(b, at + 8);
        if (!string_at(b, strings, stringBytes, rd<u32>(b, at), rd<u32>(b, at + 4), &name) ||
            check_asset_name(name) || (kind != 1 && kind != 2))
            KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogName, "entry %llu: bad name or kind",
                              static_cast<unsigned long long>(i));
        if (rd<u16>(b, at + 10) != 0 || rd<u32>(b, at + 12) != 0)
            KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogReserved,
                              "entry %llu: flags or reserved bytes are not zero",
                              static_cast<unsigned long long>(i));
        if (i > 0) {
            int const c = compare_names(prev, name);
            if (c > 0 || (c == 0 && prevKind > kind))
                KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogOrder, "entry %llu is out of order",
                                  static_cast<unsigned long long>(i));
            if (c == 0 && prevKind == kind)
                KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogDuplicate, "entry %llu repeats '%.*s'",
                                  static_cast<unsigned long long>(i), KILN_SV(name));
        }
        prev     = name;
        prevKind = kind;
    }

    // A record's hash follows from its entry, so strictly rising (hash, entry) pairs give
    // distinct entries: `count` of them below `count` is every entry once.
    for (u64 i = 0; i < count; ++i) {
        u64 const at = index + i * kCatalogIndexBytes;
        u64 const h = rd<u64>(b, at), e = rd<u64>(b, at + 8);
        bool ok = e < count && h == hash_name(v.entry(e).name);
        if (ok && i > 0) {
            u64 const ph = rd<u64>(b, at - kCatalogIndexBytes), pe = rd<u64>(b, at - kCatalogIndexBytes + 8);
            ok = ph < h || (ph == h && pe < e);
        }
        if (!ok)
            KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogIndex, "index record %llu is wrong",
                              static_cast<unsigned long long>(i));
    }
    if (!(rd_hash(b, kCatalogChecksumOffset) == fmt::xxh3_128_zeroed(b, kCatalogChecksumOffset, 16)))
        KILN_CATALOG_FAIL(Code::Corrupt, kDiagCatalogChecksum, "checksum does not match");
    return v;
}

#undef KILN_CATALOG_FAIL

CatalogEntry CatalogView::entry(u64 i) const noexcept {
    KILN_ASSERT(i < count_);
    u64 const at = entries_ + i * kCatalogEntryBytes;
    CatalogEntry e;
    e.name     = StrView(reinterpret_cast<char const*>(bytes_.data + strings_ + rd<u32>(bytes_, at)),
                         rd<u32>(bytes_, at + 4));
    e.kind     = rd<u16>(bytes_, at + 8) == 1 ? AssetKind::Mesh : AssetKind::Texture;
    e.key      = rd_hash(bytes_, at + 16);
    e.checksum = rd_hash(bytes_, at + 32);
    e.bytes    = rd<u64>(bytes_, at + 48);
    return e;
}

CatalogProfile CatalogView::profile() const noexcept {
    if (bytes_.empty()) return {};
    return {.name = StrView(reinterpret_cast<char const*>(bytes_.data + strings_ + rd<u32>(bytes_, 80)),
                            rd<u32>(bytes_, 84)),
            .hash = rd<u64>(bytes_, 64),
            .blockFormats = rd<u64>(bytes_, 72)};
}

bool CatalogView::find(AssetKind kind, StrView name, CatalogEntry* out) const noexcept {
    u64 const h = hash_name(name);
    u64 lo = 0, hi = count_;
    while (lo < hi) {
        u64 const mid = lo + (hi - lo) / 2;
        if (rd<u64>(bytes_, index_ + mid * kCatalogIndexBytes) < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < count_ && rd<u64>(bytes_, index_ + lo * kCatalogIndexBytes) == h; ++lo) {
        CatalogEntry const e = entry(rd<u64>(bytes_, index_ + lo * kCatalogIndexBytes + 8));
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

usize catalog_file_path(StrView storeDir, StrView profile, char* out, usize cap) noexcept {
    return format(out, cap, "%.*s/catalogs/%.*s.kcat", KILN_SV(storeDir), KILN_SV(profile));
}

usize artifact_file_path(StrView storeDir, AssetKind kind, Hash128 const& key, char* out,
                         usize cap) noexcept {
    char hex[33];
    hash128_hex(key, hex);
    return format(out, cap, "%.*s/artifacts/%.2s/%s.%s", KILN_SV(storeDir), hex, hex,
                  kind == AssetKind::Mesh ? "mesh" : "ktx2");
}

} // namespace kiln
