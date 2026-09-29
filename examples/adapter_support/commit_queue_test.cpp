// examples/adapter_support/commit_queue_test.cpp — CommitQueue: order, retries, and pushes from
// several threads while the pump side takes.
#include "commit_queue.h"
#include "kiln_test.h"

#include <atomic>
#include <thread>

using namespace kiln;
using kiln::ex::CommitQueue;

KILN_TEST(CommitQueue, TakesInPushOrderAndEmpties) {
    CommitQueue q(nullptr, 8);
    KILN_CHECK(q.take().empty());
    q.push(3);
    q.push(1);
    q.push(2);
    Span<u32 const> const t = q.take();
    KILN_REQUIRE_EQ(t.size, usize(3));
    KILN_CHECK(t[0] == 3 && t[1] == 1 && t[2] == 2);
    KILN_CHECK(q.take().empty());
}

KILN_TEST(CommitQueue, RetryLandsInTheNextTake) {
    CommitQueue q(nullptr, 4);
    q.push(0);
    q.push(1);
    Span<u32 const> const first = q.take();
    KILN_REQUIRE_EQ(first.size, usize(2));
    q.push(first[1]);            // flush could not place it: again next time
    q.push(2);                   // committed meanwhile
    KILN_CHECK_EQ(first[0], 0u); // the taken span is untouched by the pushes
    Span<u32 const> const second = q.take();
    KILN_REQUIRE_EQ(second.size, usize(2));
    KILN_CHECK(second[0] == 1 && second[1] == 2);
}

// Four workers commit 2500 uploads each while the pump side takes; every index arrives once.
KILN_TEST(CommitQueue, ConcurrentPushesArriveOnce) {
    constexpr u32 kThreads = 4, kPer = 2500, kTotal = kThreads * kPer;
    CommitQueue q(nullptr, kTotal);
    Vec<u8> seen(default_allocator(), Tag::Test);
    seen.resize(kTotal, 0);
    std::atomic<u32> done{0};
    std::thread workers[kThreads];
    for (u32 w = 0; w < kThreads; ++w)
        workers[w] = std::thread([&q, &done, w] {
            for (u32 i = 0; i < kPer; ++i)
                q.push(w * kPer + i);
            done.fetch_add(1);
        });
    u32 received = 0;
    for (;;) {
        bool const last = done.load() == kThreads; // read before take(): nothing can follow it
        for (u32 i : q.take()) {
            KILN_REQUIRE(i < kTotal && seen[i] == 0);
            seen[i] = 1;
            ++received;
        }
        if (last) break;
    }
    for (std::thread& t : workers)
        t.join();
    KILN_CHECK_EQ(received, kTotal);
}
