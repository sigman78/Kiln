#include "kiln_test.h"

#include "kiln/containers.h"

#include <utility>

using namespace kiln;

namespace {

// ---------------------------------------------------------------------------
// Live: non-trivial type with a static live-instance counter
// ---------------------------------------------------------------------------

struct Live {
    static int count;
    int v;
    explicit Live(int x = 0) noexcept : v(x) { ++count; }
    Live(Live const& o) noexcept : v(o.v) { ++count; }
    Live(Live&& o) noexcept : v(o.v) {
        ++count;
        o.v = -1;
    }
    ~Live() noexcept { --count; }
    Live& operator=(Live const& o) noexcept {
        v = o.v;
        return *this;
    }
    Live& operator=(Live&& o) noexcept {
        v   = o.v;
        o.v = -1;
        return *this;
    }
};
int Live::count = 0;

// ---------------------------------------------------------------------------
// MoveOnly: move-only type that owns an allocator-backed int, for Vec relocation
// ---------------------------------------------------------------------------

struct MoveOnly {
    static int count; // number of currently-live allocations
    int* ptr = nullptr;

    explicit MoveOnly(int v) noexcept {
        ptr  = alloc_array<int>(default_allocator(), 1, Tag::Test);
        *ptr = v;
        ++count;
    }
    MoveOnly(MoveOnly const&)            = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
    MoveOnly(MoveOnly&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    MoveOnly& operator=(MoveOnly&& o) noexcept {
        if (this != &o) {
            release_ptr();
            ptr   = o.ptr;
            o.ptr = nullptr;
        }
        return *this;
    }
    ~MoveOnly() noexcept { release_ptr(); }
    void release_ptr() noexcept {
        if (ptr) {
            free_array(default_allocator(), ptr, 1, Tag::Test);
            ptr = nullptr;
            --count;
        }
    }
    [[nodiscard]] int value() const noexcept { return ptr ? *ptr : -1; }
};
int MoveOnly::count = 0;

// ---------------------------------------------------------------------------
// A key type with a custom hash_of() found via ADL
// ---------------------------------------------------------------------------

namespace user_ns {
struct MyKey {
    u32 v;
};
[[nodiscard]] constexpr u64 hash_of(MyKey k) noexcept { return kiln::mix64(u64(k.v)); }
[[nodiscard]] constexpr bool operator==(MyKey a, MyKey b) noexcept { return a.v == b.v; }
} // namespace user_ns

// ---------------------------------------------------------------------------
// A deliberately bad hasher: forces heavy collisions to stress backward-shift erase
// ---------------------------------------------------------------------------

struct BadHash {
    [[nodiscard]] constexpr u64 operator()(u32 k) const noexcept { return u64(k % 4); }
};

} // namespace

// ===========================================================================
// FixedArray<T, N>
// ===========================================================================

KILN_TEST(Containers, FixedArrayPushEmplacePop) {
    FixedArray<int, 4> a;
    KILN_CHECK(a.empty());
    a.push_back(1);
    a.push_back(2);
    a.emplace_back(3);
    KILN_REQUIRE_EQ(a.size(), usize(3));
    KILN_CHECK_EQ(a[0], 1);
    KILN_CHECK_EQ(a[1], 2);
    KILN_CHECK_EQ(a[2], 3);
    a.pop_back();
    KILN_CHECK_EQ(a.size(), usize(2));
    KILN_CHECK_EQ(a.back(), 2);
}

KILN_TEST(Containers, FixedArrayFullAndTryPushBack) {
    FixedArray<int, 4> a;
    KILN_CHECK(a.try_push_back(1));
    KILN_CHECK(a.try_push_back(2));
    KILN_CHECK(a.try_push_back(3));
    KILN_CHECK(a.try_push_back(4));
    KILN_CHECK(a.full());
    KILN_CHECK(!a.try_push_back(5));
    KILN_CHECK_EQ(a.size(), usize(4));
}

KILN_TEST(Containers, FixedArrayEraseUnordered) {
    FixedArray<int, 4> a;
    a.push_back(10);
    a.push_back(20);
    a.push_back(30);
    a.erase_unordered(0); // swaps last (30) into slot 0
    KILN_REQUIRE_EQ(a.size(), usize(2));
    KILN_CHECK_EQ(a[0], 30);
    KILN_CHECK_EQ(a[1], 20);
}

KILN_TEST(Containers, FixedArrayResizeAndClear) {
    FixedArray<int, 4> a;
    a.resize(3);
    KILN_REQUIRE_EQ(a.size(), usize(3));
    KILN_CHECK_EQ(a[0], 0);
    KILN_CHECK_EQ(a[1], 0);
    KILN_CHECK_EQ(a[2], 0);
    a[0] = 5;
    a[1] = 6;
    a[2] = 7;
    a.resize(1);
    KILN_REQUIRE_EQ(a.size(), usize(1));
    KILN_CHECK_EQ(a[0], 5);
    a.clear();
    KILN_CHECK(a.empty());
    KILN_CHECK_EQ(a.size(), usize(0));
}

KILN_TEST(Containers, FixedArrayCopyAndMove) {
    FixedArray<int, 4> a;
    a.push_back(1);
    a.push_back(2);

    FixedArray<int, 4> b(a);
    KILN_REQUIRE_EQ(b.size(), usize(2));
    KILN_CHECK_EQ(b[0], 1);
    b[0] = 99;
    KILN_CHECK_EQ(a[0], 1); // independent copy

    FixedArray<int, 4> c(std::move(b));
    KILN_REQUIRE_EQ(c.size(), usize(2));
    KILN_CHECK_EQ(c[0], 99);
    KILN_CHECK_EQ(b.size(), usize(0)); // moved-from is empty
}

KILN_TEST(Containers, FixedArrayNonTrivialDestroys) {
    int before = Live::count;
    {
        FixedArray<Live, 4> a;
        a.emplace_back(1);
        a.emplace_back(2);
        a.emplace_back(3);
        KILN_CHECK_EQ(Live::count, before + 3);
        a.resize(1); // destroys 2 elements
        KILN_CHECK_EQ(Live::count, before + 1);
        a.clear();
        KILN_CHECK_EQ(Live::count, before);
        a.emplace_back(9);
        KILN_CHECK_EQ(Live::count, before + 1);
    }
    KILN_CHECK_EQ(Live::count, before);
}

// ===========================================================================
// Vec<T>
// ===========================================================================

KILN_TEST(Containers, VecDefaultThenInitPush1000) {
    Vec<int> v;
    v.init(default_allocator(), Tag::Test);
    for (int i = 0; i < 1000; ++i)
        v.push_back(i);
    KILN_REQUIRE_EQ(v.size(), usize(1000));
    bool ok = true;
    for (int i = 0; i < 1000; ++i)
        if (v[usize(i)] != i) ok = false;
    KILN_CHECK(ok);
    v.release();
}

KILN_TEST(Containers, VecReserveDoesNotChangeSize) {
    Vec<int> v(default_allocator(), Tag::Test);
    v.push_back(1);
    v.push_back(2);
    v.reserve(100);
    KILN_CHECK_EQ(v.size(), usize(2));
    KILN_CHECK(v.capacity() >= usize(100));
    v.release();
}

KILN_TEST(Containers, VecResizeGrowsWithZeroesAndShrinks) {
    Vec<int> v(default_allocator(), Tag::Test);
    v.push_back(7);
    v.resize(5);
    KILN_REQUIRE_EQ(v.size(), usize(5));
    KILN_CHECK_EQ(v[0], 7);
    KILN_CHECK_EQ(v[1], 0);
    KILN_CHECK_EQ(v[2], 0);
    KILN_CHECK_EQ(v[3], 0);
    KILN_CHECK_EQ(v[4], 0);
    v.resize(2);
    KILN_REQUIRE_EQ(v.size(), usize(2));
    KILN_CHECK_EQ(v[0], 7);
    KILN_CHECK_EQ(v[1], 0);
    v.release();
}

KILN_TEST(Containers, VecEraseOrdered) {
    Vec<int> v(default_allocator(), Tag::Test);
    for (int i = 0; i < 5; ++i)
        v.push_back(i);
    v.erase(1); // removes value 1, shifts the rest down
    KILN_REQUIRE_EQ(v.size(), usize(4));
    int expect[4] = {0, 2, 3, 4};
    bool ok       = true;
    for (usize i = 0; i < 4; ++i)
        if (v[i] != expect[i]) ok = false;
    KILN_CHECK(ok);
    v.release();
}

KILN_TEST(Containers, VecEraseUnordered) {
    Vec<int> v(default_allocator(), Tag::Test);
    for (int i = 0; i < 5; ++i)
        v.push_back(i);
    v.erase_unordered(1); // swaps last (4) into slot 1
    KILN_REQUIRE_EQ(v.size(), usize(4));
    KILN_CHECK_EQ(v[0], 0);
    KILN_CHECK_EQ(v[1], 4);
    KILN_CHECK_EQ(v[2], 2);
    KILN_CHECK_EQ(v[3], 3);
    v.release();
}

KILN_TEST(Containers, VecAppendSpan) {
    Vec<int> v(default_allocator(), Tag::Test);
    v.push_back(1);
    int extra[3] = {2, 3, 4};
    v.append({extra, usize(3)});
    KILN_REQUIRE_EQ(v.size(), usize(4));
    for (usize i = 0; i < 4; ++i)
        KILN_CHECK_EQ(v[i], int(i + 1));
    v.release();
}

KILN_TEST(Containers, VecAppendUninit) {
    Vec<int> v(default_allocator(), Tag::Test);
    v.push_back(10);
    Span<int> extra = v.append_uninit(3);
    KILN_REQUIRE_EQ(extra.size, usize(3));
    extra[0] = 1;
    extra[1] = 2;
    extra[2] = 3;
    KILN_REQUIRE_EQ(v.size(), usize(4));
    KILN_CHECK_EQ(v[0], 10);
    KILN_CHECK_EQ(v[1], 1);
    KILN_CHECK_EQ(v[2], 2);
    KILN_CHECK_EQ(v[3], 3);
    v.release();
}

KILN_TEST(Containers, VecCloneEqualsButDistinctPointer) {
    Vec<int> v(default_allocator(), Tag::Test);
    for (int i = 0; i < 10; ++i)
        v.push_back(i);
    Vec<int> c = v.clone();
    KILN_REQUIRE_EQ(c.size(), v.size());
    bool ok = true;
    for (usize i = 0; i < v.size(); ++i)
        if (c[i] != v[i]) ok = false;
    KILN_CHECK(ok);
    KILN_CHECK(c.data() != v.data());
    v.release();
    c.release();
}

KILN_TEST(Containers, VecMoveLeavesSourceEmpty) {
    Vec<int> v(default_allocator(), Tag::Test);
    v.push_back(1);
    v.push_back(2);
    Vec<int> m(std::move(v));
    KILN_CHECK_EQ(m.size(), usize(2));
    KILN_CHECK_EQ(v.size(), usize(0));
    KILN_CHECK(v.data() == nullptr);
    KILN_CHECK_EQ(v.capacity(), usize(0));
    m.release();
}

KILN_TEST(Containers, VecReleaseAndDtorReturnMemoryToBaseline) {
    u64 baseline = default_alloc_stats(Tag::Test).bytesCurrent;
    {
        Vec<int> v(default_allocator(), Tag::Test);
        for (int i = 0; i < 500; ++i)
            v.push_back(i);
        KILN_CHECK(default_alloc_stats(Tag::Test).bytesCurrent > baseline);
        v.release();
        KILN_CHECK_EQ(default_alloc_stats(Tag::Test).bytesCurrent, baseline);
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Test).bytesCurrent, baseline);
}

