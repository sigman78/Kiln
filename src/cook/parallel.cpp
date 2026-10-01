// src/cook/parallel.cpp — helping parallel_for (parallel.h).
//
// Lifetime: a submitted helper may start only after the call has returned (every
// chunk was already claimed and this helper was still queued). Waiting for every
// helper to start would deadlock when the caller occupies the only worker, so the call
// state is a heap block shared by reference count: the caller holds one reference and
// each submitted helper holds one. A helper's last access is its release; whoever
// drops the count to zero frees the block. A late helper finds no chunk left,
// releases, and exits, touching only memory its reference keeps alive.
#include "parallel.h"

#include <atomic>
#include <thread>

namespace kiln::cook {

namespace {

constexpr u32 kMaxHelpers = 64; ///< jobs submitted per call, at most

struct Call {
    Allocator const* alloc = nullptr;
    ParallelFn fn          = nullptr;
    void* user             = nullptr;
    u32 count              = 0;
    u32 grain              = 0;
    u32 chunks             = 0;
    std::atomic<u32> next{0};     ///< next chunk to claim; may run past `chunks`
    std::atomic<u32> finished{0}; ///< chunks completed
    std::atomic<u32> refs{0};     ///< the caller + every submitted helper not yet released
};

void run_chunks(Call& c) {
    u32 done = 0;
    for (;;) {
        u32 const i = c.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= c.chunks) break;
        u32 const begin = i * c.grain; // < count, so no overflow
        u32 const end   = c.count - begin > c.grain ? begin + c.grain : c.count;
        c.fn(c.user, begin, end);
        ++done;
    }
    if (done) c.finished.fetch_add(done, std::memory_order_release);
}

/// Drops one reference; the last one frees the block. Nothing may touch `c` after.
void release(Call* c) {
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    std::atomic_thread_fence(std::memory_order_acquire);
    Allocator const* const alloc = c->alloc;
    delete_object(alloc, c, Tag::Jobs);
}

void helper_job(void* arg) {
    auto* const c = static_cast<Call*>(arg);
    run_chunks(*c);
    release(c); // the helper's last access
}

} // namespace

void parallel_for(JobSystem const* jobs, Allocator const* alloc, u32 count, u32 grain, ParallelFn fn,
                  void* user, u32 maxThreads) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    u32 const chunks = count / grain + (count % grain != 0 ? 1u : 0u);
    if (!jobs || !jobs->submit || chunks <= 1 || maxThreads == 1) {
        fn(user, 0, count);
        return;
    }

    if (!alloc) alloc = default_allocator();
    void* const mem = try_alloc(alloc, sizeof(Call), alignof(Call), Tag::Jobs);
    if (!mem) { // no state for helpers: still correct, just not split
        fn(user, 0, count);
        return;
    }
    static u32 const hwThreads = max(1u, std::thread::hardware_concurrency());
    if (maxThreads > hwThreads) maxThreads = hwThreads;
    u32 const helpers = min(chunks - 1, min(kMaxHelpers, maxThreads ? maxThreads - 1 : kMaxHelpers));
    auto* const c     = ::new (mem) Call;
    c->alloc          = alloc;
    c->fn             = fn;
    c->user           = user;
    c->count          = count;
    c->grain          = grain;
    c->chunks         = chunks;
    c->refs.store(helpers + 1, std::memory_order_relaxed); // published by submit()

    for (u32 i = 0; i < helpers; ++i)
        jobs->submit(jobs->user, &helper_job, c);

    run_chunks(*c);
    while (c->finished.load(std::memory_order_acquire) != chunks)
        std::this_thread::yield(); // chunks claimed by helpers are still running
    release(c);
}

} // namespace kiln::cook
