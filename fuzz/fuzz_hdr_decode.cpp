// fuzz/fuzz_hdr_decode.cpp — libFuzzer target: kiln's own Radiance .hdr decoder. The PNG, JPEG and
// WebP decoders are wuffs, which is fuzzed upstream.
#include <kiln/cook/image.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    Result<cook::Image> const r = cook::decode_hdr(Span<u8 const>(data, size), default_allocator());
    if (r.ok() && r.value().pixels.size() != r.value().byte_size()) __builtin_trap();
    return 0;
}
