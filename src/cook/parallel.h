// src/cook/parallel.h — helping parallel_for over a JobSystem. Internal to kiln_cook,
// never installed. Design: docs/design/cook-kernels.md ("Splitting work").
#pragma once

#include "kiln/io.h"

namespace kiln::cook {

using ParallelFn = void (*)(void* user, u32 begin, u32 end) noexcept;

/// Runs `fn(user, begin, end)` over [0, count) in chunks of `grain` items (the last
/// chunk may be shorter) and returns when every chunk has finished. Each chunk runs
/// exactly once, on the calling thread or on a worker of `jobs`; chunks may run
/// concurrently and in any order, so `fn` must not depend on either.
///
/// The calling thread claims and runs chunks itself, so the call always makes progress:
/// it cannot deadlock when `jobs` is null, when the pool has one thread or is busy, or
/// when the caller is itself a job of `jobs`. Runs inline with no submission when
/// `jobs` is null, there is only one chunk, or the call state cannot be allocated.
///
/// With helpers, one small block of call state comes from `alloc` (null = default
/// allocator, Tag::Jobs); the last helper to leave frees it, possibly after the call
/// has returned, so `alloc` must outlive the jobs submitted to `jobs`.
void parallel_for(JobSystem const* jobs, Allocator const* alloc, u32 count, u32 grain, ParallelFn fn,
                  void* user) noexcept;

} // namespace kiln::cook
