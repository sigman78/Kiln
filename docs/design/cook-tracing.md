# Cook tracing

**Status:** Follow-up task (owner, 2026-09-30). Not designed yet; this note records the request and
the constraints it must meet.
**Decides (when designed):** how kiln records time per asset through the cook pipeline, and how it
reports it: a summary in the log, or a Chrome trace file.

## Why

A cold start of `kiln-gl` took 8.7 s. Finding where the time went took temporary logging in four
files: the provider, the loader, the thread pool and the example. It turned out to be two separate
things: 2.4 s of BC encoding capped at 3 threads, and a 5 s driver stall on the pump thread. The
existing `CookStats` (per-stage times of one `cook_mesh` / `cook_texture` call, printed by
`kiln-cook -v`) shows neither the waits between stages nor the work on other threads.

## The request

- Lightweight and off by default. When it is off, the cost is one branch per recording point.
- Each item that enters the cook pipeline gets a trace key, for example the asset name and its
  build key. Each processing step records a keyed timestamp against it: queued, started and ended,
  and the thread it ran on.
- The steps are flexible. A stage records a named span, so no fixed enum of steps is needed.
- Two outputs:
  - a per-asset summary in the log, when enabled;
  - a Chrome trace file (Trace Event Format JSON, `"ph": "X"` spans per thread). It opens in
    `chrome://tracing` and Perfetto as a flame chart of the cook jobs.

## Constraints

- No allocation per event while tracing is on: use a fixed ring or per-thread buffers from an
  `Allocator` with a `Tag`, and count the events it drops.
- `kiln_runtime` never writes files. The trace file is written by `kiln_cook` / `kiln-cook`, or by
  the host through a sink callback (function pointer + `void* user`).
- Formatting uses `kiln::format`; no iostreams.
- Tracing never changes cooked bytes.

## Open points

1. Scope: only the cook, or the whole load of one asset? The 5 s stall of 2026-09-30 happened
   between `request` and dispatch, on the pump thread, so a cook-only trace would have missed it.
   The runtime stages to cover would be request, dispatch, meta job, prepare (cook on miss),
   upload and Ready.
2. How to turn it on: a `CookEnv` / `ContextDesc` field, a `kiln-cook --trace <file>` flag, an
   environment variable for the examples, or all three.
3. Whether `CookStats` becomes a view of the same events or stays as it is.
