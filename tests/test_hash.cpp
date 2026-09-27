#include "kiln_test.h"

#include "kiln/hash.h"

#include <cstring>

using namespace kiln;

namespace {

struct DummyTag; // phantom tag for Handle<>

enum class Color : u32 { Red, Green, Blue };

constexpr u8 kAbc[3] = {'a', 'b', 'c'};

} // namespace

// ---------------------------------------------------------------------------
// fourcc
// ---------------------------------------------------------------------------

static_assert(fourcc('O', 'M', 'S', 'H') == 0x48534D4Fu);
static_assert(fourcc("OMSH") == fourcc('O', 'M', 'S', 'H'));

KILN_TEST(Hash, FourccStrRoundTrip) {
    char buf[5];
    fourcc_str(fourcc('O', 'M', 'S', 'H'), buf);
    KILN_CHECK(std::strcmp(buf, "OMSH") == 0);

    char buf2[5];
    fourcc_str(fourcc("TEST"), buf2);
    KILN_CHECK(std::strcmp(buf2, "TEST") == 0);
}

// ---------------------------------------------------------------------------
// FNV-1a 64
// ---------------------------------------------------------------------------

static_assert(fnv1a64(""_sv) == 0xcbf29ce484222325ull);
static_assert(fnv1a64("a"_sv) == 0xaf63dc4c8601ec8cull);
static_assert(fnv1a64("foobar"_sv) == 0x85944171f73967e8ull);

static_assert("foobar"_h == fnv1a64("foobar"_sv));
static_assert(hash_name("foobar"_sv) == fnv1a64("foobar"_sv));

KILN_TEST(Hash, Fnv1a64KnownVectors) {
    KILN_CHECK_EQ(fnv1a64(""_sv), u64(0xcbf29ce484222325ull));
    KILN_CHECK_EQ(fnv1a64("a"_sv), u64(0xaf63dc4c8601ec8cull));
    KILN_CHECK_EQ(fnv1a64("foobar"_sv), u64(0x85944171f73967e8ull));
}

KILN_TEST(Hash, HashNameMatchesFnv1a64) {
    KILN_CHECK_EQ(hash_name("meshes/ship"_sv), fnv1a64("meshes/ship"_sv));
}

// ---------------------------------------------------------------------------
// XXH64
// ---------------------------------------------------------------------------

// Regression: constexpr evaluation over buffers shorter than 8 bytes (the tail
// loops must not form pointers more than one past the end).
static_assert(xxh64(kAbc, 3) == 0x44BC2CF5AD770999ull);

KILN_TEST(Hash, Xxh64SmallArrayRuntime) { KILN_CHECK_EQ(xxh64(kAbc, usize(3)), u64(0x44BC2CF5AD770999ull)); }

// XXH32 reference vectors (xxHash test suite / python-xxhash docs).
static_assert(xxh32(kAbc, 3) == 0x32D153FFu);
KILN_TEST(Hash, Xxh32ReferenceVectors) {
    KILN_CHECK_EQ(xxh32(""_sv), u32(0x02CC5D05u));
    KILN_CHECK_EQ(xxh32("a"_sv), u32(0x550D7456u));
    KILN_CHECK_EQ(xxh32("abc"_sv), u32(0x32D153FFu));
    KILN_CHECK_EQ(xxh32("Nobody inspects the spammish repetition"_sv), u32(0xE2293B2Fu));
    KILN_CHECK_EQ(xxh32(""_sv, 1u), u32(0x0B2CB792u));
}

KILN_TEST(Hash, Xxh32TailPathsAreDeterministic) {
    // Lengths 0..40 exercise the 16-byte stripe loop, 4-byte tail and byte tail.
    u8 buf[40];
    for (usize i = 0; i < sizeof buf; ++i)
        buf[i] = u8(i * 31 + 7);
    u32 prev = 0;
    for (usize len = 0; len <= sizeof buf; ++len) {
        u32 h = xxh32(buf, len);
        KILN_CHECK_EQ(h, xxh32(buf, len)); // stable
        if (len) KILN_CHECK_NE(h, prev);   // length-sensitive
        prev = h;
    }
}