KILN_TEST(Containers, VecMoveOnlySurvivesGrowthRelocation) {
    int before = MoveOnly::count;
    {
        Vec<MoveOnly> v(default_allocator(), Tag::Test);
        for (int i = 0; i < 50; ++i)
            v.push_back(MoveOnly(i)); // forces several grow_to() relocations
        KILN_REQUIRE_EQ(v.size(), usize(50));
        KILN_CHECK_EQ(MoveOnly::count, before + 50);
        bool ok = true;
        for (int i = 0; i < 50; ++i)
            if (v[usize(i)].value() != i) ok = false;
        KILN_CHECK(ok);
        v.release();
        KILN_CHECK_EQ(MoveOnly::count, before);
    }
    KILN_CHECK_EQ(MoveOnly::count, before);
}

// ===========================================================================
// HashMap<K, V>
// ===========================================================================

KILN_TEST(Containers, HashMapInsertFindContainsErase) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    m.insert(1, 100);
    m.insert(2, 200);
    KILN_CHECK(m.contains(1));
    int* v = m.find(1);
    KILN_REQUIRE(v != nullptr);
    KILN_CHECK_EQ(*v, 100);
    KILN_CHECK(!m.contains(999));
    KILN_CHECK(m.find(999) == nullptr);
    KILN_CHECK(m.erase(1));
    KILN_CHECK(!m.contains(1));
    KILN_CHECK(!m.erase(1));
    KILN_CHECK_EQ(m.size(), usize(1));
}

