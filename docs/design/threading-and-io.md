# Threading and IO

**Status:** Proposed (awaiting owner sign-off). Implemented in M3: `include/kiln/io.h`,
`src/io/`, `src/runtime/` (thread rules at the top of `runtime_internal.h`), and for the cooker
`src/cook/parallel.cpp` (`cook-kernels.md`). Details are recorded as R5a-R5l in
`../open-questions.md`.
**Decides:** Which threading primitives kiln uses, the job and IO interfaces, and where results
surface.

## Decision

### Threading primitives

- `<thread>`, `<mutex>`, `<atomic>` and `<condition_variable>` in `.cpp` files only
  (open-questions A3). `<atomic>` is also on the lightweight list for public headers.
- No own OS wrapper for threads.
- OS calls that std lacks (positional reads, directory listing, atomic rename, `mkdir`, thread
  names and priorities) sit inline in the `.cpp` that needs them: `src/io/compat_io.cpp`,
  `src/io/thread_pool.cpp`, `src/cook/store.cpp`, `src/cook/provider.cpp`,
  `tools/kiln-cook/main.cpp`. A shared private OS wrapper comes with native file watchers.

### Job system

`JobSystem { submit(user, fn, arg); wait_idle(user); user; }`, with `wait_idle` optional.

- Passed in `ContextDesc::jobs`. Null means the built-in pool (`create_thread_pool`), sized by
  `ContextDesc::workerThreads`; 0 means `hardware_concurrency - 1`, clamped to [1, 16]. Its queue
  holds `queueCapacity` jobs (default 4096); `submit` blocks when it is full.
- kiln never assumes jobs run in order or on a specific thread.
- kiln tracks its own in-flight job count. `destroy()` waits for it to reach 0 (1 ms sleeps); it
  does not call `wait_idle`.
- **Worker priority.** `ThreadPoolDesc::priority` (`Normal`, `Low`, `High`) is the host's hint for
  where kiln's workers sit relative to its render, net and game threads. The built-in pool applies
  it in each worker at start-up: `SetThreadPriority` below or above normal on Windows, a per-thread
  nice of +5 or -5 on Linux (raising needs privileges; failure is ignored), nothing elsewhere yet.
  `ContextDesc::workerPriority` passes it to the built-in pool. A host `JobSystem` decides its own
  thread priorities.

### Where results surface

- `pump()` is the **only** place where state changes become visible, events are produced,
  diagnostics from workers reach the `DiagSink`, and adapter completion is polled.
- Workers post completions to a mutex-guarded queue; `pump()` drains it (R5a).
- No user callback fires on a worker thread, except `Allocator`, `LogSink`, the `IoBackend`, the
  cook provider's `cook()`, and the adapter's `begin_upload` / `commit_upload` (`adapter.md`). All
  of these must be thread-safe. `acquire` runs on the thread that calls `request_*()`.

### Runtime state ownership

- Registry state (slots, id maps, queues, groups, events, placeholders, stats) is read and mutated
  only on the pump thread: the thread that calls `request_*`, `release`, `pump()`, `wait()` and
  the queries. kiln does not lock it.
- A worker runs exactly one stage of one asset at a time. While a job is in flight:
  - the worker reads only the slot's job-input fields, which the pump thread wrote before
    submitting and does not write while the job runs;
  - the worker writes only the slot's job-output fields, which the pump thread does not touch
    until it has popped the job's completion;
  - the worker never reads fields the pump thread may change meanwhile (state, refcount,
    generation, zombie flag, priority, queue links, group).
- The completion queue's mutex orders the worker's writes before the pump thread's reads. The
  job's last access to the context is the in-flight counter decrement.
- A slot released while a job is in flight becomes a zombie; it returns to the free list only when
  the pump thread pops the job's completion (R5b).
- A worker captures the first diagnostic it produces into the slot; `pump()` emits it inside the
  single K5xxx diagnostic for the failed load (R5f).
- Steady state: `pump()` and the queries never allocate. Every runtime table is sized at
  `create()`. Per-load buffers are allocated on workers (`Tag::Payload` / `Tag::Io`) and freed on
  the pump thread.

### Worker stages

One job runs one stage of one asset. `pump()` dispatches at most `ContextDesc::maxIoJobs` jobs at
once (0 = worker count), `High` queues before `Normal` (R5c).

- **Meta**: open the source (store file, registered bytes, or cook-on-miss), read and validate the
  metadata (`.mesh` CPU region or KTX2 prefix), compute the texture upload layout, close the source.
- **Upload**: `begin_upload`, reopen the source, read or decode the payload straight into the
  adapter's destination, `commit_upload` (R5d). `PumpOptions::uploadBytes` caps the bytes started
  per pump; at least one upload starts per pump.