KILN_TEST(Hash, Xxh64ReferenceVectorsSeedZero) {
    KILN_CHECK_EQ(xxh64(""_sv), u64(0xEF46DB3751D8E999ull));
    KILN_CHECK_EQ(xxh64("a"_sv), u64(0xD24EC4F1A98C6E5Bull));
    KILN_CHECK_EQ(xxh64("abc"_sv), u64(0x44BC2CF5AD770999ull));
    KILN_CHECK_EQ(xxh64("Nobody inspects the spammish repetition"_sv), u64(0xFBCEA83C8A378BF1ull));
}

KILN_TEST(Hash, Xxh64ReferenceVectorWithSeed) {
    KILN_CHECK_EQ(xxh64(""_sv, u64(1)), u64(0xD5AFBA1336A3BE4Bull));
}

KILN_TEST(Hash, Xxh64StreamingConsistency) {
    u8 buf[201];
    for (usize len = 0; len <= 200; ++len) {
        for (usize i = 0; i < len; ++i)
            buf[i] = u8(i * 31 + 7);
        u64 expected = xxh64(buf, len);

        Xxh64State oneShot;
        oneShot.update(buf, len);
        if (!KILN_CHECK_MSG(oneShot.digest() == expected, "one-shot mismatch at len=%zu", len)) continue;

        Xxh64State byteAtATime;
        for (usize i = 0; i < len; ++i)
            byteAtATime.update(buf + i, 1);
        KILN_CHECK_MSG(byteAtATime.digest() == expected, "byte-at-a-time mismatch at len=%zu", len);

        Xxh64State chunk7;
        for (usize off = 0; off < len;) {
            usize n = min(usize(7), len - off);
            chunk7.update(buf + off, n);
            off += n;
        }
        KILN_CHECK_MSG(chunk7.digest() == expected, "chunk7 mismatch at len=%zu", len);

        Xxh64State chunk32;
        for (usize off = 0; off < len;) {
            usize n = min(usize(32), len - off);
            chunk32.update(buf + off, n);
            off += n;
        }
        KILN_CHECK_MSG(chunk32.digest() == expected, "chunk32 mismatch at len=%zu", len);

        Xxh64State chunk33;
        for (usize off = 0; off < len;) {
            usize n = min(usize(33), len - off);
            chunk33.update(buf + off, n);
            off += n;
        }
        KILN_CHECK_MSG(chunk33.digest() == expected, "chunk33 mismatch at len=%zu", len);
    }
}

// ---------------------------------------------------------------------------
// mix64 / hash_combine / hash_of
// ---------------------------------------------------------------------------

KILN_TEST(Hash, Mix64Distinct) {
    KILN_CHECK_NE(mix64(u64(1)), mix64(u64(2)));
    KILN_CHECK_EQ(mix64(u64(1)), mix64(u64(1)));
}

KILN_TEST(Hash, HashCombineNotCommutative) {
    KILN_CHECK_NE(hash_combine(u64(1), u64(2)), hash_combine(u64(2), u64(1)));
}

KILN_TEST(Hash, HashOfIntDeterministic) {
    KILN_CHECK_EQ(hash_of(42), hash_of(42));
    KILN_CHECK_NE(hash_of(42), hash_of(43));
}

KILN_TEST(Hash, HashOfStrViewDeterministic) {
    KILN_CHECK_EQ(hash_of("abc"_sv), hash_of("abc"_sv));
    KILN_CHECK_NE(hash_of("abc"_sv), hash_of("abd"_sv));
}

KILN_TEST(Hash, HashOfHandleDeterministic) {
    Handle<DummyTag> h1 = Handle<DummyTag>::from_bits(0x0000000100000002ull);
    Handle<DummyTag> h2 = Handle<DummyTag>::from_bits(0x0000000100000002ull);
    Handle<DummyTag> h3 = Handle<DummyTag>::from_bits(0x0000000100000003ull);
    KILN_CHECK_EQ(hash_of(h1), hash_of(h2));
    KILN_CHECK_NE(hash_of(h1), hash_of(h3));
}

KILN_TEST(Hash, HashOfEnumDeterministic) {
    KILN_CHECK_EQ(hash_of(Color::Red), hash_of(Color::Red));
    KILN_CHECK_NE(hash_of(Color::Red), hash_of(Color::Green));
}
