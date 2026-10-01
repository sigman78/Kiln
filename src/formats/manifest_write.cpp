// Store manifest writer, format 0.1 (docs/design/store-manifest.md). The same profiles and entries
// give the same bytes, whatever their order.
#include "formats_internal.h"

#include "kiln/cook/manifest.h"

#include <algorithm>

namespace kiln::cook {

namespace {

template <class T> void wr(Vec<u8>& b, u64 off, T v) { write_unaligned<T>(b.data() + off, v); }

bool name_less(StrView a, StrView b) {
    usize const n = min(a.size, b.size);
    int const c   = n ? std::memcmp(a.data, b.data, n) : 0;
    return c != 0 ? c < 0 : a.size < b.size;
}

u16 kind_code(AssetKind k) { return k == AssetKind::Mesh ? 1 : 2; }

u64 align8(u64 v) { return (v + 7) & ~u64(7); }

struct IndexRec {
    u64 hash, entry;
};

} // namespace

Status write_manifest(ManifestDesc const& d, Vec<u8>* out, DiagSink const* diag) {
    Allocator const* alloc = out->allocator();
    usize const pc         = d.profiles.size;

    Vec<u32> porder(alloc, Tag::Cook);
    porder.resize(pc);
    for (usize i = 0; i < pc; ++i)
        porder[i] = u32(i);
    std::sort(porder.begin(), porder.end(),
              [&d](u32 a, u32 b) { return name_less(d.profiles[a].name, d.profiles[b].name); });

    // Every profile's entries sorted by (name, kind), one after another in profile order.
    Vec<ManifestEntry const*> entries(alloc, Tag::Cook);
    u64 stringBytes = 0;
    for (usize p = 0; p < pc; ++p) {
        ManifestProfileDesc const& prof = d.profiles[porder[p]];
        if (char const* why = check_profile_name(prof.name))
            return diagf(diag, make_status(Code::InvalidArgument), kDiagManifestName, Severity::Error,
                         prof.name, "manifest", "bad profile name: %s", why);
        if (p > 0 && prof.name == d.profiles[porder[p - 1]].name)
            return diagf(diag, make_status(Code::InvalidArgument), kDiagManifestDuplicate, Severity::Error,
                         prof.name, "manifest", "the profile is given twice");
        stringBytes += prof.name.size;
        usize const first = entries.size();
        for (ManifestEntry const& e : prof.entries)
            entries.push_back(&e);
        std::sort(entries.begin() + first, entries.end(), [](ManifestEntry const* x, ManifestEntry const* y) {
            if (x->name != y->name) return name_less(x->name, y->name);
            return kind_code(x->kind) < kind_code(y->kind);
        });
        for (usize i = first; i < entries.size(); ++i) {
            ManifestEntry const& e = *entries[i];
            if (char const* why = check_asset_name(e.name))
                return diagf(diag, make_status(Code::InvalidArgument), kDiagManifestName, Severity::Error,
                             e.name, "manifest", "bad asset name: %s", why);
            if (i > first && e.name == entries[i - 1]->name && e.kind == entries[i - 1]->kind)
                return diagf(diag, make_status(Code::InvalidArgument), kDiagManifestDuplicate,
                             Severity::Error, e.name, "manifest", "the name is given twice in profile '%.*s'",
                             KILN_SV(prof.name));
            stringBytes += e.name.size;
        }
    }
    if (stringBytes > 0xFFFFFFFFull) return make_status(Code::InvalidArgument);

    u64 const ec    = entries.size();
    u64 const pOff  = kManifestHeaderBytes;
    u64 const eOff  = pOff + u64(pc) * kManifestProfileBytes;
    u64 const iOff  = eOff + ec * kManifestEntryBytes;
    u64 const sOff  = iOff + ec * kManifestIndexBytes;
    u64 const total = align8(sOff + stringBytes);
    out->clear();
    out->resize(usize(total), u8(0));

    wr<u32>(*out, 0, kManifestMagic);
    wr<u16>(*out, 4, kManifestMajor);
    wr<u16>(*out, 6, kManifestMinor);
    wr<u32>(*out, 8, kManifestHeaderBytes);
    wr<u64>(*out, 16, total);
    wr<u32>(*out, 24, u32(pc));
    wr<u64>(*out, 32, ec);
    wr<u64>(*out, 40, pOff);
    wr<u64>(*out, 48, eOff);
    wr<u64>(*out, 56, iOff);
    wr<u64>(*out, 64, sOff);
    wr<u64>(*out, 72, stringBytes);

    u64 str               = 0;
    auto const put_string = [&](StrView s) {
        u64 const at = str;
        if (s.size) std::memcpy(out->data() + sOff + str, s.data, s.size);
        str += s.size;
        return u32(at);
    };
    Vec<IndexRec> idx(alloc, Tag::Cook);
    idx.resize(usize(ec));
    u64 first = 0;
    for (usize p = 0; p < pc; ++p) {
        ManifestProfileDesc const& prof = d.profiles[porder[p]];
        u64 const at                    = pOff + u64(p) * kManifestProfileBytes;
        wr<u32>(*out, at, put_string(prof.name));
        wr<u32>(*out, at + 4, u32(prof.name.size));
        wr<u64>(*out, at + 8, prof.hash);
        wr<u64>(*out, at + 16, prof.blockFormats);
        wr<u64>(*out, at + 24, first);
        wr<u64>(*out, at + 32, u64(prof.entries.size));
        for (u64 i = first; i < first + prof.entries.size; ++i) {
            ManifestEntry const& e = *entries[usize(i)];
            u64 const eat          = eOff + i * kManifestEntryBytes;
            wr<u32>(*out, eat, put_string(e.name));
            wr<u32>(*out, eat + 4, u32(e.name.size));
            wr<u16>(*out, eat + 8, kind_code(e.kind));
            std::memcpy(out->data() + eat + 16, e.key.bytes, 16);
            std::memcpy(out->data() + eat + 32, e.checksum.bytes, 16);
            wr<u64>(*out, eat + 48, e.bytes);
            idx[usize(i)] = {hash_name(e.name), i};
        }
        std::sort(idx.begin() + first, idx.begin() + first + prof.entries.size,
                  [](IndexRec const& a, IndexRec const& b) {
                      return a.hash != b.hash ? a.hash < b.hash : a.entry < b.entry;
                  });
        first += prof.entries.size;
    }
    for (u64 i = 0; i < ec; ++i) {
        wr<u64>(*out, iOff + i * kManifestIndexBytes, idx[usize(i)].hash);
        wr<u64>(*out, iOff + i * kManifestIndexBytes + 8, idx[usize(i)].entry);
    }

    Hash128 const sum =
        fmt::xxh3_128_zeroed(Span<u8 const>(out->data(), out->size()), kManifestChecksumOffset, 16);
    std::memcpy(out->data() + kManifestChecksumOffset, sum.bytes, 16);
    return kOk;
}

} // namespace kiln::cook