KILN_TEST(Containers, HashMapTryEmplaceNoOverwriteOnDuplicate) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    auto r1 = m.try_emplace(5, 10);
    KILN_REQUIRE(r1.inserted);
    KILN_CHECK_EQ(*r1.value, 10);

    auto r2 = m.try_emplace(5, 999);
    KILN_CHECK(!r2.inserted);
    KILN_CHECK_EQ(*r2.value, 10); // unchanged
    KILN_CHECK_EQ(m.size(), usize(1));
}

KILN_TEST(Containers, HashMapInsertOverwrites) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    m.insert(5, 10);
    m.insert(5, 20);
    int* v = m.find(5);
    KILN_REQUIRE(v != nullptr);
    KILN_CHECK_EQ(*v, 20);
    KILN_CHECK_EQ(m.size(), usize(1));
}

KILN_TEST(Containers, HashMapTenThousandInsertsFindEraseEveryOther) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    constexpr u64 kN = 10000;
    for (u64 i = 0; i < kN; ++i)
        m.insert(i, int(i));
    KILN_REQUIRE_EQ(m.size(), usize(kN));

    bool allFound = true;
    for (u64 i = 0; i < kN; ++i) {
        int* v = m.find(i);
        if (!v || *v != int(i)) allFound = false;
    }
    KILN_CHECK(allFound);

    for (u64 i = 0; i < kN; i += 2)
        m.erase(i);
    KILN_REQUIRE_EQ(m.size(), usize(kN / 2));

    bool remainOk = true;
    for (u64 i = 0; i < kN; ++i) {
        bool shouldExist = (i % 2) != 0;
        if (m.contains(i) != shouldExist) remainOk = false;
    }
    KILN_CHECK(remainOk);
}

