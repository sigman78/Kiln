// fuzz/fuzz_ktx2_read.cpp — libFuzzer target: Ktx2View::open on arbitrary bytes, then every
// accessor a loader uses on what it accepted.
#include <kiln/ktx2.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    Result<ktx2::Ktx2View> const r = ktx2::Ktx2View::open(Span<u8 const>(data, size));
    if (r.failed()) return 0;
    ktx2::Ktx2View const& v = r.value();
    (void)ktx2::Ktx2View::metadata_size(v.header());
    (void)v.has_all_level_data();
    for (u32 l = 0; l < v.levels().size; ++l) {
        (void)v.level_image_bytes(l);
        Span<u8 const> const d = v.level_data(l);
        if (d.size) (void)(d[0] + d[d.size - 1]); // the span must lie inside the input
    }
    return 0;
}
