// kiln/io.h — IO backend and job system interfaces, with built-in defaults (compat
// file backend, small thread pool). See docs/design/threading-and-io.md.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln {

/// Runs jobs on worker threads. Jobs never touch host state; completion reaches the
/// host only through pump(). `wait_idle` is optional.
struct JobSystem {
    void (*submit)(void* user, void (*fn)(void* arg), void* arg) = nullptr;
    void (*wait_idle)(void* user)                                = nullptr;
    void* user                                                   = nullptr;
};

/// Scheduling hint for worker threads relative to the host's own threads. Loading and
/// cooking belong below render, net and game threads, so Low is the usual choice.
enum class ThreadPriority : u8 { Normal = 0, Low, High };

struct ThreadPoolDesc {
    Allocator const* alloc  = nullptr;                ///< nullptr = default allocator
    u32 threads             = 0;                      ///< 0 = hardware_concurrency - 1, clamped to [1, 16]
    u32 queueCapacity       = 4096;                   ///< max queued jobs; submit blocks when full
    ThreadPriority priority = ThreadPriority::Normal; ///< accepted; the built-in pool does not apply it yet
};

/// The built-in pool. `destroy` waits for idle, joins the threads and frees the pool.
[[nodiscard]] KILN_API Result<JobSystem> create_thread_pool(ThreadPoolDesc const& desc) noexcept;
KILN_API void destroy_thread_pool(JobSystem const& jobs) noexcept;
[[nodiscard]] KILN_API u32 thread_pool_thread_count(JobSystem const& jobs) noexcept;

struct IoFile {
    u64 bits = 0; ///< backend-defined; 0 = invalid
    [[nodiscard]] constexpr bool valid() const noexcept { return bits != 0; }
};

/// Range-based reads into caller memory. `read_range` blocks, runs on worker threads
/// and may run concurrently for one file, so it must read positionally (pread /
/// ReadFile with an offset), never seek-then-read on shared state.
struct IoBackend {
    Status (*open)(void* user, StrView path, IoFile* out)                       = nullptr;
    Status (*size)(void* user, IoFile f, u64* out)                              = nullptr;
    Status (*read_range)(void* user, IoFile f, u64 offset, u64 size, void* dst) = nullptr;
    void (*close)(void* user, IoFile f)                                         = nullptr;
    void* user                                                                  = nullptr;
};

/// The built-in compatibility backend (thread-safe, positional reads, UTF-8 paths).
[[nodiscard]] KILN_API IoBackend const* compat_io_backend() noexcept;

/// Reads a whole file through `io` into `out` (Tag::Io).
KILN_API Status io_read_file(IoBackend const* io, StrView path, Allocator const* alloc,
                             Vec<u8>* out) noexcept;

/// True if `path` names an existing regular file (through the compat backend's rules).
[[nodiscard]] KILN_API bool io_file_exists(StrView path) noexcept;

} // namespace kiln
