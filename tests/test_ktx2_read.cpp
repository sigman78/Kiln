// tests/test_ktx2_read.cpp — reader-only KTX2 tests; the shipping (KILN_BUILD_COOK=OFF) build runs them.
// No kiln/cook/ includes and no ktx2::write(): every file is hand-built byte by byte.
// Writer round trips and DFD/KVD error paths live in test_ktx2.cpp.
#include "kiln_test.h"

#include "kiln/containers.h"
#include "kiln/ktx2.h"

using namespace kiln;
using namespace kiln::ktx2;

namespace {

static_assert(sizeof(Header) == 80);
static_assert(sizeof(LevelIndex) == 24);
static_assert(sizeof(kIdentifier) == 12);
static_assert(kIdentifier[0] == 0xAB && kIdentifier[1] == 'K' && kIdentifier[7] == 0xBB &&
              kIdentifier[11] == '\n');
static_assert(offsetof(Header, vkFormat) == 12);
static_assert(offsetof(Header, supercompressionScheme) == 44);
static_assert(offsetof(Header, sgdByteOffset) == 64);

/// Enum -> integer for KILN_CHECK_EQ messages.
template <class E> u32 ev(E e) { return u32(e); }

struct DiagCapture {
    u32 code          = 0;
    Severity severity = Severity::Info;
    int count         = 0;

    static void fn(void* user, Diagnostic const& d) {
        auto* self     = static_cast<DiagCapture*>(user);
        self->code     = d.code;
        self->severity = d.severity;
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

void put_u32(Vec<u8>& v, usize off, u32 x) { write_unaligned<u32>(v.data() + off, x); }
void put_u64(Vec<u8>& v, usize off, u64 x) { write_unaligned<u64>(v.data() + off, x); }
u64 get_u64(Vec<u8> const& v, usize off) { return read_unaligned<u64>(v.data() + off); }

// Level index entry i starts at 80 + 24 * i: byteOffset, byteLength, uncompressedByteLength.
constexpr usize level_field(u32 i, u32 field) { return 80 + 24 * usize(i) + 8 * usize(field); }

/// Minimal R8G8B8A8_UNORM file: header, `levelCount` level entries, a 44-byte DFD
/// (basic block + one sample; the reader does not parse samples), empty KVD, then
/// level data. `offsets` gives each level's byteOffset.
Vec<u8> hand_built(u32 w, u32 h, u32 levelCount, u32 const* offsets, u32 total) {
    Vec<u8> f(default_allocator(), Tag::Test);
    f.resize(total);
    u32 const entries = max(levelCount, 1u);
    std::memcpy(f.data(), kIdentifier, 12);
    put_u32(f, 12, 37); // VK_FORMAT_R8G8B8A8_UNORM
    put_u32(f, 16, 1);  // typeSize
    put_u32(f, 20, w);
    put_u32(f, 24, h);
    put_u32(f, 28, 0);
    put_u32(f, 32, 0);
    put_u32(f, 36, 1);
    put_u32(f, 40, levelCount);
    put_u32(f, 44, 0);
    u32 const dfdOff = 80 + 24 * entries;
    put_u32(f, 48, dfdOff);
    put_u32(f, 52, 44);
    put_u32(f, 56, 0); // kvdByteOffset
    put_u32(f, 60, 0); // kvdByteLength
    put_u64(f, 64, 0);
    put_u64(f, 72, 0);
    for (u32 i = 0; i < entries; ++i) {
        u64 bytes = u64(max(w >> i, 1u)) * max(h >> i, 1u) * 4;
        put_u64(f, level_field(i, 0), offsets[i]);
        put_u64(f, level_field(i, 1), bytes);
        put_u64(f, level_field(i, 2), bytes);
    }
    put_u32(f, dfdOff + 0, 44);
    put_u32(f, dfdOff + 4, 0);
    put_u32(f, dfdOff + 8, 2u | (40u << 16));
    f[dfdOff + 12] = 1; // RGBSDA
    f[dfdOff + 13] = 1; // BT709
    f[dfdOff + 14] = 1; // linear
    f[dfdOff + 20] = 4; // bytesPlane0
    put_u32(f, dfdOff + 28, 31u << 16);
    put_u32(f, dfdOff + 40, 0xFFFFFFFFu);
    return f;
}

} // namespace

KILN_TEST(Ktx2, HandBuiltMinimal) {
    // 1x1: header 80 + index 24 + DFD 44 = 148 (4-aligned), then 4 pixel bytes.
    u32 const offsets[] = {148};
    Vec<u8> f           = hand_built(1, 1, 1, offsets, 152);
    f[148]              = 10;
    f[149]              = 20;
    f[150]              = 30;
    f[151]              = 40;
    DiagCapture cap;
    DiagSink sink      = cap.sink();
    Result<Ktx2View> r = Ktx2View::open(f.span(), &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(cap.count, 0);
    KILN_CHECK_EQ(ev(r->desc().format), ev(Format::R8G8B8A8_UNORM));
    KILN_CHECK_EQ(r->desc().width, 1u);
    KILN_CHECK_EQ(r->desc().levels, 1u);
    KILN_CHECK(!r->has_kvd());
    KILN_CHECK(r->find_key("KTXwriter").empty());
    KILN_CHECK_EQ(r->dfd_sample_count(), 1u);
    Span<u8 const> px = r->level_data(0);
    KILN_REQUIRE_EQ(px.size, usize(4));
    KILN_CHECK(px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 40);
    KILN_CHECK(!r->wants_generated_mips());
}

KILN_TEST(Ktx2, HandBuiltLevelCountZero) {
    // levelCount 0 = "one level stored, generate the rest": one index entry.
    u32 const offsets[] = {148};
    Vec<u8> f           = hand_built(2, 2, 0, offsets, 164);
    Result<Ktx2View> r  = Ktx2View::open(f.span());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc().levels, 1u);
    KILN_CHECK_EQ(r->levels().size, usize(1));
    KILN_CHECK(r->wants_generated_mips());
    KILN_CHECK_EQ(r->level_data(0).size, usize(16));
}

KILN_TEST(Ktx2, HandBuiltLevelOrder) {
    // 2x1 with 2 levels: index ends at 128, DFD at 128..172. Level 0 (8 bytes) stored
    // before level 1 (4 bytes) violates the smallest-first rule.
    u32 const bad[] = {172, 180};
    Vec<u8> f       = hand_built(2, 1, 2, bad, 184);
    DiagCapture cap;
    DiagSink sink = cap.sink();
    KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));

