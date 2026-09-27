// tests/test_parallel.cpp — cook-internal parallel_for (src/cook/parallel.h).
#include "kiln_test.h"

#include "../src/cook/parallel.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace kiln;
using namespace kiln::cook;

namespace {

constexpr u32 kMaxItems = 5000;

/// Counts how often each item ran and flags chunks that break the grain contract.
struct Coverage {
    std::atomic<u32> hits[kMaxItems];
    std::atomic<u32> badChunks{0};
    u32 count = 0;
    u32 grain = 0;

    void reset(u32 n, u32 g) noexcept {
        for (auto& h : hits)
            h.store(0, std::memory_order_relaxed);
        badChunks.store(0, std::memory_order_relaxed);
        count = n;
        grain = g;
    }

    static void fn(void* user, u32 begin, u32 end) noexcept {
        auto* self       = static_cast<Coverage*>(user);
        u32 const g      = self->grain ? self->grain : 1;
        bool const whole = begin < end && end <= self->count && (end - begin <= g || begin == 0);
        if (!whole) self->badChunks.fetch_add(1, std::memory_order_relaxed);
        for (u32 i = begin; i < end && i < kMaxItems; ++i)
            self->hits[i].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool exactly_once() const noexcept {
        for (u32 i = 0; i < kMaxItems; ++i)
            if (hits[i].load(std::memory_order_relaxed) != (i < count ? 1u : 0u)) return false;
        return badChunks.load(std::memory_order_relaxed) == 0;
    }
};

bool covers(JobSystem const* jobs, Coverage& cov, u32 count, u32 grain) {
    cov.reset(count, grain);
    parallel_for(jobs, nullptr, count, grain, &Coverage::fn, &cov);
    return cov.exactly_once();
}

void check_pool(u32 threads) {
    Result<JobSystem> pool = create_thread_pool({.threads = threads});
    KILN_REQUIRE(pool.ok());
    auto* cov = new_object<Coverage>(default_allocator(), Tag::Test);
    KILN_CHECK(covers(&*pool, *cov, 4096, 16));
    KILN_CHECK(covers(&*pool, *cov, 4097, 16)); // short last chunk
    KILN_CHECK(covers(&*pool, *cov, 1000, 1));  // more chunks than helpers
    KILN_CHECK(covers(&*pool, *cov, 3, 1));
    KILN_CHECK(covers(&*pool, *cov, 0, 8));
    KILN_CHECK(covers(&*pool, *cov, 5, 8)); // count < grain: inline
    for (u32 i = 0; i < 200; ++i)           // back-to-back calls while late helpers linger
        if (!KILN_CHECK(covers(&*pool, *cov, 64 + i, 4))) break;
    delete_object(default_allocator(), cov, Tag::Test);
    destroy_thread_pool(*pool);
}

/// A parallel_for inside a job of the same pool.
struct Nested {
    JobSystem const* jobs = nullptr;
    Coverage* cov         = nullptr;
    std::atomic<bool> done{false};

    static void job(void* arg) {
        auto* self = static_cast<Nested*>(arg);
        parallel_for(self->jobs, nullptr, self->cov->count, self->cov->grain, &Coverage::fn, self->cov);
        self->done.store(true, std::memory_order_release);
    }
};

} // namespace

KILN_TEST(parallel, inline_without_jobs) {
    auto* cov = new_object<Coverage>(default_allocator(), Tag::Test);
    KILN_CHECK(covers(nullptr, *cov, 4096, 16));
    KILN_CHECK(covers(nullptr, *cov, 0, 16));
    KILN_CHECK(covers(nullptr, *cov, 7, 16));
    KILN_CHECK(covers(nullptr, *cov, 7, 0)); // grain 0 is treated as 1
    delete_object(default_allocator(), cov, Tag::Test);
}

KILN_TEST(parallel, pool_1_thread) { check_pool(1); }
KILN_TEST(parallel, pool_2_threads) { check_pool(2); }
KILN_TEST(parallel, pool_8_threads) { check_pool(8); }

KILN_TEST(parallel, nested_in_job) {
    constexpr u32 kThreads[] = {1, 2};
    for (u32 threads : kThreads) {
        Result<JobSystem> pool = create_thread_pool({.threads = threads});
        KILN_REQUIRE(pool.ok());
        auto* cov = new_object<Coverage>(default_allocator(), Tag::Test);
        cov->reset(3000, 7);
        Nested n;
        n.jobs = &*pool;
        n.cov  = cov;
        pool->submit(pool->user, &Nested::job, &n);
        pool->wait_idle(pool->user); // also drains helpers that started after the call returned
        KILN_CHECK(n.done.load(std::memory_order_acquire));
        KILN_CHECK(cov->exactly_once());
        delete_object(default_allocator(), cov, Tag::Test);
        destroy_thread_pool(*pool);
    }
}

// Call state is freed by whoever releases it last, possibly a helper that starts after
// the call returned; once the pool is drained and destroyed nothing may remain.
KILN_TEST(parallel, call_state_freed) {
    u64 const before       = default_alloc_stats(Tag::Jobs).bytesCurrent;
    Result<JobSystem> pool = create_thread_pool({.threads = 8});
    KILN_REQUIRE(pool.ok());
    auto* cov = new_object<Coverage>(default_allocator(), Tag::Test);
    for (u32 i = 0; i < 200; ++i)
        if (!KILN_CHECK(covers(&*pool, *cov, 64 + i, 4))) break;
    delete_object(default_allocator(), cov, Tag::Test);
    destroy_thread_pool(*pool);
    KILN_CHECK_EQ(default_alloc_stats(Tag::Jobs).bytesCurrent, before);
}

/// Counts how many chunks run at once, so the thread budget can be observed.
struct Peak {
    std::atomic<u32> active{0};
    std::atomic<u32> peak{0};

    static void fn(void* user, u32, u32) noexcept {
        auto* self  = static_cast<Peak*>(user);
        u32 const a = self->active.fetch_add(1, std::memory_order_acq_rel) + 1;
        u32 p       = self->peak.load(std::memory_order_relaxed);
        while (p < a && !self->peak.compare_exchange_weak(p, a, std::memory_order_relaxed)) {
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        self->active.fetch_sub(1, std::memory_order_acq_rel);
    }
};

KILN_TEST(parallel, max_threads_caps_helpers) {
    Result<JobSystem> pool = create_thread_pool({.threads = 8});
    KILN_REQUIRE(pool.ok());
    Peak two;
    parallel_for(&*pool, nullptr, 512, 1, &Peak::fn, &two, 2);
    KILN_CHECK(two.peak.load() <= 2u);
    Peak one;
    parallel_for(&*pool, nullptr, 512, 1, &Peak::fn, &one, 1); // inline
    KILN_CHECK_EQ(one.peak.load(), 1u);
    Peak many;
    parallel_for(&*pool, nullptr, 512, 1, &Peak::fn, &many, 0);
    KILN_CHECK(many.peak.load() <= 9u);
    destroy_thread_pool(*pool);
}
