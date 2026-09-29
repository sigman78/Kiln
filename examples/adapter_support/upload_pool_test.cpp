// examples/adapter_support/upload_pool_test.cpp — UploadPool: tokens, staleness, exhaustion and
// the state machine.
#include "kiln_test.h"
#include "upload_pool.h"

using namespace kiln;
using kiln::ex::UploadPool;
using kiln::ex::UploadState;

namespace {
struct Data {
    u64 offset = 0;
    u32 object = kInvalid;
};
} // namespace

KILN_TEST(UploadPool, TokensGoStaleOnRelease) {
    UploadPool<Data> pool(nullptr, 2);
    u32 const a = pool.acquire();
    KILN_REQUIRE(a != kInvalid);
    KILN_CHECK(pool.state(a) == UploadState::Writing);
    pool[a].offset = 42;
    u64 const t    = pool.token(a);
    KILN_CHECK_NE(t, u64(0));
    KILN_CHECK_EQ(pool.index_of(t), a);
    pool.advance(a, UploadState::Committed);
    pool.advance(a, UploadState::Complete);
    pool.release(a);
    KILN_CHECK_EQ(pool.index_of(t), kInvalid); // released: the old token finds nothing
    u32 const b = pool.acquire();              // the same record again, reset
    KILN_CHECK_EQ(b, a);
    KILN_CHECK_EQ(pool[b].offset, u64(0));
    KILN_CHECK_EQ(pool.index_of(t), kInvalid); // a new generation
    KILN_CHECK_EQ(pool.index_of(pool.token(b)), b);
    KILN_CHECK_EQ(pool.index_of(0), kInvalid);
    KILN_CHECK_EQ(pool.index_of(u64(7) << 32 | 99), kInvalid);
}

KILN_TEST(UploadPool, ExhaustionAndCounts) {
    UploadPool<Data> pool(nullptr, 2);
    u32 const a = pool.acquire();
    u32 const b = pool.acquire();
    KILN_CHECK(a != kInvalid && b != kInvalid && a != b);
    KILN_CHECK_EQ(pool.acquire(), kInvalid); // Busy until one comes back
    KILN_CHECK_EQ(pool.in_use(), 2u);
    pool.advance(b, UploadState::Committed);
    pool.advance(b, UploadState::InFlight);
    pool.advance(b, UploadState::Failed);
    pool.release(b);
    KILN_CHECK_EQ(pool.in_use(), 1u);
    KILN_CHECK(pool.acquire() != kInvalid);
}

KILN_TEST(UploadPool, DiscardFreesAWritingRecord) {
    UploadPool<Data> pool(nullptr, 1);
    u32 const a = pool.acquire();
    u64 const t = pool.token(a);
    pool.discard(a); // kiln's write failed: never committed
    KILN_CHECK_EQ(pool.index_of(t), kInvalid);
    KILN_CHECK_EQ(pool.in_use(), 0u);
    KILN_CHECK(pool.acquire() != kInvalid);
}

KILN_TEST(UploadPool, StateMachine) {
    using S = UploadState;
    KILN_CHECK(ex::can_advance(S::Writing, S::Committed));
    KILN_CHECK(!ex::can_advance(S::Writing, S::Complete)); // kiln always commits first
    KILN_CHECK(ex::can_advance(S::Committed, S::InFlight));
    KILN_CHECK(ex::can_advance(S::Committed, S::Complete)); // done in flush (sokol, NGA meshes)
    KILN_CHECK(ex::can_advance(S::Committed, S::Failed));
    KILN_CHECK(ex::can_advance(S::InFlight, S::Complete));
    KILN_CHECK(ex::can_advance(S::InFlight, S::Failed));
    KILN_CHECK(!ex::can_advance(S::Complete, S::Failed));
    KILN_CHECK(!ex::can_advance(S::Failed, S::Complete));
    KILN_CHECK(!ex::can_advance(S::Free, S::Writing));  // acquire() only
    KILN_CHECK(!ex::can_advance(S::Complete, S::Free)); // release() only
    KILN_CHECK(ex::status_of(S::Writing) == UploadStatus::Pending);
    KILN_CHECK(ex::status_of(S::Committed) == UploadStatus::Pending);
    KILN_CHECK(ex::status_of(S::InFlight) == UploadStatus::Pending);
    KILN_CHECK(ex::status_of(S::Complete) == UploadStatus::Complete);
    KILN_CHECK(ex::status_of(S::Failed) == UploadStatus::Failed);
}
