// fuzz/fuzz_manifest_read.cpp — libFuzzer target: ManifestView::open on arbitrary bytes, then every
// profile, entry and lookup. The checksum is patched in first, so the other checks run too.
#include "../src/formats/formats_internal.h"

#include <kiln/containers.h>
#include <kiln/manifest.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    Vec<u8> bytes(default_allocator(), Tag::Test);
    bytes.append(Span<u8 const>(data, size));
    if (size >= kManifestHeaderBytes) {
        Hash128 const h =
            fmt::xxh3_128_zeroed(Span<u8 const>(bytes.data(), bytes.size()), kManifestChecksumOffset, 16);
        std::memcpy(bytes.data() + kManifestChecksumOffset, h.bytes, 16);
    }
    Result<ManifestView> const r = ManifestView::open(Span<u8 const>(bytes.data(), bytes.size()));
    if (r.failed()) return 0;
    ManifestView const& v = r.value();
    for (u32 p = 0; p < v.profile_count(); ++p) {
        ManifestProfile const prof = v.profile(p);
        ManifestProfile found;
        if (!v.find_profile(prof.name(), &found) || found.size() != prof.size()) __builtin_trap();
        for (u64 i = 0; i < prof.size(); ++i) {
            ManifestEntry const e = prof.entry(i);
            ManifestEntry again;
            if (!prof.find(e.kind, e.name, &again) || !(again.key == e.key)) __builtin_trap();
        }
    }
    return 0;
}
