// Store catalog writer, format 0.1 (docs/design/store-catalog.md). The same entries give the
// same bytes, whatever their order.
#include "formats_internal.h"

#include "kiln/cook/catalog.h"

#include <algorithm>

namespace kiln::cook {

namespace {

template <class T> void wr(Vec<u8>& b, u64 off, T v) noexcept { write_unaligned<T>(b.data() + off, v); }

bool name_less(StrView a, StrView b) noexcept {
    usize const n = min(a.size, b.size);
    int const c   = n ? std::memcmp(a.data, b.data, n) : 0;
    return c != 0 ? c < 0 : a.size < b.size;
}

u16 kind_code(AssetKind k) noexcept { return k == AssetKind::Mesh ? 1 : 2; }

} // namespace

Status write_catalog(CatalogDesc const& d, Vec<u8>* out, DiagSink const* diag) noexcept {
    if (char const* why = check_profile_name(d.profile.name))
        return diagf(diag, make_status(Code::InvalidArgument), kDiagCatalogName, Severity::Error,
                     d.profile.name, "catalog", "bad profile name: %s", why);
    Allocator const* alloc = out->allocator();
    usize const n          = d.entries.size;

    Vec<u32> order(alloc, Tag::Cook);
    order.resize(n);
    for (usize i = 0; i < n; ++i)
        order[i] = u32(i);
    std::sort(order.begin(), order.end(), [&d](u32 a, u32 b) noexcept {
        CatalogEntry const &x = d.entries[a], &y = d.entries[b];
        if (x.name != y.name) return name_less(x.name, y.name);
        return kind_code(x.kind) < kind_code(y.kind);
    });

    u64 stringBytes = d.profile.name.size;
    for (usize i = 0; i < n; ++i) {
        CatalogEntry const& e = d.entries[order[i]];
        if (char const* why = check_asset_name(e.name))
            return diagf(diag, make_status(Code::InvalidArgument), kDiagCatalogName, Severity::Error, e.name,
                         "catalog", "bad asset name: %s", why);
        if (i > 0 && e.name == d.entries[order[i - 1]].name && e.kind == d.entries[order[i - 1]].kind)
            return diagf(diag, make_status(Code::InvalidArgument), kDiagCatalogDuplicate, Severity::Error,
                         e.name, "catalog", "the name is given twice");
        stringBytes += e.name.size;
    }
    if (stringBytes > 0xFFFFFFFFull) return make_status(Code::InvalidArgument);

    u64 const entries = kCatalogHeaderBytes;
    u64 const index   = entries + u64(n) * kCatalogEntryBytes;
    u64 const strings = index + u64(n) * kCatalogIndexBytes;
    u64 const total   = (strings + stringBytes + 7) & ~u64(7);
    out->clear();
    out->resize(usize(total), u8(0));

    wr<u32>(*out, 0, kCatalogMagic);
    wr<u16>(*out, 4, kCatalogMajor);
    wr<u16>(*out, 6, kCatalogMinor);
    wr<u32>(*out, 8, kCatalogHeaderBytes);
    wr<u64>(*out, 16, total);
    wr<u64>(*out, 24, u64(n));
    wr<u64>(*out, 32, entries);
    wr<u64>(*out, 40, index);
    wr<u64>(*out, 48, strings);
    wr<u64>(*out, 56, stringBytes);
    wr<u64>(*out, 64, d.profile.hash);
    wr<u64>(*out, 72, d.profile.blockFormats);
    wr<u32>(*out, 80, 0);
    wr<u32>(*out, 84, u32(d.profile.name.size));
    std::memcpy(out->data() + strings, d.profile.name.data, d.profile.name.size);

    struct IndexRec {
        u64 hash, entry;
    };
    Vec<IndexRec> idx(alloc, Tag::Cook);
    idx.resize(n);
    u64 str = d.profile.name.size;
    for (usize i = 0; i < n; ++i) {
        CatalogEntry const& e = d.entries[order[i]];
        u64 const at          = entries + u64(i) * kCatalogEntryBytes;
        wr<u32>(*out, at, u32(str));
        wr<u32>(*out, at + 4, u32(e.name.size));
        wr<u16>(*out, at + 8, kind_code(e.kind));
        std::memcpy(out->data() + at + 16, e.key.bytes, 16);
        std::memcpy(out->data() + at + 32, e.checksum.bytes, 16);
        wr<u64>(*out, at + 48, e.bytes);
        std::memcpy(out->data() + strings + str, e.name.data, e.name.size);
        str += e.name.size;
        idx[i] = {hash_name(e.name), u64(i)};
    }
    std::sort(idx.begin(), idx.end(), [](IndexRec const& a, IndexRec const& b) noexcept {
        return a.hash != b.hash ? a.hash < b.hash : a.entry < b.entry;
    });
    for (usize i = 0; i < n; ++i) {
        wr<u64>(*out, index + u64(i) * kCatalogIndexBytes, idx[i].hash);
        wr<u64>(*out, index + u64(i) * kCatalogIndexBytes + 8, idx[i].entry);
    }

    Hash128 const sum =
        fmt::xxh3_128_zeroed(Span<u8 const>(out->data(), out->size()), kCatalogChecksumOffset, 16);
    std::memcpy(out->data() + kCatalogChecksumOffset, sum.bytes, 16);
    return kOk;
}

} // namespace kiln::cook
