// Built-in thread pool (docs/design/threading-and-io.md). Workers pop jobs from a
// bounded ring; submit() blocks while the ring is full.
#include "kiln/io.h"

#include "kiln/log.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(KILN_OS_LINUX)
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace kiln {

namespace {

struct Job {
    void (*fn)(void*);
    void* arg;
};

// The pool, its job ring and its worker array are allocated once from `alloc` with
// Tag::Jobs. submit, running jobs and wait_idle never allocate.
struct Pool {
    Allocator const* alloc = nullptr;

    Job* ring     = nullptr;
    u32 capacity  = 0;
    u32 head      = 0; ///< next slot to pop
    u32 tail      = 0; ///< next slot to push
    u32 count     = 0; ///< queued jobs
    u32 inFlight  = 0; ///< queued + currently running; 0 == idle
    bool stopping = false;

    std::thread* workers    = nullptr;
    u32 workerCount         = 0;
    ThreadPriority priority = ThreadPriority::Normal;

    std::mutex mutex;
    std::condition_variable notEmpty; ///< signalled when a job is queued or stopping begins
    std::condition_variable notFull;  ///< signalled when a slot frees up or stopping begins
    std::condition_variable idle;     ///< signalled when inFlight drops to 0
};

#if defined(KILN_OS_WINDOWS)
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

void set_worker_thread_name(u32 index) {
    static SetThreadDescriptionFn const fn = [] {
        HMODULE mod = GetModuleHandleW(L"kernel32.dll");
        return mod ? reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(mod, "SetThreadDescription"))
                   : SetThreadDescriptionFn(nullptr);
    }();
    if (!fn) return;

    char name[32];
    usize n = format(name, sizeof name, "kiln.worker%u", index);
    wchar_t wname[32];
    usize i = 0;
    for (; i < n && i + 1 < countof(wname); ++i)
        wname[i] = wchar_t(static_cast<unsigned char>(name[i]));
    wname[i] = L'\0';
    fn(GetCurrentThread(), wname);
}

void apply_worker_priority(ThreadPriority prio) {
    if (prio == ThreadPriority::Normal) return;
    (void)SetThreadPriority(GetCurrentThread(), prio == ThreadPriority::Low ? THREAD_PRIORITY_BELOW_NORMAL
                                                                            : THREAD_PRIORITY_ABOVE_NORMAL);
}
#elif defined(KILN_OS_LINUX)
void set_worker_thread_name(u32 index) {
    char name[16]; // Linux pthread name limit is 16 bytes including the NUL.
    format(name, sizeof name, "kiln.wrk%u", index);
    (void)pthread_setname_np(pthread_self(), name);
}

/// SCHED_OTHER has no per-thread priority, so the hint becomes a nice value for this
/// thread only. Raising above 0 needs privileges; a failure is ignored.
void apply_worker_priority(ThreadPriority prio) {
    if (prio == ThreadPriority::Normal) return;
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), prio == ThreadPriority::Low ? 5 : -5);
}
#else
void set_worker_thread_name(u32) {}
void apply_worker_priority(ThreadPriority) {}
#endif

u32 resolve_thread_count(u32 requested) {
    if (requested != 0) return requested;
    unsigned hwc = std::thread::hardware_concurrency();
    u32 n        = hwc > 1 ? u32(hwc - 1) : 1u;
    return clamp(n, u32(1), u32(16));
}

void worker_main(Pool* p, u32 index) {
    set_worker_thread_name(index);
    apply_worker_priority(p->priority);
    for (;;) {
        Job job{};
        {
            std::unique_lock<std::mutex> lock(p->mutex);
            p->notEmpty.wait(lock, [p] { return p->count > 0 || p->stopping; });
            if (p->count == 0) return; // stopping and fully drained
            job     = p->ring[p->head];
            p->head = (p->head + 1) % p->capacity;
            --p->count;
        }
        p->notFull.notify_one();

        job.fn(job.arg); // never holds the mutex while running a job

        {
            std::unique_lock<std::mutex> lock(p->mutex);
            KILN_ASSERT(p->inFlight > 0);
            if (--p->inFlight == 0) p->idle.notify_all();
        }
    }
}