    u32 const good[] = {176, 172};
    Vec<u8> g        = hand_built(2, 1, 2, good, 184);
    KILN_CHECK(Ktx2View::open(g.span()).ok());

    u32 const overlap[] = {172, 176}; // level 1 at 176..180 overlaps level 0 at 172..180
    Vec<u8> o           = hand_built(2, 1, 2, overlap, 184);
    KILN_CHECK_EQ(ev(Ktx2View::open(o.span()).code()), ev(Code::Corrupt));
}

KILN_TEST(Ktx2, ErrorBadIdentifier) {
    u32 const offsets[] = {148};
    Vec<u8> f           = hand_built(1, 1, 1, offsets, 152);
    f[1]                = 'X';
    DiagCapture cap;
    DiagSink sink = cap.sink();
    KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxIdentifier));
    KILN_CHECK(cap.severity == Severity::Error);
    KILN_CHECK_EQ(cap.count, 1);
}

KILN_TEST(Ktx2, ErrorTruncatedHeader) {
    u32 const offsets[] = {148};
    Vec<u8> f           = hand_built(1, 1, 1, offsets, 152);
    DiagCapture cap;
    DiagSink sink      = cap.sink();
    Result<Ktx2View> r = Ktx2View::open(f.span().first(40), &sink);
    KILN_CHECK_EQ(ev(r.code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxTruncated));
    KILN_CHECK(!Ktx2View::open(Span<u8 const>()).ok());
}

