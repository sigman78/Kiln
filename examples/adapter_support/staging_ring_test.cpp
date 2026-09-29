// examples/adapter_support/staging_ring_test.cpp — StagingRing: order, wrap-around, out-of-order
// release, exhaustion, and a seeded stress run that checks held ranges never overlap.
#include "kiln_test.h"
#include "staging_ring.h"

using namespace kiln;
using kiln::ex::StagingRing;

KILN_TEST(StagingRing, ReservesInOrderAndWraps) {
    StagingRing ring(nullptr, 100, 8);
    StagingRing::Reservation const a = ring.reserve(30, 1);
    StagingRing::Reservation const b = ring.reserve(30, 1);
    StagingRing::Reservation const c = ring.reserve(30, 1);
    KILN_REQUIRE(a.ok() && b.ok() && c.ok());
    KILN_CHECK_EQ(a.offset, u64(0));
    KILN_CHECK_EQ(b.offset, u64(30));
    KILN_CHECK_EQ(c.offset, u64(60));
    KILN_CHECK(!ring.reserve(30, 1).ok()); // 10 bytes at the top, nothing below yet: Busy
    ring.release(a.id);
    StagingRing::Reservation const d = ring.reserve(30, 1);
    KILN_REQUIRE(d.ok());
    KILN_CHECK_EQ(d.offset, u64(0)); // wrapped into the space `a` gave back
    KILN_CHECK_EQ(ring.used(), u64(100));
}

KILN_TEST(StagingRing, OutOfOrderReleaseReclaimsInOrder) {
    StagingRing ring(nullptr, 90, 8);
    StagingRing::Reservation const a = ring.reserve(30, 1);
    StagingRing::Reservation const b = ring.reserve(30, 1);
    StagingRing::Reservation const c = ring.reserve(30, 1);
    ring.release(b.id); // b completed first; a still holds the front
    KILN_CHECK_EQ(ring.held(), 3u);
    KILN_CHECK(!ring.reserve(30, 1).ok());
    ring.release(a.id); // now a and b both return
    KILN_CHECK_EQ(ring.held(), 1u);
    KILN_CHECK_EQ(ring.used(), u64(30));
    StagingRing::Reservation const d = ring.reserve(60, 1);
    KILN_REQUIRE(d.ok());
    KILN_CHECK_EQ(d.offset, u64(0));
    ring.release(c.id);
    ring.release(d.id);
    KILN_CHECK_EQ(ring.held(), 0u);
    KILN_CHECK_EQ(ring.used(), u64(0));
}

KILN_TEST(StagingRing, AlignmentAndLimits) {
    StagingRing ring(nullptr, 256, 2);
    StagingRing::Reservation const a = ring.reserve(10, 1);
    StagingRing::Reservation const b = ring.reserve(10, 64);
    KILN_REQUIRE(a.ok() && b.ok());
    KILN_CHECK_EQ(b.offset, u64(64));
    KILN_CHECK(!ring.reserve(1, 1).ok()); // two reservations at most
    ring.release(a.id);
    ring.release(b.id);
    KILN_CHECK(ring.can_fit(256));
    KILN_CHECK(!ring.can_fit(257));
    KILN_CHECK(!ring.reserve(257, 1).ok()); // never fits; the adapter says Unsupported
    StagingRing::Reservation const z = ring.reserve(0, 16);
    KILN_REQUIRE(z.ok());
    KILN_CHECK_EQ(ring.used(), u64(1)); // a zero size takes one byte
    ring.release(z.id);
    StagingRing::Reservation const whole = ring.reserve(256, 256);
    KILN_CHECK(whole.ok() && whole.offset == 0);
}

// Random sizes, alignments and release order; every held range stays inside the ring and apart
// from the others, and releasing everything empties the ring.
KILN_TEST(StagingRing, StressHeldRangesNeverOverlap) {
    constexpr u64 kSize = 4096;
    constexpr u32 kMax  = 32;
    StagingRing ring(nullptr, kSize, kMax);
    struct Held {
        u32 id;
        u64 begin, end;
    };
    Held held[kMax];
    u32 n    = 0;
    u64 seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&seed](u64 m) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return (seed >> 33) % m;
    };
    u32 reserved = 0, busy = 0;
    for (int step = 0; step < 20000; ++step) {
        if (n > 0 && (n == kMax || rnd(3) == 0)) {
            u32 const k = u32(rnd(n)); // any order
            ring.release(held[k].id);
            held[k] = held[--n];
            continue;
        }
        u64 const size                   = 1 + rnd(900);
        u64 const align                  = u64(1) << rnd(9);
        StagingRing::Reservation const r = ring.reserve(size, align);
        if (!r.ok()) {
            ++busy;
            continue;
        }
        ++reserved;
        KILN_REQUIRE(r.offset % align == 0 && r.offset + size <= kSize);
        for (u32 i = 0; i < n; ++i)
            KILN_REQUIRE(r.offset + size <= held[i].begin || held[i].end <= r.offset);
        held[n++] = {r.id, r.offset, r.offset + size};
    }
    KILN_CHECK(reserved > 1000 && busy > 0); // both paths ran
    while (n)
        ring.release(held[--n].id);
    KILN_CHECK_EQ(ring.held(), 0u);
    KILN_CHECK_EQ(ring.used(), u64(0));
}
