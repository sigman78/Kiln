#include "kiln_test.h"

#include "kiln/containers.h"
#include "kiln/ktx2.h"
#include "kiln/ktx2_writer.h"

#include <cstdio>

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

constexpr u32 lcm4(u32 blockBytes) {
    u32 a = blockBytes, b = 4;
    while (b != 0) {
        u32 t = a % b;
        a     = b;
        b     = t;
    }
    return blockBytes / a * 4;
}

/// A 7x5 texture with its full mip chain (7x5, 3x2, 1x1) and a deterministic pattern.
struct TestImage {
    static constexpr u32 kWidth  = 7;
    static constexpr u32 kHeight = 5;
    static constexpr u32 kLevels = 3;

    Format format;
    Vec<u8> data[kLevels];
    Span<u8 const> spans[kLevels];

    explicit TestImage(Format f) : format(f) {
        for (u32 i = 0; i < kLevels; ++i) {
            u64 n = format_image_bytes(f, max(kWidth >> i, 1u), max(kHeight >> i, 1u));
            data[i].init(default_allocator(), Tag::Test);
            data[i].resize(usize(n));
            for (usize k = 0; k < data[i].size(); ++k)
                data[i][k] = u8(i * 31u + k * 7u + u32(f));
            spans[i] = data[i].span();
        }
    }
    WriteDesc desc(StrView tag = "kiln-cook") const {
        return WriteDesc{.format    = format,
                         .width     = kWidth,
                         .height    = kHeight,
                         .levels    = Span<Span<u8 const> const>(spans, kLevels),
                         .writerTag = tag};
    }
};

