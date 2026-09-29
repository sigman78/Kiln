// fuzz/fuzz_catalog_read.cpp — libFuzzer target: CatalogView::open on arbitrary bytes, then every
// entry and a lookup of each name. The checksum is patched in first, so the other checks run too.
#include "../src/formats/formats_internal.h"

#include <kiln/catalog.h>
#include <kiln/containers.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    Vec<u8> bytes(default_allocator(), Tag::Test);
    bytes.append(Span<u8 const>(data, size));
    if (size >= kCatalogHeaderBytes) {
        Hash128 const h =
            fmt::xxh3_128_zeroed(Span<u8 const>(bytes.data(), bytes.size()), kCatalogChecksumOffset, 16);
        std::memcpy(bytes.data() + kCatalogChecksumOffset, h.bytes, 16);
    }
    Result<CatalogView> const r = CatalogView::open(Span<u8 const>(bytes.data(), bytes.size()));
    if (r.failed()) return 0;
    CatalogView const& v = r.value();
    (void)v.profile();
    for (u64 i = 0; i < v.size(); ++i) {
        CatalogEntry const e = v.entry(i);
        CatalogEntry found;
        if (!v.find(e.kind, e.name, &found) || !(found.key == e.key)) __builtin_trap();
    }
    return 0;
}
