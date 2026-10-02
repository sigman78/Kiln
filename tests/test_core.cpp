#include "kiln_test.h"

#include "kiln/core.h"

#include <cstdint>

using namespace kiln;

namespace {

KILN_TEST(Core, SpanFromArray) {
    int arr[5] = {1, 2, 3, 4, 5};
    Span<int> sp(arr);
    KILN_REQUIRE_EQ(sp.size, usize(5));
    KILN_CHECK(sp.data == arr);
    KILN_CHECK_EQ(sp[0], 1);
    KILN_CHECK_EQ(sp[4], 5);
    KILN_CHECK(!sp.empty());
    KILN_CHECK_EQ(sp.size_bytes(), usize(5 * sizeof(int)));
}

KILN_TEST(Core, SpanEmpty) {
    Span<int> sp;
    KILN_CHECK(sp.empty());
    KILN_CHECK_EQ(sp.size, usize(0));
    KILN_CHECK(sp.data == nullptr);
}

KILN_TEST(Core, SpanFirstLastSubspan) {
    int arr[5] = {10, 20, 30, 40, 50};
    Span<int> sp(arr);

    Span<int> f = sp.first(2);
    KILN_REQUIRE_EQ(f.size, usize(2));
    KILN_CHECK_EQ(f[0], 10);
    KILN_CHECK_EQ(f[1], 20);

    Span<int> l = sp.last(2);
    KILN_REQUIRE_EQ(l.size, usize(2));
    KILN_CHECK_EQ(l[0], 40);
    KILN_CHECK_EQ(l[1], 50);

    Span<int> sub = sp.subspan(1, 3);
    KILN_REQUIRE_EQ(sub.size, usize(3));
    KILN_CHECK_EQ(sub[0], 20);
    KILN_CHECK_EQ(sub[1], 30);
    KILN_CHECK_EQ(sub[2], 40);

    Span<int> sub2 = sp.subspan(3);
    KILN_REQUIRE_EQ(sub2.size, usize(2));
    KILN_CHECK_EQ(sub2[0], 40);
    KILN_CHECK_EQ(sub2[1], 50);
}

KILN_TEST(Core, SpanConstConversion) {
    int arr[3] = {7, 8, 9};
    Span<int> sp(arr);
    Span<int const> csp = sp; // implicit conversion to Span<T const>
    KILN_REQUIRE_EQ(csp.size, usize(3));
    KILN_CHECK_EQ(csp[0], 7);
    KILN_CHECK_EQ(csp[1], 8);
    KILN_CHECK_EQ(csp[2], 9);
    KILN_CHECK(csp.data == sp.data);
}

KILN_TEST(Core, SpanAsBytes) {
    u32 arr[2] = {0x11223344u, 0x55667788u};
    Span<u32> sp(arr);
    Span<u8 const> bytes = as_bytes(sp);
    KILN_REQUIRE_EQ(bytes.size, usize(8));
    // little-endian only (static_assert in core.h)
    KILN_CHECK_EQ(bytes[0], u8(0x44));
    KILN_CHECK_EQ(bytes[1], u8(0x33));
    KILN_CHECK_EQ(bytes[2], u8(0x22));
    KILN_CHECK_EQ(bytes[3], u8(0x11));
    KILN_CHECK_EQ(bytes[4], u8(0x88));
    KILN_CHECK_EQ(bytes[5], u8(0x77));
    KILN_CHECK_EQ(bytes[6], u8(0x66));
    KILN_CHECK_EQ(bytes[7], u8(0x55));
}

KILN_TEST(Core, SpanAsWritableBytes) {
    u16 arr[2] = {0, 0};
    Span<u16> sp(arr);
    Span<u8> bytes = as_writable_bytes(sp);
    KILN_REQUIRE_EQ(bytes.size, usize(4));
    bytes[0] = u8(0xAB);
    bytes[1] = u8(0xCD);
    KILN_CHECK_EQ(arr[0], u16(0xCDAB));
}

KILN_TEST(Core, StrViewFromLiteralAndSv) {
    StrView a = "test";
    StrView b = "test"_sv;
    KILN_CHECK(a == b);
    StrView c = "other"_sv;
    KILN_CHECK(a != c);
    KILN_CHECK_EQ(a.size, usize(4));
    KILN_CHECK(!a.empty());
    StrView e;
    KILN_CHECK(e.empty());
}

KILN_TEST(Core, StrViewEqualityAndCompare) {
    KILN_CHECK("abc"_sv == "abc"_sv);
    KILN_CHECK("abc"_sv != "abd"_sv);
    KILN_CHECK(compare("abc"_sv, "abd"_sv) < 0);
    KILN_CHECK(compare("abd"_sv, "abc"_sv) > 0);
    KILN_CHECK_EQ(compare("abc"_sv, "abc"_sv), 0);
    KILN_CHECK(compare("ab"_sv, "abc"_sv) < 0);
    KILN_CHECK(compare("abc"_sv, "ab"_sv) > 0);
}

KILN_TEST(Core, StrViewStartsEndsWith) {
    StrView s = "hello world"_sv;
    KILN_CHECK(s.starts_with("hello"_sv));
    KILN_CHECK(!s.starts_with("world"_sv));
    KILN_CHECK(s.ends_with("world"_sv));
    KILN_CHECK(!s.ends_with("hello"_sv));
    KILN_CHECK(s.starts_with(""_sv));
    KILN_CHECK(s.ends_with(""_sv));
}

KILN_TEST(Core, StrViewFindRfind) {
    StrView s = "hello world"_sv;
    KILN_CHECK_EQ(s.find('o'), usize(4));
    KILN_CHECK_EQ(s.find('o', 5), usize(7));
    KILN_CHECK_EQ(s.find('z'), StrView::kNpos);
    KILN_CHECK_EQ(s.rfind('o'), usize(7));
    KILN_CHECK_EQ(s.rfind('h'), usize(0));
    KILN_CHECK_EQ(s.rfind('z'), StrView::kNpos);
}

KILN_TEST(Core, StrViewSubstr) {
    StrView s = "hello world"_sv;
    KILN_CHECK(s.substr(6) == "world"_sv);
    KILN_CHECK(s.substr(0, 5) == "hello"_sv);
    KILN_CHECK(s.substr(6, 100) == "world"_sv); // clamped to remaining size
    KILN_CHECK(s.substr(s.size).empty());
}

struct MeshTag; // phantom tag type, never defined

KILN_TEST(Core, HandleNullByDefault) {
    Handle<MeshTag> h;
    KILN_CHECK(h.is_null());
    KILN_CHECK(!bool(h));
    KILN_CHECK_EQ(h.index, u32(0));
    KILN_CHECK_EQ(h.generation, u32(0));
    KILN_CHECK(h == Handle<MeshTag>::from_bits(0));
}

KILN_TEST(Core, HandleFromBitsRoundTrip) {
    Handle<MeshTag> h{42, 7};
    KILN_CHECK(bool(h));
    u64 bits           = h.bits();
    Handle<MeshTag> h2 = Handle<MeshTag>::from_bits(bits);
    KILN_CHECK(h == h2);
    KILN_CHECK_EQ(h2.index, u32(42));
    KILN_CHECK_EQ(h2.generation, u32(7));
}

KILN_TEST(Core, HandleEquality) {
    Handle<MeshTag> a{1, 1};
    Handle<MeshTag> b{1, 1};
    Handle<MeshTag> c{1, 2};
    Handle<MeshTag> d{2, 1};
    KILN_CHECK(a == b);
    KILN_CHECK(a != c);
    KILN_CHECK(a != d);
}

int add_ints(int a, int b) { return a + b; } // not noexcept on purpose; add_ints_noexcept covers that case

struct Pair {
    int a;
    int b;
};

int pair_sum_thunk(void* user, int x) {
    Pair* p = static_cast<Pair*>(user);
    return p->a + p->b + x;
}

KILN_TEST_DEPRECATED_BEGIN

KILN_TEST(Core, FunctionRefFromFunctionPointer) {
    FunctionRef<int(int, int)> f = add_ints;
    KILN_CHECK(bool(f));
    KILN_CHECK_EQ(f(2, 3), 5);
}

// Regression: a noexcept function must pick the function-pointer constructor, not
// the generic object-callable one (which would cast a function pointer to void*).
int add_ints_noexcept(int a, int b) noexcept { return a + b; }
KILN_TEST(Core, FunctionRefFromNoexceptFunctionPointer) {
    FunctionRef<int(int, int)> f = add_ints_noexcept;
    KILN_CHECK_EQ(f(2, 3), 5);
}

KILN_TEST(Core, FunctionRefFromCapturingLambda) {
    int captured            = 100;
    auto lam                = [captured](int x) { return x + captured; };
    FunctionRef<int(int)> f = lam;
    KILN_CHECK(bool(f));
    KILN_CHECK_EQ(f(5), 105);
}

KILN_TEST(Core, FunctionRefFromUserPair) {
    Pair p{1, 2};
    FunctionRef<int(int)> f(&pair_sum_thunk, &p);
    KILN_CHECK(bool(f));
    KILN_CHECK_EQ(f(10), 13);
}

KILN_TEST(Core, FunctionRefBoolConversion) {
    FunctionRef<int(int)> f;
    KILN_CHECK(!bool(f));
    FunctionRef<int(int)> g = nullptr;
    KILN_CHECK(!bool(g));
    auto lam                = [](int x) { return x * 2; };
    FunctionRef<int(int)> h = lam;
    KILN_CHECK(bool(h));
    KILN_CHECK_EQ(h(21), 42);
}

KILN_TEST_DEPRECATED_END

static_assert(align_up(u32(5), u32(8)) == u32(8));
static_assert(align_up(u32(8), u32(8)) == u32(8));
static_assert(align_up(u64(17), u64(16)) == u64(32));
static_assert(is_aligned(u32(16), u32(8)));
static_assert(!is_aligned(u32(15), u32(8)));
static_assert(is_pow2(u32(16)));
static_assert(is_pow2(u32(1)));
static_assert(!is_pow2(u32(15)));
static_assert(!is_pow2(u32(0)));
static_assert(min(3, 5) == 3);
static_assert(max(3, 5) == 5);
static_assert(min(5, 3) == 3);
static_assert(max(5, 3) == 5);
static_assert(clamp(10, 0, 5) == 5);
static_assert(clamp(-10, 0, 5) == 0);
static_assert(clamp(3, 0, 5) == 3);

KILN_TEST(Core, AlignAndMinMaxClampRuntime) {
    KILN_CHECK_EQ(align_up(usize(5), usize(8)), usize(8));
    KILN_CHECK(is_aligned(usize(16), usize(8)));
    KILN_CHECK(is_pow2(usize(64)));
    KILN_CHECK_EQ(min(3, 7), 3);
    KILN_CHECK_EQ(max(3, 7), 7);
    KILN_CHECK_EQ(clamp(100, 0, 10), 10);
}

KILN_TEST(Core, ReadWriteUnaligned) {
    alignas(1) u8 buf[9] = {};
    u64 v                = 0x0123456789abcdefull;
    write_unaligned(buf + 1, v); // offset 1: guaranteed unaligned for u64
    u64 back = read_unaligned<u64>(buf + 1);
    KILN_CHECK_EQ(back, v);

    u32 v2 = 0xdeadbeefu;
    write_unaligned(buf + 3, v2);
    u32 back2 = read_unaligned<u32>(buf + 3);
    KILN_CHECK_EQ(back2, v2);
}

KILN_TEST(Core, Countof) {
    int arr[7] = {};
    KILN_CHECK_EQ(countof(arr), usize(7));
}

} // namespace

KILN_TEST(Core, AssumeAndSizeLiterals) {
    int calls = 0;
    auto side = [&] {
        ++calls;
        return true;
    };
    int x = 5;
    KILN_ASSUME(x > 0);
    (void)side; // KILN_ASSUME must never evaluate its argument at runtime; keep `side` out of it.
    KILN_CHECK_EQ(calls, 0);
    Span<int const> none;
    KILN_CHECK_EQ(none.size, 0uz);
    KILN_CHECK_EQ(sizeof(u32), 4uz);
}