void pool_submit(void* user, void (*fn)(void* arg), void* arg) {
    Pool* p = static_cast<Pool*>(user);
    std::unique_lock<std::mutex> lock(p->mutex);
    KILN_VERIFY(!p->stopping && "submit() after destroy_thread_pool()");
    p->notFull.wait(lock, [p] { return p->count < p->capacity || p->stopping; });
    KILN_VERIFY(!p->stopping && "submit() after destroy_thread_pool()");

    p->ring[p->tail] = Job{fn, arg};
    p->tail          = (p->tail + 1) % p->capacity;
    ++p->count;
    ++p->inFlight;
    lock.unlock();
    p->notEmpty.notify_one();
}

void pool_wait_idle(void* user) {
    Pool* p = static_cast<Pool*>(user);
    std::unique_lock<std::mutex> lock(p->mutex);
    p->idle.wait(lock, [p] { return p->inFlight == 0; });
}

} // namespace

Result<JobSystem> create_thread_pool(ThreadPoolDesc const& desc) {
    KILN_VERIFY(desc.queueCapacity > 0);
    Allocator const* a    = desc.alloc ? desc.alloc : default_allocator();
    u32 const threadCount = resolve_thread_count(desc.threads);

    Pool* p     = new_object<Pool>(a, Tag::Jobs);
    p->alloc    = a;
    p->capacity = desc.queueCapacity;
    p->priority = desc.priority;
    p->ring     = alloc_array<Job>(a, p->capacity, Tag::Jobs);
    p->workers  = alloc_array<std::thread>(a, threadCount, Tag::Jobs);

    u32 started = 0;
    bool ok     = true;
    for (u32 i = 0; i < threadCount; ++i) {
        // std::thread's constructor may throw on resource exhaustion. Third-party
        // throws are caught at the call site and converted to a Status.
#if KILN_HAS_EXCEPTIONS
        try {
            ::new (static_cast<void*>(&p->workers[i])) std::thread(&worker_main, p, i);
            started = i + 1;
        } catch (...) {
            ok = false;
            break;
        }
#else
        // Without exceptions a failed construction terminates. Native thread
        // creation (v0.9 OS wrappers) will return a Status instead.
        ::new (static_cast<void*>(&p->workers[i])) std::thread(&worker_main, p, i);
        started = i + 1;
#endif
    }

    if (!ok) {
        {
            std::unique_lock<std::mutex> lock(p->mutex);
            p->stopping = true;
        }
        p->notEmpty.notify_all();
        for (u32 i = 0; i < started; ++i) {
            p->workers[i].join();
            p->workers[i].~thread();
        }
        free_array(a, p->workers, threadCount, Tag::Jobs);
        free_array(a, p->ring, p->capacity, Tag::Jobs);
        delete_object(a, p, Tag::Jobs);
        return make_status(Code::Unknown);
    }

    p->workerCount = threadCount;

    JobSystem js;
    js.submit    = &pool_submit;
    js.wait_idle = &pool_wait_idle;
    js.user      = p;
    return js;
}

void destroy_thread_pool(JobSystem const& jobs) {
    if (!jobs.user) return;
    Pool* p = static_cast<Pool*>(jobs.user);

    {
        std::unique_lock<std::mutex> lock(p->mutex);
        p->stopping = true;
    }
    p->notEmpty.notify_all(); // wake workers so they can drain-then-exit
    p->notFull.notify_all();  // wake any submitter blocked on a full queue (misuse -> panic)

    for (u32 i = 0; i < p->workerCount; ++i)
        p->workers[i].join();
    for (u32 i = 0; i < p->workerCount; ++i)
        p->workers[i].~thread();

    Allocator const* a = p->alloc;
    free_array(a, p->workers, p->workerCount, Tag::Jobs);
    free_array(a, p->ring, p->capacity, Tag::Jobs);
    delete_object(a, p, Tag::Jobs);
}

u32 thread_pool_thread_count(JobSystem const& jobs) {
    KILN_VERIFY(jobs.user != nullptr);
    Pool const* p = static_cast<Pool const*>(jobs.user);
    return p->workerCount;
}

} // namespace kiln
