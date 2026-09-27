# Threading and IO

**Status:** Proposed (awaiting owner sign-off)
**Implementation:** M3 (2026-09-27), see open-questions R5+ for deviations.
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
  adapter's `begin_upload` / `commit_upload` (see `adapter.md`), which must be thread-safe. The
  adapter's `acquire` runs on the thread that calls `request()`; `publish` runs only on the pump
  thread.

### `wait()` on a load group

- `wait(ctx, g, { .timeoutMs })` runs on the pump thread only (see `handles-and-states.md`). It
  loops: `pump()`, check whether the group is settled or the timeout expired, then a short sleep.
- The sleep is `std::this_thread::sleep_for` (about 1 ms), in the `.cpp` only. No OS wrapper is
  needed. The deadline uses `std::chrono::steady_clock`.
- The sleep yields the core to workers doing IO and decode. A pure spin would compete with them.
- `wait()` adds no threads and no queues. Workers see nothing special apart from the boosted
  priority of the group members.
- `create()` uses the same loop, without a group, to spin on placeholder uploads when the adapter
  is self-submitting (`adapter.md`).

### `.mesh` payload load (blob table)

The payload path follows mesh-format-spec §5.9 and §7. Both paths run on workers.

**`kPayloadRaw` set (the only output of the v0.5 cooker):**

- One `read_range(gpuDataOffset, gpuDataSize)` directly into the adapter destination (staging /
  ReBAR). No scratch buffer, no per-byte CPU work.
- The `BLOB` table is only validated (ranges, identity layout), not iterated for decoding.

**`kPayloadRaw` clear (the blob decode loop, implemented in v0.5 with codec `None`):**

- One job per blob (or per small batch of blobs). The order does not matter; blobs write disjoint
  decoded ranges.
- Each job reads the encoded range into a worker-local scratch arena, decodes (outer Zstd, then
  codec, then unfilter) and writes exactly `decodedSize` bytes at `decodedOffset` in the adapter
  destination. For codec `None` with no filter the read goes straight into the destination and the
  scratch step is skipped.
- Gaps between decoded ranges (alignment padding) are **zero-filled** in the destination by the
  job that owns the preceding range, or by the first job for a gap at offset 0.
- A job that fails validation or decoding marks the load failed. The last job to finish calls
  `commit_upload`, or reports the failure; the asset moves to `Failed` on the next `pump()`.
- Unsupported codecs or filters (anything but `None` in v0.5) fail the asset with
  `Code::Unsupported` before any read.
- The scratch arena is per worker and reused, so steady-state loads do not allocate.

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
- **As implemented (M3):** there is no mount list. `ContextDesc` holds exactly one `IoBackend const*
  io` (null = the compat backend) for the whole store, plus one `storeDir` root the loader
  concatenates with the asset path (`<storeDir>/<assetPath>.mesh|.ktx2`); `sourceRoots` is a
  separate, cook-provider-only list of directories searched for *source* files (`.glb`/`.png`), not
  an `IoBackend` mount table. The `{prefix, IoBackend, rootPath}` ordered-mount design below did not
  land in M3; a pack-file backend would need it added later.

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
- Blob jobs write into adapter memory from several workers at once. The adapter's destination must
  tolerate concurrent writes to disjoint ranges (plain mapped memory does).
- `wait()` blocks the pump thread by design. It is for small groups only; long waits stall the host
  frame loop.

## Open points for the owner

- Confirm std threading in `.cpp` files (Q3).
- Should the async IO interface change happen before v0.5 is tagged, to avoid breaking the external
  project later? Proposed: no, keep blocking for v0.5.
- Confirm the `wait()` sleep (about 1 ms `sleep_for` between pumps) over a condition variable
  signalled by workers. A condition variable would not help: completion also depends on the
  adapter's `is_upload_complete`, which must be polled.
- Confirm one job per blob for the decode loop (vs one job per mesh that iterates its blobs).
  Proposed: per blob, which is what split blobs are for; tiny blobs are batched.
