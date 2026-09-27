// kiln/io.h — IO backend and job system interfaces (docs/design/threading-and-io.md).
// Both are structs of function pointers plus a user pointer; kiln ships a
// compatibility file backend (C/POSIX, blocking reads run on workers) and a small
// built-in thread pool as defaults.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln {

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

/// Submit work to be run on some worker thread. Callbacks never touch host state:
/// completion is delivered through pump() only. `wait_idle` is optional.
struct JobSystem {
    void (*submit)(void* user, void (*fn)(void* arg), void* arg) = nullptr;
    void (*wait_idle)(void* user)                                = nullptr;
    void* user                                                   = nullptr;
};

struct ThreadPoolDesc {
    Allocator const* alloc = nullptr; ///< nullptr = default allocator
    u32 threads            = 0;       ///< 0 = hardware_concurrency - 1, clamped to [1, 16]
    u32 queueCapacity      = 4096;    ///< max queued jobs; submit blocks when full
};

/// The built-in pool. `destroy` waits for idle, joins the threads and frees the pool.
[[nodiscard]] KILN_API Result<JobSystem> create_thread_pool(ThreadPoolDesc const& desc) noexcept;
KILN_API void destroy_thread_pool(JobSystem const& jobs) noexcept;
[[nodiscard]] KILN_API u32 thread_pool_thread_count(JobSystem const& jobs) noexcept;

// ---------------------------------------------------------------------------
// IO
// ---------------------------------------------------------------------------

struct IoFile {
    u64 bits = 0; ///< backend-defined; 0 = invalid
    [[nodiscard]] constexpr bool valid() const noexcept { return bits != 0; }
};

/// Range-based reads into caller memory. `read_range` is blocking in v0.5 and is
/// called from worker threads, possibly concurrently for the same file, so it must
/// be positional (pread / ReadFile with an offset), never seek-then-read on shared
/// state. A true async backend (io_uring, IoRing, overlapped) will add a completion
/// token and a poll (v0.9); the range-based contract stays.
struct IoBackend {
    Status (*open)(void* user, StrView path, IoFile* out)                       = nullptr;
    Status (*size)(void* user, IoFile f, u64* out)                              = nullptr;
    Status (*read_range)(void* user, IoFile f, u64 offset, u64 size, void* dst) = nullptr;
    void (*close)(void* user, IoFile f)                                         = nullptr;
    void* user                                                                  = nullptr;
};

/// The built-in compatibility backend (thread-safe, positional reads, UTF-8 paths).
[[nodiscard]] KILN_API IoBackend const* compat_io_backend() noexcept;

/// Convenience: read a whole file through a backend into `out` (Tag::Io).
KILN_API Status io_read_file(IoBackend const* io, StrView path, Allocator const* alloc,
                             Vec<u8>* out) noexcept;

/// True if `path` names an existing regular file (through the compat backend's rules).
[[nodiscard]] KILN_API bool io_file_exists(StrView path) noexcept;

} // namespace kiln
