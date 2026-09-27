#include "kiln_test.h"

#include "kiln/alloc.h"

#include <cstdint>
#include <cstring>

using namespace kiln;

namespace {

struct Counted {
    static int ctorCount;
    static int dtorCount;
    int value;
    explicit Counted(int v) noexcept : value(v) { ++ctorCount; }
    ~Counted() noexcept { ++dtorCount; }
};
int Counted::ctorCount = 0;
int Counted::dtorCount = 0;

bool is_aligned_ptr(void const* p, usize align) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & (align - 1)) == 0;
}

// ---------------------------------------------------------------------------
// default_allocator: alloc/free + alignment
// ---------------------------------------------------------------------------

KILN_TEST(Alloc, DefaultAllocatorRawAlignment) {
    Allocator const* a = default_allocator();
    void* p            = a->alloc(a->user, 100, 64, Tag::Test);
    KILN_REQUIRE(p != nullptr);
    KILN_CHECK(is_aligned_ptr(p, 64));
    a->free(a->user, p, 100, 64, Tag::Test);
}

KILN_TEST(Alloc, ConvenienceAllocFreeVariousAlignments) {
    Allocator const* a = default_allocator();
    usize aligns[]     = {16, 32, 64, 128, 256};
    for (usize align : aligns) {
        void* p = alloc(a, 37, align, Tag::Test);
        KILN_REQUIRE(p != nullptr);
        KILN_CHECK(is_aligned_ptr(p, align));
        free(a, p, 37, align, Tag::Test);
    }
}

// ---------------------------------------------------------------------------
// default_alloc_stats deltas
// ---------------------------------------------------------------------------

KILN_TEST(Alloc, StatsBytesCurrentDelta) {
    Allocator const* a = default_allocator();
    AllocStats before  = default_alloc_stats(Tag::Test);
    void* p            = alloc(a, 256, 16, Tag::Test);
    AllocStats mid     = default_alloc_stats(Tag::Test);
    KILN_CHECK_EQ(mid.bytesCurrent, before.bytesCurrent + u64(256));
    KILN_CHECK_EQ(mid.allocCount, before.allocCount + u64(1));
    free(a, p, 256, 16, Tag::Test);
    AllocStats after = default_alloc_stats(Tag::Test);
    KILN_CHECK_EQ(after.bytesCurrent, before.bytesCurrent);
    KILN_CHECK_EQ(after.freeCount, before.freeCount + u64(1));
    KILN_CHECK_EQ(after.allocCount, before.allocCount + u64(1));
}

KILN_TEST(Alloc, StatsMultipleAllocations) {
    Allocator const* a = default_allocator();
    AllocStats before  = default_alloc_stats(Tag::Test);
    void* p1           = alloc(a, 64, 16, Tag::Test);
    void* p2           = alloc(a, 128, 16, Tag::Test);
    AllocStats mid     = default_alloc_stats(Tag::Test);
    KILN_CHECK_EQ(mid.bytesCurrent, before.bytesCurrent + u64(64 + 128));
    KILN_CHECK_EQ(mid.allocCount, before.allocCount + u64(2));
    free(a, p1, 64, 16, Tag::Test);
    free(a, p2, 128, 16, Tag::Test);
    AllocStats after = default_alloc_stats(Tag::Test);
    KILN_CHECK_EQ(after.bytesCurrent, before.bytesCurrent);
    KILN_CHECK_EQ(after.freeCount, before.freeCount + u64(2));
}

// ---------------------------------------------------------------------------
// new_object / delete_object
// ---------------------------------------------------------------------------

KILN_TEST(Alloc, NewDeleteObjectConstructsAndDestroys) {
    int beforeCtor = Counted::ctorCount;
    int beforeDtor = Counted::dtorCount;
    Counted* c     = new_object<Counted>(default_allocator(), Tag::Test, 42);
    KILN_REQUIRE(c != nullptr);
    KILN_CHECK_EQ(c->value, 42);
    KILN_CHECK_EQ(Counted::ctorCount, beforeCtor + 1);
    KILN_CHECK_EQ(Counted::dtorCount, beforeDtor);
    delete_object(default_allocator(), c, Tag::Test);
    KILN_CHECK_EQ(Counted::dtorCount, beforeDtor + 1);
}

KILN_TEST(Alloc, DeleteObjectNullIsNoOp) {
    int beforeDtor = Counted::dtorCount;
    Counted* c     = nullptr;
    delete_object(default_allocator(), c, Tag::Test);
    KILN_CHECK_EQ(Counted::dtorCount, beforeDtor);
}

// ---------------------------------------------------------------------------
// alloc_array / free_array
// ---------------------------------------------------------------------------