bool bytes_equal(Span<u8 const> a, Span<u8 const> b) {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

Vec<u8> write_ok(WriteDesc const& d) {
    Result<Vec<u8>> r = write(d, default_allocator());
    KILN_CHECK(r.ok());
    if (!r.ok()) return Vec<u8>(default_allocator(), Tag::Test);
    return std::move(r).value();
}

/// Write `bytes` to `<dir>/<name>` with plain C stdio, checking that fopen/fwrite succeed.
void write_sample_file(char const* dir, char const* name, Span<u8 const> bytes) {
    char path[1024];
    format(path, sizeof path, "%s/%s", dir, name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", path)) return;
    usize written = bytes.size ? std::fwrite(bytes.data, 1, bytes.size, f) : 0;
    KILN_CHECK_MSG(written == bytes.size, "fwrite %s: wrote %zu of %zu bytes", path, written, bytes.size);
    std::fclose(f);
}

void put_u32(Vec<u8>& v, usize off, u32 x) { write_unaligned<u32>(v.data() + off, x); }
void put_u64(Vec<u8>& v, usize off, u64 x) { write_unaligned<u64>(v.data() + off, x); }
u64 get_u64(Vec<u8> const& v, usize off) { return read_unaligned<u64>(v.data() + off); }

// Level index entry i starts at 80 + 24 * i: byteOffset, byteLength, uncompressedByteLength.
constexpr usize level_field(u32 i, u32 field) { return 80 + 24 * usize(i) + 8 * usize(field); }

} // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

KILN_TEST(Ktx2, RoundTripFormats) {
    Format const formats[] = {
        Format::R8_UNORM,  Format::R8G8_UNORM,   Format::R8G8B8A8_UNORM,      Format::R8G8B8A8_SRGB,
        Format::R16_UNORM, Format::R16G16_SNORM, Format::R16G16B16A16_SFLOAT, Format::R32G32_SFLOAT};
    for (Format f : formats) {
        FormatInfo const& info = *format_info(f);
        TestImage img(f);
        Vec<u8> file = write_ok(img.desc());
        DiagCapture cap;
        DiagSink sink      = cap.sink();
        Result<Ktx2View> r = Ktx2View::open(file.span(), &sink, "roundtrip");
        KILN_CHECK_MSG(r.ok(), "%s: open failed (K%u)", info.name, cap.code);
        if (!r.ok()) continue;
        KILN_CHECK_EQ(cap.count, 0);
        Ktx2View const& v = *r;

        TextureDesc d = v.desc();
        KILN_CHECK_EQ(ev(d.format), ev(f));
        KILN_CHECK_EQ(d.width, 7u);
        KILN_CHECK_EQ(d.height, 5u);
        KILN_CHECK_EQ(d.depth, 1u);
        KILN_CHECK_EQ(d.layers, 1u);
        KILN_CHECK_EQ(d.faces, 1u);
        KILN_CHECK_EQ(d.levels, 3u);
        KILN_CHECK(!d.isArray);
        KILN_CHECK(!d.isCube);
        KILN_CHECK_EQ(v.header().typeSize, u32(info.bitsPerChannel / 8));
        KILN_CHECK_EQ(v.levels().size, usize(3));
        KILN_CHECK(v.has_all_level_data());

        u32 const expectW[3] = {7, 3, 1}, expectH[3] = {5, 2, 1};
        u32 const align = lcm4(info.bytesPerBlock);
        for (u32 i = 0; i < 3; ++i) {
            KILN_CHECK_EQ(v.level_width(i), expectW[i]);
            KILN_CHECK_EQ(v.level_height(i), expectH[i]);
            KILN_CHECK_EQ(v.level_image_bytes(i), u64(img.spans[i].size));
            KILN_CHECK_MSG(bytes_equal(v.level_data(i), img.spans[i]), "%s level %u data", info.name, i);
            KILN_CHECK_EQ(v.levels()[i].byteOffset % align, u64(0));
            KILN_CHECK_EQ(v.levels()[i].uncompressedByteLength, v.levels()[i].byteLength);
        }
        // Smallest level first; level 0 last and ending at EOF.
        KILN_CHECK(v.levels()[2].byteOffset < v.levels()[1].byteOffset);
        KILN_CHECK(v.levels()[1].byteOffset < v.levels()[0].byteOffset);
        KILN_CHECK_EQ(v.levels()[0].byteOffset + v.levels()[0].byteLength, u64(file.size()));

        Span<u8 const> writer = v.find_key("KTXwriter");
        KILN_CHECK(bytes_equal(writer, Span<u8 const>(reinterpret_cast<u8 const*>("kiln-cook"), 10)));
        KILN_CHECK(v.find_key("KTXorientation").empty());
        KILN_CHECK(v.has_kvd());

        KILN_CHECK_EQ(v.dfd_transfer_function(), u8(info.srgb ? 2 : 1));
        KILN_CHECK_EQ(v.dfd_color_model(), u8(1));
        KILN_CHECK_EQ(v.dfd_flags(), u8(0));
        KILN_CHECK_EQ(v.dfd_sample_count(), u32(info.channels));
        KILN_CHECK_EQ(v.dfd().size, usize(4 + 24 + 16 * info.channels));
        KILN_CHECK_EQ(v.header().dfdByteOffset, u32(80 + 24 * 3));
        KILN_CHECK_EQ(v.header().kvdByteOffset, v.header().dfdByteOffset + v.header().dfdByteLength);
        KILN_CHECK_EQ(v.header().kvdByteLength % 4, 0u);
    }
}

KILN_TEST(Ktx2, DfdMatchesVk2dfd) {
    // R16G16_SNORM: two 16-bit signed samples, lower -32767, upper 32767.
    {
        TestImage img(Format::R16G16_SNORM);
        Vec<u8> file  = write_ok(img.desc());
        u8 const* dfd = file.data() + read_unaligned<u32>(file.data() + 48);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 0), 4u + 24u + 32u);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 4), 0u);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 8), 2u | (56u << 16));
        KILN_CHECK_EQ(dfd[20], u8(4)); // bytesPlane0
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 28), 0u | (15u << 16) | (0x40u << 24));
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 36), 0xFFFF8001u);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 40), 0x7FFFu);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 44), 16u | (15u << 16) | ((0x40u | 1u) << 24));
    }
    // R8G8B8A8_SRGB: alpha sample carries the LINEAR qualifier; UNORM range 0..255.
    {
        TestImage img(Format::R8G8B8A8_SRGB);
        Vec<u8> file  = write_ok(img.desc());
        u8 const* dfd = file.data() + read_unaligned<u32>(file.data() + 48);
        KILN_CHECK_EQ(dfd[14], u8(2));
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 28), 0u | (7u << 16));
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 40), 255u);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 28 + 48), 24u | (7u << 16) | ((15u | 0x20u) << 24));
    }
    // R16G16B16A16_SFLOAT: -1.0f / 1.0f bounds.
    {
        TestImage img(Format::R16G16B16A16_SFLOAT);
        Vec<u8> file  = write_ok(img.desc());
        u8 const* dfd = file.data() + read_unaligned<u32>(file.data() + 48);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 28), 0u | (15u << 16) | (0xC0u << 24));
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 36), 0xBF800000u);
        KILN_CHECK_EQ(read_unaligned<u32>(dfd + 40), 0x3F800000u);
    }
    // premultipliedAlpha sets the DFD flag.
    {
        TestImage img(Format::R8G8B8A8_UNORM);
        WriteDesc d          = img.desc();
        d.premultipliedAlpha = true;
        Vec<u8> file         = write_ok(d);
        Result<Ktx2View> r   = Ktx2View::open(file.span());
        KILN_REQUIRE(r.ok());
        KILN_CHECK_EQ(r->dfd_flags(), u8(1));
    }
}