KILN_TEST(Containers, HashMapClearKeepsCapacity) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    for (u64 i = 0; i < 50; ++i)
        m.insert(i, int(i));
    usize capBefore = m.capacity();
    m.clear();
    KILN_CHECK_EQ(m.size(), usize(0));
    KILN_CHECK_EQ(m.capacity(), capBefore);
    m.insert(1, 42);
    KILN_REQUIRE(m.find(1) != nullptr);
    KILN_CHECK_EQ(*m.find(1), 42);
}

KILN_TEST(Containers, HashMapIterationVisitsEveryEntryExactlyOnce) {
    HashMap<u64, int> m(default_allocator(), Tag::Test);
    u64 expectedSum = 0;
    for (u64 i = 0; i < 200; ++i) {
        m.insert(i, int(i));
        expectedSum += i;
    }
    u64 sum    = 0;
    usize seen = 0;
    for (auto& e : m) {
        sum += e.key;
        ++seen;
    }
    KILN_CHECK_EQ(seen, usize(200));
    KILN_CHECK_EQ(sum, expectedSum);
}

KILN_TEST(Containers, HashMapKeyZeroHashesToZeroInternally) {
    // hash_of(u64) is mix64(v); mix64(0) == 0, so key 0 exercises the
    // "0 is the empty marker" remap inside HashMap::hash_key.
    KILN_CHECK_EQ(mix64(u64(0)), u64(0));

    HashMap<u64, int> m(default_allocator(), Tag::Test);
    m.insert(0, 111);
    for (u64 i = 1; i < 20; ++i)
        m.insert(i, int(i));

    KILN_REQUIRE(m.contains(0));
    KILN_CHECK_EQ(*m.find(0), 111);
    bool restOk = true;
    for (u64 i = 1; i < 20; ++i)
        if (!m.contains(i)) restOk = false;
    KILN_CHECK(restOk);
    KILN_CHECK(m.erase(0));
    KILN_CHECK(!m.contains(0));
    KILN_CHECK_EQ(m.size(), usize(19));
}

