# Threading and IO

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** Which threading primitives kiln uses, the job and IO interfaces, and where results surface.

## Decision

### Threading primitives

- `<thread>`, `<mutex>`, `<atomic>` and `<condition_variable>` are allowed in `.cpp` files only
  (HANDOFF §13 Q3). `<atomic>` is already on the lightweight list for public headers.
- No own OS wrapper for threads.
- A thin OS wrapper (`src/core/os.h`, private) covers only what std lacks or does badly:
  file IO with `pread`-style offsets, directory listing, file modification times and sizes,
  atomic rename (store writes), and later native file watchers.

### Job system

```cpp
struct JobSystem {
    void (*submit)(void* user, void (*fn)(void* arg), void* arg);
    void (*wait_idle)(void* user);   // optional; may be null
    void* user;
};
```

- Passed at context creation. Null means the built-in pool (worker count defaults to
  `hardware_concurrency - 1`, at least 1).
- kiln never assumes jobs run in order or on a specific thread.
- `wait_idle` is used only by `destroy()` and tests. If null, kiln tracks its own in-flight count
  and spins with a yield.

### Where results surface

- `pump()` is the **only** place where state changes become visible, events are produced,
  diagnostics from workers are delivered to the `DiagSink`, and adapter completion is polled.
- Workers push results into short-critical-section queues. `pump()` drains them.
- No user callback ever fires on a worker thread. Exceptions: `Allocator`, `LogSink`, and the
  adapter's `begin_upload` / `commit_upload` (see `adapter.md`), which must be thread-safe.

### IO backend

```cpp
struct IoFile { u64 bits; };   // opaque

struct IoBackend {
    Status (*open)(void* user, StrView path, IoFile* out);
    Status (*size)(void* user, IoFile f, u64* out);
    Status (*read_range)(void* user, IoFile f, u64 offset, u64 size, void* dst);  // blocking in v0.5
    void   (*close)(void* user, IoFile f);
    void*  user;
};
```

- v0.5 ships one "compat" backend: C stdio / POSIX (`pread` where available). Reads are blocking
  and called from worker threads.
- Reads always target **caller-provided memory**, so the runtime can read straight into adapter
  staging.
- **Mountable roots:** the context holds an ordered list of `{prefix, IoBackend, rootPath}`.
  Lookups try mounts in order. v0.5 mounts directories only. Pack files would be another backend.

### Path to true async IO

When native async backends land (Win32 overlapped / IoRing, io_uring, dispatch IO):

- `read_range` gains a completion token: `Status (*read_range)(..., void* dst, u64* outToken)`
  returns immediately.
- A new `poll(user, Span<u64> completed)` call is added, drained by an IO thread or by `pump()`.
- The compat backend then implements `read_range` + `poll` on top of its worker threads.
- The scheduler already treats a read as "submit, then complete later", so only the backend
  boundary changes, not the load pipeline.

## Rationale

- std threading is portable across all three target compilers and is confined to `.cpp` files,
  so compile-time cost stays out of public headers.
- A function-pointer job interface lets hosts plug kiln into their own scheduler.
- Blocking reads on workers are the simplest correct implementation and shape the API first
  (HANDOFF §2, OS-dependent code).

## Alternatives considered

- **Own thread wrapper**: more code, no benefit until a platform lacks std threads.
- **Async `read_range` from day one**: more complex compat backend before the API is proven.

## Consequences / what this constrains later

- The IO interface changes shape once (blocking to token + poll). Hosts that implemented their
  own `IoBackend` will need a migration note in `CHANGELOG.md`.
- Everything the runtime does on workers must be safe with a host job system that runs jobs
  on arbitrary threads.

## Open points for the owner

- Confirm std threading in `.cpp` files (Q3).
- Should the async IO interface change happen before v0.5 is tagged, to avoid breaking the external
  project later? Proposed: no, keep blocking for v0.5.