// ---------------------------------------------------------------------------
// Determinism, prefix open
// ---------------------------------------------------------------------------

KILN_TEST(Ktx2, Deterministic) {
    TestImage img(Format::R8G8B8A8_SRGB);
    Vec<u8> a = write_ok(img.desc());
    Vec<u8> b = write_ok(img.desc());
    KILN_CHECK(bytes_equal(a.span(), b.span()));

    // Same-length tags keep the layout, so only KVD bytes may differ.
    Vec<u8> c = write_ok(img.desc("other-tag"));
    KILN_REQUIRE_EQ(a.size(), c.size());
    u32 const kvdOff = read_unaligned<u32>(a.data() + 56);
    u32 const kvdLen = read_unaligned<u32>(a.data() + 60);
    usize diffs      = 0;
    for (usize i = 0; i < a.size(); ++i) {
        if (a[i] == c[i]) continue;
        ++diffs;
        KILN_CHECK_MSG(i >= kvdOff && i < kvdOff + kvdLen, "byte %zu differs outside the KVD", i);
    }
    KILN_CHECK(diffs > 0);

    Result<Ktx2View> r = Ktx2View::open(c.span());
    KILN_REQUIRE(r.ok());
    KILN_CHECK(
        bytes_equal(r->find_key("KTXwriter"), Span<u8 const>(reinterpret_cast<u8 const*>("other-tag"), 10)));
}

KILN_TEST(Ktx2, PrefixOpen) {
    TestImage img(Format::R16_UNORM);
    Vec<u8> file          = write_ok(img.desc());
    Result<Ktx2View> full = Ktx2View::open(file.span());
    KILN_REQUIRE(full.ok());
    u64 const firstLevel = full->levels()[2].byteOffset; // smallest level is stored first
    KILN_CHECK(Ktx2View::metadata_size(full->header()) <= firstLevel);

    Result<Ktx2View> r = Ktx2View::open(file.span().first(usize(firstLevel)));
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc().levels, 3u);
    KILN_CHECK(!r->has_all_level_data());
    for (u32 i = 0; i < 3; ++i)
        KILN_CHECK(r->level_data(i).empty());
    KILN_CHECK_EQ(r->levels()[0].byteOffset, full->levels()[0].byteOffset);

    // Exactly the metadata prefix works too.
    Result<Ktx2View> m = Ktx2View::open(file.span().first(usize(Ktx2View::metadata_size(full->header()))));
    KILN_CHECK(m.ok());

    // One byte short of it does not.
    DiagCapture cap;
    DiagSink sink = cap.sink();
    Result<Ktx2View> t =
        Ktx2View::open(file.span().first(usize(Ktx2View::metadata_size(full->header())) - 1), &sink);
    KILN_CHECK_EQ(ev(t.code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxTruncated));
}

// ---------------------------------------------------------------------------
// Reader error paths
// ---------------------------------------------------------------------------