KILN_TEST(Containers, HashMapStrViewKeys) {
    HashMap<StrView, int> m(default_allocator(), Tag::Test);
    m.insert("alpha"_sv, 1);
    m.insert("beta"_sv, 2);
    m.insert("gamma"_sv, 3);
    KILN_REQUIRE(m.contains("beta"_sv));
    KILN_CHECK_EQ(*m.find("beta"_sv), 2);
    KILN_CHECK(m.erase("alpha"_sv));
    KILN_CHECK(!m.contains("alpha"_sv));
    KILN_CHECK_EQ(m.size(), usize(2));
}

KILN_TEST(Containers, HashMapCustomHashOfViaAdl) {
    HashMap<user_ns::MyKey, int> m(default_allocator(), Tag::Test);
    m.insert(user_ns::MyKey{1}, 10);
    m.insert(user_ns::MyKey{2}, 20);
    KILN_REQUIRE(m.contains(user_ns::MyKey{1}));
    KILN_CHECK_EQ(*m.find(user_ns::MyKey{1}), 10);
    KILN_CHECK(m.erase(user_ns::MyKey{2}));
    KILN_CHECK_EQ(m.size(), usize(1));
}

KILN_TEST(Containers, HashMapMoveSemantics) {
    HashMap<u64, int> a(default_allocator(), Tag::Test);
    a.insert(1, 10);
    a.insert(2, 20);

    HashMap<u64, int> b(std::move(a));
    KILN_CHECK_EQ(b.size(), usize(2));
    KILN_REQUIRE(b.find(1) != nullptr);
    KILN_CHECK_EQ(*b.find(1), 10);
    KILN_CHECK_EQ(a.size(), usize(0));
    KILN_CHECK_EQ(a.capacity(), usize(0));

    HashMap<u64, int> c(default_allocator(), Tag::Test);
    c.insert(9, 90);
    c = std::move(b);
    KILN_CHECK_EQ(c.size(), usize(2));
    KILN_REQUIRE(c.find(2) != nullptr);
    KILN_CHECK_EQ(*c.find(2), 20);
}

KILN_TEST(Containers, HashMapReleaseReturnsMemoryToBaseline) {
    u64 baseline = default_alloc_stats(Tag::Test).bytesCurrent;
    {
        HashMap<u64, int> m(default_allocator(), Tag::Test);
        for (u64 i = 0; i < 300; ++i)
            m.insert(i, int(i));
        KILN_CHECK(default_alloc_stats(Tag::Test).bytesCurrent > baseline);
        m.release();
        KILN_CHECK_EQ(default_alloc_stats(Tag::Test).bytesCurrent, baseline);
    }
    KILN_CHECK_EQ(default_alloc_stats(Tag::Test).bytesCurrent, baseline);
}

KILN_TEST(Containers, HashMapBackwardShiftDeletionStress) {
    HashMap<u32, u32, BadHash> m(default_allocator(), Tag::Test);
    constexpr u32 kN = 64;
    for (u32 i = 0; i < kN; ++i)
        m.insert(i, i * 10);
    KILN_REQUIRE_EQ(m.size(), usize(kN));

    // Deterministic scramble: (i * 37 + 5) mod 64 is a permutation of 0..63
    // since gcd(37, 64) == 1.
    u32 order[kN];
    for (u32 i = 0; i < kN; ++i)
        order[i] = (i * 37 + 5) % kN;

    bool erased[kN] = {};
    bool ok         = true;
    for (u32 idx = 0; idx < kN; ++idx) {
        u32 key = order[idx];
        if (!m.erase(key)) ok = false;
        erased[key] = true;

        for (u32 k = 0; k < kN; ++k) {
            bool shouldExist = !erased[k];
            bool exists      = m.contains(k);
            if (exists != shouldExist) {
                ok = false;
                continue;
            }
            if (exists) {
                u32* v = m.find(k);
                if (!v || *v != k * 10) ok = false;
            }
        }
    }
    KILN_CHECK(ok);
    KILN_CHECK_EQ(m.size(), usize(0));
}