### Cook provider

- `install_provider` / `uninstall_provider` run on the pump thread.
- `cook()` runs on a worker, possibly concurrently for different assets. A published provider is
  never mutated: `cook()` reads only its own locals and the provider's read-only fields.
- The context-to-provider registry is touched only by install and uninstall, under a mutex.
- The cooker splits its heavy passes with `parallel_for` over the context's job system. The
  calling worker runs chunks itself, so a cook inside a worker cannot deadlock the pool
  (`cook-kernels.md`).

### `wait()` on a load group

- `wait()` runs on the pump thread only (`handles-and-states.md`). It loops: check whether the group
  is settled or the timeout expired, `pump()`, then a 1 ms `std::this_thread::sleep_for`. The
  deadline uses `std::chrono::steady_clock`.
- The sleep yields the core to workers doing IO and decode. A pure spin would compete with them.
- `wait()` adds no threads and no queues. Workers see nothing special apart from the raised
  priority of the group members.
- `create()` uses the same kind of loop to spin on placeholder uploads when the adapter is
  self-submitting (`adapter.md`).

### `.mesh` payload load (blob table)

One upload job per mesh (mesh-format-spec §5.9, §7):

- **`kPayloadRaw` set (the only output of the v0.5 cooker):** one `read_range(gpuDataOffset,
  payloadDecodedSize)` directly into the adapter destination. No scratch buffer, no per-byte work.
  The `BLOB` table is only validated.
- **`kPayloadRaw` clear:** read `GPUD` into a `Tag::Io` scratch buffer, then `mesh::decode_payload`
  zero-fills the destination and decodes blob by blob. Any codec or filter other than `None` fails
  the asset with `Code::Unsupported` (K4015).
- Proposed for when compressed codecs land (v0.6-0.7): one job per blob (or small batch of blobs),
  a per-worker reused scratch arena, the job owning the preceding range zero-fills each gap, and
  the last job to finish commits or reports the failure.

### IO backend

`IoBackend { open, size, read_range, close, user }` over an opaque `IoFile`.

- The compat backend (`compat_io_backend()`) is thread-safe and reads positionally (`pread`, or
  `ReadFile` with an offset). Reads block and run on worker threads, possibly concurrently for one
  file.
- Reads always target **caller-provided memory**, so the runtime reads straight into adapter
  staging.
- `ContextDesc` holds one `IoBackend const* io` (null = compat) and one `storeDir`. The loader reads
  `<storeDir>/<name>.mesh|.ktx2` (a named mount's `m:` prefix becomes the directory `m#/`).
  `ContextDesc::mounts` (`mounts(ctx)`) is the cook provider's named source roots (`struct Mount {
  StrView name; StrView root; }`); a pack-file backend would use the same mount table.

### Path to true async IO

When native async backends land (Win32 overlapped / IoRing, io_uring, dispatch IO):

- `read_range` gains a completion token and returns at once.
- A new `poll(user, Span<u64> completed)` call is added, drained by an IO thread or by `pump()`.
- The compat backend then implements `read_range` + `poll` on top of its worker threads.
- Only the backend boundary changes, not the load pipeline.

## Rationale

- std threading is portable across all target compilers and stays in `.cpp` files, so its
  compile-time cost stays out of public headers.
- A function-pointer job interface lets hosts plug kiln into their own scheduler.
- Blocking reads on workers are the simplest correct implementation and shape the API first.

## Alternatives considered

An own thread wrapper (more code, no benefit until a platform lacks std threads) and async
`read_range` from day one (a more complex compat backend before the API is proven): rejected.

## Consequences / what this constrains later

- The IO interface changes shape once (blocking to token + poll). Hosts with their own
  `IoBackend` need a migration note in `CHANGELOG.md`.
- Everything the runtime does on workers must be safe with a host job system that runs jobs on
  arbitrary threads.
- With per-blob jobs, several workers write into one adapter destination at once. The destination
  must tolerate concurrent writes to disjoint ranges (plain mapped memory does).
- `wait()` blocks the pump thread by design. Long waits stall the host frame loop.

## Open points for the owner

- Confirm std threading in `.cpp` files (A3).
- Should the async IO interface change happen before v0.5 is tagged, to avoid breaking the external
  project later? Proposed: no, keep blocking for v0.5.
- Confirm the `wait()` sleep (1 ms between pumps) over a condition variable signalled by workers.
  A condition variable would not help: completion also depends on the adapter's
  `is_upload_complete`, which must be polled.
- Confirm one job per blob for the decode loop once codecs land (vs one job per mesh, as now).