namespace {

struct Mutated {
    Vec<u8> file;
    DiagCapture cap;
    Code open() {
        DiagSink sink = cap.sink();
        return Ktx2View::open(file.span(), &sink, "mutated").code();
    }
};

Mutated mutate_rgba8() {
    TestImage img(Format::R8G8B8A8_UNORM);
    return Mutated{write_ok(img.desc()), {}};
}

} // namespace

KILN_TEST(Ktx2, ErrorBadIdentifier) {
    Mutated m = mutate_rgba8();
    m.file[1] = 'X';
    KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
    KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxIdentifier));
    KILN_CHECK(m.cap.severity == Severity::Error);
    KILN_CHECK_EQ(m.cap.count, 1);
}

KILN_TEST(Ktx2, ErrorTruncatedHeader) {
    Mutated m          = mutate_rgba8();
    DiagSink sink      = m.cap.sink();
    Result<Ktx2View> r = Ktx2View::open(m.file.span().first(40), &sink);
    KILN_CHECK_EQ(ev(r.code()), ev(Code::Corrupt));
    KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxTruncated));
    KILN_CHECK(!Ktx2View::open(Span<u8 const>()).ok());
}

KILN_TEST(Ktx2, ErrorFormat) {
    Mutated m = mutate_rgba8();
    put_u32(m.file, 12, 0);
    KILN_CHECK_EQ(ev(m.open()), ev(Code::Unsupported));
    KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxFormat));

    Mutated u = mutate_rgba8();
    put_u32(u.file, 12, 1000); // not in the table
    KILN_CHECK_EQ(ev(u.open()), ev(Code::Unsupported));
    KILN_CHECK_EQ(u.cap.code, u32(kDiagKtxFormat));
}

KILN_TEST(Ktx2, ErrorTypeSize) {
    Mutated m = mutate_rgba8();
    put_u32(m.file, 16, 2);
    KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
    KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxTypeSize));
}

KILN_TEST(Ktx2, ErrorSupercompression) {
    Mutated m = mutate_rgba8();
    put_u32(m.file, 44, 2);
    KILN_CHECK_EQ(ev(m.open()), ev(Code::Unsupported));
    KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxSupercompression));
    KILN_CHECK(StrView(supercompression_name(Supercompression::Zstd)) == "Zstd");
}

KILN_TEST(Ktx2, ErrorDimensions) {
    {
        Mutated m = mutate_rgba8();
        put_u32(m.file, 24, 0); // pixelHeight 0: 1D
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Unsupported));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDimensions));
    }
    {
        Mutated m = mutate_rgba8();
        put_u32(m.file, 28, 4); // pixelDepth 4: 3D
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Unsupported));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDimensions));
    }
    {
        Mutated m = mutate_rgba8();
        put_u32(m.file, 36, 3); // faceCount 3
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDimensions));
    }
    {
        Mutated m = mutate_rgba8();
        put_u32(m.file, 40, 4); // 7x5 has at most 3 levels
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDimensions));
    }
}

KILN_TEST(Ktx2, ErrorLevelIndex) {
    {
        Mutated m = mutate_rgba8();
        put_u64(m.file, level_field(0, 1), get_u64(m.file, level_field(0, 1)) + 1); // byteLength only
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Mutated m = mutate_rgba8();
        u64 len   = get_u64(m.file, level_field(1, 1)) + 1; // both lengths: size vs dimensions
        put_u64(m.file, level_field(1, 1), len);
        put_u64(m.file, level_field(1, 2), len);
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Mutated m = mutate_rgba8();
        put_u64(m.file, level_field(2, 0), get_u64(m.file, level_field(2, 0)) + 2); // misaligned
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxLevelIndex));
    }
    {
        Mutated m = mutate_rgba8();
        put_u64(m.file, level_field(2, 0), 80); // inside the metadata
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxLevelIndex));
    }
}