KILN_TEST(Ktx2, ErrorFormat) {
    u32 const offsets[] = {148};
    {
        Vec<u8> f = hand_built(1, 1, 1, offsets, 152);
        put_u32(f, 12, 0);
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Unsupported));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxFormat));
    }
    {
        Vec<u8> f = hand_built(1, 1, 1, offsets, 152);
        put_u32(f, 12, 1000); // not in the table
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Unsupported));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxFormat));
    }
}

KILN_TEST(Ktx2, ErrorTypeSize) {
    u32 const offsets[] = {148};
    Vec<u8> f           = hand_built(1, 1, 1, offsets, 152);
    put_u32(f, 16, 2);
    DiagCapture cap;
    DiagSink sink = cap.sink();
    KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxTypeSize));
}

KILN_TEST(Ktx2, ErrorSupercompression) {
    u32 const offsets[] = {148};
    for (u32 scheme : {1u, 3u, 4u}) { // BasisLZ, Zlib, unknown
        Vec<u8> f = hand_built(1, 1, 1, offsets, 152);
        put_u32(f, 44, scheme);
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Unsupported));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxSupercompression));
    }
    {
        // Zstd has no global data.
        Vec<u8> f = hand_built(1, 1, 1, offsets, 152);
        put_u32(f, 44, 2);
        put_u32(f, 72, 4); // sgdByteLength
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxSupercompression));
    }
    KILN_CHECK(StrView(supercompression_name(Supercompression::Zstd)) == "Zstd");
}

KILN_TEST(Ktx2, ErrorDimensions) {
    u32 const offsets1[] = {148};
    {
        Vec<u8> f = hand_built(1, 1, 1, offsets1, 152);
        put_u32(f, 24, 0); // pixelHeight 0: 1D
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Unsupported));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxDimensions));
    }
    {
        Vec<u8> f = hand_built(1, 1, 1, offsets1, 152);
        put_u32(f, 28, 4); // pixelDepth 4: 3D
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Unsupported));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxDimensions));
    }
    {
        Vec<u8> f = hand_built(1, 1, 1, offsets1, 152);
        put_u32(f, 36, 3); // faceCount 3
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxDimensions));
    }
    {
        // 2x2 has at most 2 levels (bit_width(2) == 2); levelCount 3 exceeds it. The
        // check runs before the level index is read, so one physical entry is enough.
        u32 const offsets2[] = {148};
        Vec<u8> f            = hand_built(2, 2, 0, offsets2, 164);
        put_u32(f, 40, 3);
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxDimensions));
    }
}

KILN_TEST(Ktx2, ErrorLevelIndex) {
    // 7x5 with the full 3-level mip chain, smallest first: level2 (1x1, 4 B) at 196,
    // level1 (3x2, 24 B) at 200, level0 (7x5, 140 B) at 224 (ends at 364 = EOF).
    u32 const offsets[] = {224, 200, 196};
    {
        Vec<u8> f = hand_built(7, 5, 3, offsets, 364);
        put_u64(f, level_field(0, 1), get_u64(f, level_field(0, 1)) + 1); // byteLength only
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Vec<u8> f = hand_built(7, 5, 3, offsets, 364);
        u64 len   = get_u64(f, level_field(1, 1)) + 1; // both lengths: size vs dimensions
        put_u64(f, level_field(1, 1), len);
        put_u64(f, level_field(1, 2), len);
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Vec<u8> f = hand_built(7, 5, 3, offsets, 364);
        put_u64(f, level_field(2, 0), get_u64(f, level_field(2, 0)) + 2); // misaligned
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Vec<u8> f = hand_built(7, 5, 3, offsets, 364);
        put_u64(f, level_field(2, 0), 80); // inside the metadata
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
}

namespace {

/// A 1x1 RGBA8 file whose one level is the Zstd frame `frame` (hand-built: a raw block, so the
/// reader tests do not depend on the encoder), stored at `offset`.
Vec<u8> zstd_file(Span<u8 const> frame, u32 offset = 148, u64 uncompressed = 4) {
    u32 const offsets[] = {offset};
    Vec<u8> f           = hand_built(1, 1, 1, offsets, u32(offset + frame.size));
    put_u32(f, 44, 2); // Zstd
    put_u64(f, level_field(0, 1), frame.size);
    put_u64(f, level_field(0, 2), uncompressed);
    std::memcpy(f.data() + offset, frame.data, frame.size);
    return f;
}

// Magic, frame header (single segment, 1-byte content size 4), last raw block of 4 bytes, data.
constexpr u8 kFrame[] = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x04, 0x21, 0x00, 0x00, 10, 20, 30, 40};

} // namespace