KILN_TEST(Alloc, ArrayAllocFree) {
    usize const n = 10;
    int* arr      = alloc_array<int>(default_allocator(), n, Tag::Test);
    KILN_REQUIRE(arr != nullptr);
    for (usize i = 0; i < n; ++i)
        arr[i] = int(i);
    bool ok = true;
    for (usize i = 0; i < n; ++i)
        if (arr[i] != int(i)) ok = false;
    KILN_CHECK(ok);
    free_array(default_allocator(), arr, n, Tag::Test);
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

KILN_TEST(Alloc, ArenaAllocAlignment) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    void* p1 = arena.alloc(10, 64);
    KILN_REQUIRE(p1 != nullptr);
    KILN_CHECK(is_aligned_ptr(p1, 64));
    void* p2 = arena.alloc(3, 16);
    KILN_REQUIRE(p2 != nullptr);
    KILN_CHECK(is_aligned_ptr(p2, 16));
}

KILN_TEST(Alloc, ArenaZeroSizeAllocIsNonNull) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    void* p = arena.alloc(0, 8);
    KILN_CHECK(p != nullptr);
}

KILN_TEST(Alloc, ArenaManySmallAllocsSpanBlocks) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    constexpr usize kCount = 100;
    constexpr usize kSize  = 64;
    void* ptrs[kCount];

    for (usize i = 0; i < kCount; ++i) {
        ptrs[i] = arena.alloc(kSize, 8);
        KILN_REQUIRE(ptrs[i] != nullptr);
        std::memset(ptrs[i], int(u8(i)), kSize);
    }
    // Should have needed more than one backing block.
    KILN_CHECK(arena.bytes_reserved() > usize(1024));

    // Contents must not have been clobbered by later allocations (no overlap).
    bool contentsOk = true;
    for (usize i = 0; i < kCount; ++i) {
        u8 const* b = static_cast<u8 const*>(ptrs[i]);
        for (usize j = 0; j < kSize; ++j)
            if (b[j] != u8(i)) contentsOk = false;
    }
    KILN_CHECK(contentsOk);

    // All pointers distinct (no overlap).
    bool distinct = true;
    for (usize i = 0; i < kCount; ++i)
        for (usize j = i + 1; j < kCount; ++j)
            if (ptrs[i] == ptrs[j]) distinct = false;
    KILN_CHECK(distinct);
}

KILN_TEST(Alloc, ArenaCopyNullTerminates) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    StrView sv = arena.copy("hello"_sv);
    KILN_REQUIRE_EQ(sv.size, usize(5));
    KILN_CHECK(sv == "hello"_sv);
    KILN_CHECK_EQ(sv.data[5], '\0');
}

KILN_TEST(Alloc, ArenaCopyEmptyString) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    StrView sv = arena.copy(""_sv);
    KILN_CHECK_EQ(sv.size, usize(0));
    KILN_CHECK_EQ(sv.data[0], '\0');
}

KILN_TEST(Alloc, ArenaResetKeepsOneBlockAndZeroesUsed) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    void* p0 = arena.alloc(800, 8);
    void* p1 = arena.alloc(800, 8); // should require a second backing block
    KILN_CHECK(p0 != nullptr && p1 != nullptr);
    KILN_CHECK(arena.bytes_reserved() >= usize(2 * 1024));

    arena.reset();
    KILN_CHECK(arena.bytes_reserved() >= usize(1024));
    KILN_CHECK_EQ(arena.bytes_used(), usize(0));

    // Still usable after reset.
    void* p = arena.alloc(16, 8);
    KILN_CHECK(p != nullptr);
}

KILN_TEST(Alloc, ArenaReleaseZeroesBoth) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    void* p = arena.alloc(100, 8);
    KILN_CHECK(p != nullptr);
    KILN_CHECK(arena.bytes_reserved() > usize(0));
    arena.release();
    KILN_CHECK_EQ(arena.bytes_used(), usize(0));
    KILN_CHECK_EQ(arena.bytes_reserved(), usize(0));
}

KILN_TEST(Alloc, ArenaAsAllocatorWorksWithAlloc) {
    Arena arena({.blockSize = 1024, .tag = Tag::Test});
    Allocator a = arena.as_allocator();
    void* p     = alloc(&a, 64, 16, Tag::Test);
    KILN_REQUIRE(p != nullptr);
    KILN_CHECK(is_aligned_ptr(p, 16));
    std::memset(p, 0xAB, 64);
    // free() on an arena-backed allocator is a no-op; must not crash.
    free(&a, p, 64, 16, Tag::Test);
    KILN_CHECK(arena.bytes_used() > usize(0));
}

KILN_TEST(Alloc, ArenaMoveLeavesSourceEmpty) {
    Arena src({.blockSize = 1024, .tag = Tag::Test});
    void* p = src.alloc(100, 8);
    KILN_CHECK(p != nullptr);
    usize usedBefore     = src.bytes_used();
    usize reservedBefore = src.bytes_reserved();
    KILN_REQUIRE(usedBefore > 0);

    Arena dst(std::move(src));
    KILN_CHECK_EQ(dst.bytes_used(), usedBefore);
    KILN_CHECK_EQ(dst.bytes_reserved(), reservedBefore);
    KILN_CHECK_EQ(src.bytes_used(), usize(0));
    KILN_CHECK_EQ(src.bytes_reserved(), usize(0));

    // Moved-from arena is still safely usable (starts fresh).
    void* p2 = src.alloc(16, 8);
    KILN_CHECK(p2 != nullptr);
}

} // namespace