KILN_TEST(Ktx2, ErrorDfdAndKvd) {
    {
        Mutated m = mutate_rgba8();
        usize dfd = read_unaligned<u32>(m.file.data() + 48);
        put_u32(m.file, dfd, 12); // totalSize != dfdByteLength
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDfd));
    }
    {
        Mutated m = mutate_rgba8();
        usize kvd = read_unaligned<u32>(m.file.data() + 56);
        put_u32(m.file, kvd, 1000); // entry runs past the KVD
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxKvd));
    }
    {
        Mutated m            = mutate_rgba8();
        usize kvd            = read_unaligned<u32>(m.file.data() + 56);
        m.file[kvd + 4 + 9]  = 'x'; // key's NUL overwritten: "KTXwriterxkiln-cook\0" is still terminated...
        m.file[kvd + 4 + 19] = 'y'; // ...until the value's NUL goes too
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Corrupt));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxKvd));
    }
    {
        // transferFunction disagreeing with the vkFormat is only a warning.
        TestImage img(Format::R8G8B8A8_SRGB);
        Mutated m{write_ok(img.desc()), {}};
        usize dfd        = read_unaligned<u32>(m.file.data() + 48);
        m.file[dfd + 14] = 1;
        KILN_CHECK_EQ(ev(m.open()), ev(Code::Ok));
        KILN_CHECK_EQ(m.cap.code, u32(kDiagKtxDfd));
        KILN_CHECK(m.cap.severity == Severity::Warning);
    }
}

// ---------------------------------------------------------------------------
// Hand-built file: the reader does not depend on the writer
// ---------------------------------------------------------------------------

namespace {

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

// ---------------------------------------------------------------------------
// Writer input validation
// ---------------------------------------------------------------------------

KILN_TEST(Ktx2, WriterRejectsBadInput) {
    TestImage img(Format::R8G8B8A8_UNORM);
    DiagCapture cap;
    DiagSink sink = cap.sink();

    // Wrong level byte size.
    Span<u8 const> spans[3] = {img.spans[0], img.spans[1], img.spans[2].first(0)};
    WriteDesc d             = img.desc();
    d.levels                = Span<Span<u8 const> const>(spans, 3);
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxLevelIndex));

    // Block-compressed formats are post-v0.5.
    d        = img.desc();
    d.format = Format::BC7_UNORM;
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxFormat));

    d        = img.desc();
    d.format = Format::Undefined;
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));

    // Too many levels (7x5 has 3), none, zero extent.
    Span<u8 const> four[4] = {img.spans[0], img.spans[1], img.spans[2], img.spans[2]};
    d                      = img.desc();
    d.levels               = Span<Span<u8 const> const>(four, 4);
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));
    KILN_CHECK_EQ(cap.code, u32(kDiagKtxDimensions));
    d        = img.desc();
    d.levels = {};
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));
    d       = img.desc();
    d.width = 0;
    KILN_CHECK_EQ(ev(write(d, default_allocator(), &sink).code()), ev(Code::InvalidArgument));

    // A single level (no mips) is fine.
    d                   = img.desc();
    d.levels            = Span<Span<u8 const> const>(img.spans, 1);
    Result<Vec<u8>> one = write(d, default_allocator(), &sink);
    KILN_REQUIRE(one.ok());
    Result<Ktx2View> r = Ktx2View::open(one->span());
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->desc().levels, 1u);
    KILN_CHECK(bytes_equal(r->level_data(0), img.spans[0]));
}

// ---------------------------------------------------------------------------
// Sample files for kiln-info's CTest coverage (tests/CMakeLists.txt)
// ---------------------------------------------------------------------------

KILN_TEST(Ktx2, WriteSampleFiles) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return; // no --samples <dir> given: nothing to do

    TestImage rgba(Format::R8G8B8A8_SRGB);
    Vec<u8> const rgbaFile = write_ok(rgba.desc());
    KILN_REQUIRE(!rgbaFile.empty());
    write_sample_file(dir, "sample_rgba8_srgb.ktx2", rgbaFile.span());

    TestImage snorm(Format::R16G16_SNORM);
    Vec<u8> const snormFile = write_ok(snorm.desc());
    KILN_REQUIRE(!snormFile.empty());
    write_sample_file(dir, "sample_r16g16_snorm.ktx2", snormFile.span());
}