KILN_TEST(Ktx2, ZstdHandBuiltDecodes) {
    // Zstd levels need no alignment: offset 149.
    Vec<u8> const f    = zstd_file(kFrame, 149);
    Result<Ktx2View> r = Ktx2View::open(f.span());
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->supercompressed());
    KILN_CHECK_EQ(r->level_data(0).size, sizeof kFrame);
    u8 px[4] = {};
    KILN_REQUIRE(r->decode_level(0, Span<u8>(px, 4)).ok());
    KILN_CHECK(px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 40);
}

KILN_TEST(Ktx2, ZstdLevelIndexChecks) {
    {
        Vec<u8> const f = zstd_file(kFrame, 148, 5); // a 1x1 RGBA8 level is 4 bytes
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Vec<u8> f = zstd_file(kFrame);
        put_u64(f, level_field(0, 1), 0);
        DiagCapture cap;
        DiagSink sink = cap.sink();
        KILN_CHECK_EQ(ev(Ktx2View::open(f.span(), &sink).code()), ev(Code::Corrupt));
        KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));
    }
}

KILN_TEST(Ktx2, ZstdDecodeErrors) {
    auto const decode = [](Vec<u8> const& f, u32* code) {
        Result<Ktx2View> r = Ktx2View::open(f.span());
        if (!r.ok()) return Code::Unknown;
        u8 px[4] = {};
        DiagCapture cap;
        DiagSink sink   = cap.sink();
        Status const st = r->decode_level(0, Span<u8>(px, 4), nullptr, &sink);
        *code           = cap.code;
        return st.code;
    };
    u32 code = 0;
    {
        u8 bad[sizeof kFrame];
        std::memcpy(bad, kFrame, sizeof bad);
        bad[0] = 0; // not a Zstd frame
        KILN_CHECK_EQ(ev(decode(zstd_file(bad), &code)), ev(Code::Corrupt));
        KILN_CHECK_EQ(code, u32(kDiagKtxLevelDecode));
    }
    {
        // A frame of 3 bytes for a level of 4.
        u8 const shortFrame[] = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x03, 0x19, 0x00, 0x00, 1, 2, 3};
        KILN_CHECK_EQ(ev(decode(zstd_file(shortFrame), &code)), ev(Code::Corrupt));
        KILN_CHECK_EQ(code, u32(kDiagKtxLevelDecode));
    }
    {
        // A byte after the frame.
        u8 trailing[sizeof kFrame + 1] = {};
        std::memcpy(trailing, kFrame, sizeof kFrame);
        KILN_CHECK_EQ(ev(decode(zstd_file(trailing), &code)), ev(Code::Corrupt));
        KILN_CHECK_EQ(code, u32(kDiagKtxLevelDecode));
    }
    {
        Vec<u8> const f    = zstd_file(kFrame);
        Result<Ktx2View> r = Ktx2View::open(f.span());
        KILN_REQUIRE(r.ok());
        u8 px[5] = {};
        KILN_CHECK_EQ(ev(r->decode_level(0, Span<u8>(px, 5)).code), ev(Code::InvalidArgument));
        KILN_CHECK_EQ(ev(r->decode_level(1, Span<u8>(px, 4)).code), ev(Code::InvalidArgument));
        // A metadata-only open has no level data to decode.
        Result<Ktx2View> prefix = Ktx2View::open(f.span().subspan(0, 148));
        KILN_REQUIRE(prefix.ok());
        KILN_CHECK_EQ(ev(prefix->decode_level(0, Span<u8>(px, 4)).code), ev(Code::InvalidArgument));
    }
}
